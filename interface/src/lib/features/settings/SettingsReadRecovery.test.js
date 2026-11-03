import { act, fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import Alp from '../../../routes/alp/+page.svelte';
import Gps from './SettingsGpsCard.svelte';
import Obd from './SettingsObdCard.svelte';
import { jsonResponse } from '../../../test/fetch-mock.js';

const OBD_CONFIG = {
    enabled: true, minRssi: -65, obdScanWindowMs: 10000, obdRetryIntervalMs: 90000,
    proxyOpenWindowMs: 45000, v1SettleQuietMs: 800, v1SettleFallbackMs: 2000,
    cycleTeardownAckTimeoutMs: 150
};
const CASES = [
    { name: 'ALP', Component: Alp, url: '/api/device/settings', label: /enable alp listener/i,
        field: 'alpEnabled', config: { alpEnabled: true, alpAlertPersistSec: 3, alpDisableV1LaserOnPush: false },
        invalid: { alpEnabled: true, alpAlertPersistSec: 6, alpDisableV1LaserOnPush: false } },
    { name: 'GPS', Component: Gps, url: '/api/gps/config', label: /enable gps/i,
        field: 'gpsEnabled', config: { gpsEnabled: true, gpsBaud: 38400 },
        invalid: { gpsEnabled: true, gpsBaud: 12345 } },
    { name: 'OBD', Component: Obd, url: '/api/obd/config', label: /enable obd/i,
        field: 'enabled', config: OBD_CONFIG, invalid: { ...OBD_CONFIG, minRssi: -101 } }
];

function server(testCase) {
    const state = {
        config: { ...testCase.config }, devices: [], posts: [], reads: 0, deviceReads: 0,
        configFailure: null, devicesFailure: null, postMode: 'success', readback: null
    };
    vi.stubGlobal('fetch', vi.fn(async (url, init = {}) => {
        if (init.method === 'POST') {
            state.posts.push({ url, body: init.body });
            if (state.postMode === 'rejected') return jsonResponse({ error: 'write rejected' }, 500);
            if (url === testCase.url) {
                const fields = init.body instanceof URLSearchParams
                    ? Object.fromEntries([...init.body].map(([key, value]) => [key,
                        value === 'true' ? true : value === 'false' ? false : Number(value)]))
                    : JSON.parse(init.body);
                Object.assign(state.config, fields);
            } else if (url === '/api/obd/devices/name') {
                state.devices[0].name = init.body.get('name');
            } else if (url === '/api/obd/forget') {
                state.devices = [];
            }
            if (state.postMode === 'lost') throw new Error('response lost after write');
            return jsonResponse({ success: true });
        }
        if (url === testCase.url) {
            state.reads++;
            if (state.readback) return state.readback;
            if (state.configFailure) return state.configFailure();
            return jsonResponse(state.config);
        }
        if (url === '/api/obd/devices') {
            state.deviceReads++;
            if (state.devicesFailure) return state.devicesFailure();
            return jsonResponse({ devices: state.devices });
        }
        throw new Error(`Unexpected request: ${url}`);
    }));
    return state;
}

beforeEach(() => { vi.spyOn(console, 'error').mockImplementation(() => {}); });
afterEach(() => { vi.unstubAllGlobals(); vi.restoreAllMocks(); });

for (const testCase of CASES) {
    describe(`${testCase.name} authoritative settings recovery`, () => {
        it('keeps failed initial and retry reads unavailable, then loads and saves real values', async () => {
            const state = server(testCase);
            state.configFailure = () => jsonResponse({ error: 'unavailable' }, 503);
            render(testCase.Component);
            const retryName = `Retry ${testCase.name} settings`;
            await screen.findByRole('button', { name: retryName });
            expect(screen.queryByRole('checkbox', { name: testCase.label })).not.toBeInTheDocument();
            expect(state.posts).toHaveLength(0);
            await fireEvent.click(screen.getByRole('button', { name: retryName }));
            await waitFor(() => expect(state.reads).toBe(2));
            await screen.findByRole('button', { name: retryName });
            expect(screen.queryByRole('checkbox', { name: testCase.label })).not.toBeInTheDocument();
            state.configFailure = null;
            await fireEvent.click(screen.getByRole('button', { name: retryName }));
            const control = await screen.findByRole('checkbox', { name: testCase.label });
            expect(control).toBeChecked();
            expect(control).toBeEnabled();
            expect(screen.queryByText(`Failed to load ${testCase.name} settings.`)).not.toBeInTheDocument();
            await fireEvent.click(control);
            await waitFor(() => expect(state.reads).toBe(4));
            await waitFor(() => expect(control).toBeEnabled());
            expect(state.posts).toHaveLength(1);
            expect(state.config[testCase.field]).toBe(false);
            expect(control).not.toBeChecked();
        });

        it.each(['missing', 'wrong type', 'out of range'])('rejects a %s successful response before editing', async (kind) => {
            const state = server(testCase);
            const payload = kind === 'missing' ? { [testCase.field]: true }
                : kind === 'wrong type' ? { ...testCase.config, [testCase.field]: 'true' }
                    : testCase.invalid;
            state.configFailure = () => jsonResponse(payload);
            render(testCase.Component);
            await screen.findByRole('button', { name: `Retry ${testCase.name} settings` });
            expect(screen.queryByRole('checkbox', { name: testCase.label })).not.toBeInTheDocument();
            expect(state.posts).toHaveLength(0);
        });

        it.each(['success', 'rejected', 'lost'])('requires fresh readback after a %s save without repeating the write', async (mode) => {
            const state = server(testCase);
            render(testCase.Component);
            const control = await screen.findByRole('checkbox', { name: testCase.label });
            state.postMode = mode;
            state.configFailure = () => jsonResponse({}, 503);
            await fireEvent.click(control);
            await screen.findByRole('button', { name: `Retry ${testCase.name} settings` });
            expect(state.posts).toHaveLength(1);
            expect(state.reads).toBe(2);
            expect(screen.queryByRole('checkbox', { name: testCase.label })).not.toBeInTheDocument();
            expect(screen.queryByText('ALP setting saved.')).not.toBeInTheDocument();
            state.configFailure = null;
            await fireEvent.click(screen.getByRole('button', { name: `Retry ${testCase.name} settings` }));
            const restored = await screen.findByRole('checkbox', { name: testCase.label });
            await waitFor(() => expect(restored).toBeEnabled());
            expect(restored.checked).toBe(mode === 'rejected');
            expect(state.posts).toHaveLength(1);
        });

        it('keeps editing locked until the post-write read has finished', async () => {
            const state = server(testCase);
            render(testCase.Component);
            const control = await screen.findByRole('checkbox', { name: testCase.label });
            let finish;
            state.readback = new Promise((resolve) => { finish = resolve; });
            await fireEvent.click(control);
            await waitFor(() => expect(state.reads).toBe(2));
            expect(control).toBeDisabled();
            // A dispatched event must not cause a second write even outside native disabled behavior.
            await fireEvent.change(control);
            expect(state.posts).toHaveLength(1);
            finish(jsonResponse(state.config));
            await waitFor(() => expect(control).toBeEnabled());
            expect(control).not.toBeChecked();
        });
    });
}

describe('configuration resource isolation', () => {
    it('keeps GPS recovery independent while an OBD save is awaiting readback', async () => {
        let gpsUnavailable = true;
        let obdReads = 0;
        let finishObdRead;
        let obdConfig = { ...OBD_CONFIG };
        const posts = [];
        const pendingObdRead = new Promise((resolve) => { finishObdRead = resolve; });
        vi.stubGlobal('fetch', vi.fn(async (url, init = {}) => {
            if (init.method === 'POST') {
                posts.push(url);
                if (url !== '/api/obd/config') throw new Error(`Unexpected write: ${url}`);
                obdConfig = { ...obdConfig, ...JSON.parse(init.body) };
                return jsonResponse({ success: true });
            }
            if (url === '/api/gps/config') {
                return gpsUnavailable ? jsonResponse({}, 503)
                    : jsonResponse({ gpsEnabled: true, gpsBaud: 38400 });
            }
            if (url === '/api/obd/config') {
                return ++obdReads === 2 ? pendingObdRead : jsonResponse(obdConfig);
            }
            if (url === '/api/obd/devices') return jsonResponse({ devices: [] });
            throw new Error(`Unexpected request: ${url}`);
        }));
        render(Gps);
        render(Obd);
        const retryGps = await screen.findByRole('button', { name: 'Retry GPS settings' });
        const obdControl = await screen.findByRole('checkbox', { name: /enable obd/i });
        expect(obdControl).toBeEnabled();
        await fireEvent.click(obdControl);
        await waitFor(() => expect(obdReads).toBe(2));
        expect(obdControl).toBeDisabled();

        gpsUnavailable = false;
        await fireEvent.click(retryGps);
        const gpsControl = await screen.findByRole('checkbox', { name: /enable gps/i });
        expect(gpsControl).toBeEnabled();
        expect(gpsControl).toBeChecked();
        expect(obdControl).toBeDisabled();
        expect(posts).toEqual(['/api/obd/config']);

        await act(async () => { finishObdRead(jsonResponse(obdConfig)); });
        await waitFor(() => expect(obdControl).toBeEnabled());
        expect(obdControl).not.toBeChecked();
        expect(gpsControl).toBeChecked();
    });

    it.each(CASES.slice(1))('keeps a late $name read attached to its original component', async (testCase) => {
        const state = server(testCase);
        let finishOldRead;
        state.readback = new Promise((resolve) => { finishOldRead = resolve; });
        const first = render(testCase.Component);
        await waitFor(() => expect(state.reads).toBe(1));
        first.unmount();

        state.readback = null;
        state.config[testCase.field] = false;
        render(testCase.Component);
        const control = await screen.findByRole('checkbox', { name: testCase.label });
        expect(control).not.toBeChecked();
        expect(control).toBeEnabled();

        await act(async () => { finishOldRead(jsonResponse(testCase.config)); });
        await act();
        expect(control).not.toBeChecked();
        expect(control).toBeEnabled();
        expect(state.reads).toBe(2);
        expect(state.posts).toHaveLength(0);
    });
});

describe('OBD catalog readiness is independent of config readiness', () => {
    const testCase = CASES[2];
    const device = { address: 'AA:BB:CC:DD:EE:FF', name: 'Fixture Adapter', active: true };

    it('keeps known config usable while an unreadable catalog requires Retry', async () => {
        const state = server(testCase);
        state.devices = [{ ...device }];
        state.devicesFailure = () => jsonResponse({}, 503);
        render(Obd);
        expect(await screen.findByRole('checkbox', { name: /enable obd/i })).toBeEnabled();
        await screen.findByRole('button', { name: 'Retry saved OBD devices' });
        expect(screen.queryByText('No saved OBD adapters yet.')).not.toBeInTheDocument();
        expect(screen.queryByRole('button', { name: 'Forget Device' })).not.toBeInTheDocument();
        expect(screen.queryByRole('button', { name: 'Rename' })).not.toBeInTheDocument();
        state.devicesFailure = null;
        await fireEvent.click(screen.getByRole('button', { name: 'Retry saved OBD devices' }));
        await screen.findByText(device.name);
        expect(screen.getByRole('button', { name: 'Forget Device' })).toBeEnabled();
        expect(state.reads).toBe(1);
        expect(state.posts).toHaveLength(0);
    });

    it('keeps a known catalog usable while config is unreadable', async () => {
        const state = server(testCase);
        state.devices = [{ ...device }];
        state.configFailure = () => jsonResponse({}, 503);
        render(Obd);
        await screen.findByRole('button', { name: 'Retry OBD settings' });
        expect(await screen.findByRole('button', { name: 'Rename' })).toBeEnabled();
        expect(screen.queryByRole('checkbox', { name: /enable obd/i })).not.toBeInTheDocument();
    });

    it.each([{}, { devices: [{}] }])('rejects a malformed catalog instead of asserting it is empty', async (payload) => {
        const state = server(testCase);
        state.devicesFailure = () => jsonResponse(payload);
        render(Obd);
        await screen.findByRole('button', { name: 'Retry saved OBD devices' });
        expect(screen.queryByText('No saved OBD adapters yet.')).not.toBeInTheDocument();
        expect(state.posts).toHaveLength(0);
    });

    it.each(['rename', 'forget'])('recovers a %s with a lost response through read-only Retry', async (operation) => {
        const state = server(testCase);
        state.devices = [{ ...device }];
        render(Obd);
        await screen.findByText(device.name);
        state.postMode = 'lost';
        state.devicesFailure = () => jsonResponse({}, 503);
        if (operation === 'rename') {
            await fireEvent.click(screen.getByRole('button', { name: 'Rename' }));
            await fireEvent.input(screen.getByRole('textbox'), { target: { value: 'New name' } });
            await fireEvent.click(screen.getByRole('button', { name: 'Save' }));
        } else {
            await fireEvent.click(screen.getByRole('button', { name: 'Forget Device' }));
        }
        await screen.findByRole('button', { name: 'Retry saved OBD devices' });
        expect(state.posts).toHaveLength(1);
        expect(screen.queryByRole('button', { name: 'Forget Device' })).not.toBeInTheDocument();
        expect(screen.queryByRole('button', { name: 'Rename' })).not.toBeInTheDocument();
        state.devicesFailure = null;
        await fireEvent.click(screen.getByRole('button', { name: 'Retry saved OBD devices' }));
        if (operation === 'rename') {
            expect(await screen.findByDisplayValue('New name')).toBeInTheDocument();
        } else {
            expect(await screen.findByText('No saved OBD adapters yet.')).toBeInTheDocument();
        }
        expect(state.posts).toHaveLength(1);
    });
});
