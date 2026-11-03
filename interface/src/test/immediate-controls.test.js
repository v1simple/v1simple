import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import Gps from '../routes/gps/+page.svelte';
import Obd from '../routes/obd/+page.svelte';
import Alp from '../routes/alp/+page.svelte';
import { retainDeviceSettings } from '../lib/stores/deviceSettings.svelte.js';
import { installFetchMock, jsonResponse, textResponse } from './fetch-mock.js';

const gps = { gpsEnabled: false, gpsBaud: 9600 };
const obd = { enabled: false, minRssi: -80, obdScanWindowMs: 15000, obdRetryIntervalMs: 120000,
    proxyOpenWindowMs: 60000, v1SettleQuietMs: 500, v1SettleFallbackMs: 1500, cycleTeardownAckTimeoutMs: 100 };
const alp = { alpEnabled: false, alpAlertPersistSec: 0, alpDisableV1LaserOnPush: false };
const resources = [
    { name: 'GPS', Page: Gps, url: '/api/gps/config', enabled: 'gpsEnabled', label: /Enable GPS/i, data: gps },
    { name: 'OBD', Page: Obd, url: '/api/obd/config', enabled: 'enabled', label: /Enable OBD/i, data: obd },
    { name: 'ALP', Page: Alp, url: '/api/device/settings', enabled: 'alpEnabled', label: /Enable ALP listener/i, data: alp }
];
const deferred = () => {
    let resolve;
    const promise = new Promise(done => { resolve = done; });
    return { promise, resolve };
};
async function settle(pending, response) {
    await act(async () => { pending.resolve(response); await pending.promise; });
    await act();
}
function install(resource, read, write = () => jsonResponse({ success: true })) {
    return installFetchMock([
        { method: 'GET', match: resource.url, respond: read },
        { method: 'POST', match: resource.url, respond: write },
        { method: 'GET', match: '/api/obd/devices', respond: jsonResponse({ devices: [] }) }
    ], ({ method, url }) => { throw new Error(`Unexpected ${method} ${url}`); });
}
function writes(fetchMock, url) {
    return fetchMock.mock.calls.filter(([path, init]) => path === url && init?.method === 'POST');
}

beforeEach(() => { vi.spyOn(console, 'error').mockImplementation(() => {}); });
afterEach(() => { cleanup(); vi.restoreAllMocks(); vi.unstubAllGlobals(); });

for (const resource of resources) {
    describe(`${resource.name} immediate configuration`, () => {
        it('admits false and zero values only after a complete initial read', async () => {
            install(resource, () => jsonResponse(resource.data));
            render(resource.Page);
            const toggle = await screen.findByRole('checkbox', { name: resource.label });
            expect(toggle).not.toBeChecked();
            expect(toggle).toBeEnabled();
            if (resource.name === 'ALP') expect(screen.getByLabelText('ALP display persistence (seconds)')).toHaveValue(0);
        });

        it.each(['unavailable', 'partial', 'wrong type'])('keeps a %s initial read unavailable until explicit Retry', async kind => {
            let reads = 0;
            const fetchMock = install(resource, () => {
                if (++reads > 1) return jsonResponse(resource.data);
                if (kind === 'unavailable') throw new Error('not reachable');
                if (kind === 'partial') return jsonResponse({ [resource.enabled]: false });
                return jsonResponse({ ...resource.data, [resource.enabled]: 'false' });
            });
            render(resource.Page);
            const retry = await screen.findByRole('button', { name: `Retry ${resource.name} settings` });
            expect(screen.queryByRole('checkbox', { name: resource.label })).not.toBeInTheDocument();
            expect(writes(fetchMock, resource.url)).toHaveLength(0);
            await fireEvent.click(retry);
            expect(await screen.findByRole('checkbox', { name: resource.label })).toBeEnabled();
            expect(reads).toBe(2);
        });

        for (const response of ['acknowledged', 'non-2xx', 'lost', 'unreadable body']) {
            it.each([true, false])(`reads authoritative state once after ${response}, valid readback=%s`, async valid => {
                let reads = 0;
                let stored = { ...resource.data };
                const fetchMock = install(resource, () => {
                    reads++;
                    return jsonResponse(reads > 1 && !valid ? {} : stored);
                }, () => {
                    stored = { ...stored, [resource.enabled]: true };
                    if (response === 'lost') throw new Error('response lost after commit');
                    if (response === 'non-2xx') return jsonResponse({ error: 'response rejected' }, 500);
                    if (response === 'unreadable body') return textResponse('not json', 200);
                    return jsonResponse({ success: true });
                });
                render(resource.Page);
                await fireEvent.click(await screen.findByRole('checkbox', { name: resource.label }));
                if (valid) {
                    await waitFor(() => expect(screen.getByRole('checkbox', { name: resource.label })).toBeEnabled());
                    expect(screen.getByRole('checkbox', { name: resource.label })).toBeChecked();
                    if (response === 'lost') expect(screen.getByText('Connection error.')).toBeInTheDocument();
                    if (response === 'non-2xx') expect(screen.getByText(`Failed to save ${resource.name} setting.`)).toBeInTheDocument();
                } else {
                    expect(await screen.findByRole('button', { name: `Retry ${resource.name} settings` })).toBeEnabled();
                    expect(screen.getByText(`Failed to load ${resource.name} settings.`)).toBeInTheDocument();
                    expect(screen.queryByRole('checkbox', { name: resource.label })).not.toBeInTheDocument();
                }
                expect(reads).toBe(2);
                const posts = writes(fetchMock, resource.url);
                expect(posts).toHaveLength(1);
                const fields = resource.name === 'ALP' ? Object.fromEntries(posts[0][1].body) : JSON.parse(posts[0][1].body);
                expect(fields).toEqual({ [resource.enabled]: resource.name === 'ALP' ? 'true' : true });
            });
        }

        it('blocks another write through the complete pending readback', async () => {
            const pending = deferred();
            let reads = 0;
            const fetchMock = install(resource, () => ++reads === 1 ? jsonResponse(resource.data) : pending.promise);
            render(resource.Page);
            const toggle = await screen.findByRole('checkbox', { name: resource.label });
            await fireEvent.click(toggle);
            await waitFor(() => expect(reads).toBe(2));
            expect(toggle).toBeDisabled();
            await fireEvent.click(toggle);
            expect(writes(fetchMock, resource.url)).toHaveLength(1);
            await settle(pending, jsonResponse(resource.data));
            await waitFor(() => expect(toggle).toBeEnabled());
            expect(toggle).not.toBeChecked();
        });

        it('does not let a retired initial read replace the remounted page', async () => {
            const pending = deferred();
            let reads = 0;
            install(resource, () => ++reads === 1 ? pending.promise : jsonResponse(resource.data));
            const first = render(resource.Page);
            await waitFor(() => expect(reads).toBe(1));
            first.unmount();
            render(resource.Page);
            expect(await screen.findByRole('checkbox', { name: resource.label })).not.toBeChecked();
            await settle(pending, jsonResponse({ ...resource.data, [resource.enabled]: true }));
            expect(screen.getByRole('checkbox', { name: resource.label })).not.toBeChecked();
            expect(reads).toBe(2);
        });

        it('finishes admitted readback after navigation within the existing reader lifetime', async () => {
            const pending = deferred();
            let reads = 0;
            const fetchMock = install(resource, () => { reads++; return jsonResponse(resource.data); }, () => pending.promise);
            const view = render(resource.Page);
            await fireEvent.click(await screen.findByRole('checkbox', { name: resource.label }));
            expect(writes(fetchMock, resource.url)).toHaveLength(1);
            view.unmount();
            await settle(pending, jsonResponse({ success: true }));
            // GPS/OBD own direct reads. ALP's released last consumer must not
            // create a new shared settings fetch solely for a retired page.
            expect(reads).toBe(resource.name === 'ALP' ? 1 : 2);
            expect(writes(fetchMock, resource.url)).toHaveLength(1);
        });
    });
}

it('keeps independent resources editable when the OBD first read fails', async () => {
    installFetchMock([
        { method: 'GET', match: '/api/gps/config', respond: jsonResponse(gps) },
        { method: 'GET', match: '/api/obd/config', respond: jsonResponse({}, 503) },
        { method: 'GET', match: '/api/obd/devices', respond: jsonResponse({ devices: [] }) },
        { method: 'GET', match: '/api/device/settings', respond: jsonResponse(alp) }
    ]);
    render(Gps); render(Obd); render(Alp);
    expect(await screen.findByRole('button', { name: 'Retry OBD settings' })).toBeEnabled();
    expect(await screen.findByRole('checkbox', { name: /Enable GPS/i })).toBeEnabled();
    expect(await screen.findByRole('checkbox', { name: /Enable ALP listener/i })).toBeEnabled();
});

it('invalidates an admitted ALP write after navigation when another consumer remains', async () => {
    const pending = deferred();
    let reads = 0;
    const fetchMock = install(resources[2], () => { reads++; return jsonResponse(alp); }, () => pending.promise);
    const release = retainDeviceSettings();
    const view = render(Alp);
    try {
        await fireEvent.click(await screen.findByRole('checkbox', { name: /Enable ALP listener/i }));
        expect(reads).toBe(1);
        view.unmount();
        await settle(pending, jsonResponse({ success: true }));
        expect(reads).toBe(2);
        expect(writes(fetchMock, '/api/device/settings')).toHaveLength(1);
    } finally {
        release();
    }
});

it.each([-1, 6, 0.5, '0', null])('ALP persistence rejects invalid initial value %s and recovers zero on Retry', async value => {
    let reads = 0;
    const fetchMock = install(resources[2], () => jsonResponse({ ...alp, alpAlertPersistSec: ++reads === 1 ? value : 0 }));
    render(Alp);
    await fireEvent.click(await screen.findByRole('button', { name: 'Retry ALP settings' }));
    expect(await screen.findByLabelText('ALP display persistence (seconds)')).toHaveValue(0);
    expect(writes(fetchMock, '/api/device/settings')).toHaveLength(0);
});

it('ALP persistence sends and accepts an explicit zero once', async () => {
    let stored = { ...alp, alpAlertPersistSec: 5 };
    const fetchMock = install(resources[2], () => jsonResponse(stored), ({ init }) => {
        stored = { ...stored, alpAlertPersistSec: Number(init.body.get('alpAlertPersistSec')) };
        return jsonResponse({ success: true });
    });
    render(Alp);
    const input = await screen.findByLabelText('ALP display persistence (seconds)');
    await fireEvent.input(input, { target: { value: '0' } });
    await fireEvent.change(input);
    expect(await screen.findByText('ALP setting saved.')).toBeInTheDocument();
    expect(input).toHaveValue(0);
    const posts = writes(fetchMock, '/api/device/settings');
    expect(posts).toHaveLength(1);
    expect(Object.fromEntries(posts[0][1].body)).toEqual({ alpAlertPersistSec: '0' });
});
