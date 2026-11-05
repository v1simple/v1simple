import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import Page from './+page.svelte';
import AutoPush from '../autopush/+page.svelte';
import { installFixtureFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import fixtures from '../../test/fixtures/wifi-api.json';

function install(overrides = []) {
    return installFixtureFetchMock(['frontend_core_routes', 'v1_profile_routes', 'autopush_routes'], [
        ...overrides,
        { method: 'GET', match: '/api/status', respond: jsonResponse({ maintenanceBoot: true, maintenanceBootUptimeMs: 9000 }) },
        { method: 'GET', match: '/api/v1/profiles', respond: jsonResponse({ schemaVersion: 4, profiles: [{ name: 'Daily' }] }) },
        { method: 'GET', match: '/api/v1/profile?', respond: jsonResponse({
            schemaVersion: 4, name: 'Daily', description: 'Original', settings: { bytes: [255,255,255,255,255,255], xBand: true }
        }) },
        { method: 'GET', match: '/api/autopush/slots', respond: jsonResponse({
            ...fixtures.scenarios.autopush_routes['GET /api/autopush/slots'][0].body, schemaVersion: 4
        }) }
    ]);
}
function pending() {
    let resolve;
    let reject;
    const response = new Promise((done, fail) => { resolve = done; reject = fail; });
    return { response, resolve, reject };
}
const postCalls = (mock, url) => mock.mock.calls.filter(([path, init]) => path === url && init?.method === 'POST');
beforeEach(() => {
    const values = new Map();
    Object.defineProperty(window, 'localStorage', { configurable: true, value: {
        getItem: (key) => values.get(key) ?? null, setItem: (key, value) => values.set(key, String(value)),
        removeItem: (key) => values.delete(key)
    } });
});
afterEach(() => { vi.restoreAllMocks(); });

async function startNew() {
    await fireEvent.click(await screen.findByRole('button', { name: 'New Profile', exact: true }));
    await fireEvent.input(screen.getByLabelText('Description'), { target: { value: 'Unsent description' } });
    await fireEvent.click(screen.getByLabelText('X Band'));
    await fireEvent.click(screen.getByRole('button', { name: /^In-the-Box/ }));
    await fireEvent.click(screen.getByRole('checkbox', { name: 'K Out-of-the-Box muting' }));
    await fireEvent.click(screen.getByRole('button', { name: 'Save as Profile' }));
    await fireEvent.input(screen.getByLabelText('Profile Name'), { target: { value: 'Unique' } });
    await fireEvent.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Save', exact: true }));
}

describe('mounted authoring session boundaries', () => {
    it.each(['unchanged', 'cancel', 'reset then cancel', 'reset then cancel after ACK', 'cancel and reopen', 'reopen then cancel after ACK'])(
        'retains acknowledged fields on the next description-only save after %s', async (transition) => {
            const save = pending();
            let saves = 0;
            const mock = install([{ method: 'POST', match: '/api/v1/profile', respond: () =>
                ++saves === 1 ? save.response : jsonResponse({ success: true }) }]);
            const view = render(Page);
            try {
                const row = (await screen.findByText('Daily', { exact: true })).closest('.surface-panel');
                await fireEvent.click(within(row).getByRole('button', { name: 'Edit', exact: true }));
                await screen.findByText('Editing profile: Daily');
                const editor = within(document.getElementById('profile-editor'));
                await fireEvent.click(screen.getByLabelText('X Band'));
                await fireEvent.click(editor.getByRole('button', { name: 'Save Profile', exact: true }));
                await waitFor(() => expect(postCalls(mock, '/api/v1/profile')).toHaveLength(1));
                const first = JSON.parse(postCalls(mock, '/api/v1/profile')[0][1].body);
                expect(first.settings.xBand).toBe(false);

                if (transition.startsWith('reset')) {
                    await fireEvent.click(screen.getByText('Reset this draft', { exact: true }));
                    await fireEvent.click(editor.getByRole('button', { name: 'Reset this draft to local defaults' }));
                }
                if (transition !== 'unchanged' && transition !== 'reset then cancel after ACK') {
                    await fireEvent.click(editor.getByRole('button', { name: 'Cancel', exact: true }));
                }
                const reopened = transition.includes('reopen');
                if (reopened) {
                    await fireEvent.click(editor.getByRole('button', { name: 'Edit', exact: true }));
                    await fireEvent.input(screen.getByLabelText('Description'), { target: { value: 'Later unsent description' } });
                }
                save.resolve(jsonResponse({ success: true }));
                await screen.findByText('Profile "Daily" saved');
                if (transition.endsWith('after ACK')) {
                    await fireEvent.click(editor.getByRole('button', { name: 'Cancel', exact: true }));
                }
                if (transition !== 'cancel and reopen') {
                    expect(screen.getByLabelText('X Band')).toBeDisabled();
                    await fireEvent.click(editor.getByRole('button', { name: 'Edit', exact: true }));
                } else {
                    expect(screen.getByLabelText('Description')).toHaveValue('Later unsent description');
                }
                await fireEvent.input(screen.getByLabelText('Description'), { target: { value: 'Only a new description' } });
                await fireEvent.click(editor.getByRole('button', { name: 'Save Profile', exact: true }));
                await waitFor(() => expect(postCalls(mock, '/api/v1/profile')).toHaveLength(2));
                const second = JSON.parse(postCalls(mock, '/api/v1/profile')[1][1].body);
                expect(second).toEqual({ ...first, description: 'Only a new description' });
                expect(mock.mock.calls.filter(([url]) => url.startsWith('/api/v1/profile?'))).toHaveLength(1);
                expect(postCalls(mock, '/api/v1/apply')).toHaveLength(0);
            } finally { view.unmount(); }
        }
    );

    it.each([false, true])('reselects a pending saved profile without an obsolete read (via another profile: %s)', async (viaOther) => {
        const save = pending();
        const obsoleteRead = pending();
        let saves = 0;
        let dailyReads = 0;
        const daily = { schemaVersion: 4, name: 'Daily', description: 'Original', settings: { xBand: true } };
        const mock = install([
            { method: 'GET', match: '/api/v1/profiles', respond: jsonResponse({
                schemaVersion: 4, profiles: [{ name: 'Daily' }, { name: 'Night' }]
            }) },
            { method: 'GET', match: '/api/v1/profile?name=Daily', respond: () =>
                ++dailyReads === 1 ? jsonResponse(daily) : obsoleteRead.response },
            { method: 'GET', match: '/api/v1/profile?name=Night', respond: jsonResponse({ ...daily, name: 'Night' }) },
            { method: 'POST', match: '/api/v1/profile', respond: () =>
                ++saves === 1 ? save.response : jsonResponse({ success: true }) }
        ]);
        const view = render(Page);
        const select = async (name) => {
            const row = (await screen.findByText(name, { exact: true })).closest('.surface-panel');
            const details = row.closest('details');
            if (!details.open) await fireEvent.click(details.querySelector('summary'));
            await fireEvent.click(within(row).getByRole('button', { name: 'Edit', exact: true }));
            await screen.findByText(`Editing profile: ${name}`);
        };
        try {
            await select('Daily');
            await fireEvent.click(screen.getByLabelText('X Band'));
            await fireEvent.click(screen.getByRole('button', { name: 'Save Profile', exact: true }));
            await waitFor(() => expect(postCalls(mock, '/api/v1/profile')).toHaveLength(1));
            if (viaOther) await select('Night');
            await select('Daily');
            await fireEvent.input(screen.getByLabelText('Description'), { target: { value: 'Reselected intent' } });
            expect(dailyReads).toBe(1);
            save.resolve(jsonResponse({ success: true }));
            await screen.findByText('Profile "Daily" saved');
            // A pending selection must not start a GET that could arrive after ACK.
            await act(async () => obsoleteRead.resolve(jsonResponse(daily)));
            expect(screen.getByLabelText('Description')).toHaveValue('Reselected intent');
            expect(screen.getByLabelText('X Band')).not.toBeChecked();
            await fireEvent.click(screen.getByRole('button', { name: 'Save Profile', exact: true }));
            await waitFor(() => expect(postCalls(mock, '/api/v1/profile')).toHaveLength(2));
            const payloads = postCalls(mock, '/api/v1/profile').map(([, init]) => JSON.parse(init.body));
            expect(payloads[1]).toEqual({ ...payloads[0], description: 'Reselected intent' });
            expect(mock.mock.calls.filter(([url]) => url.startsWith('/api/v1/profile?'))).toHaveLength(viaOther ? 2 : 1);
        } finally { view.unmount(); }
    });

    it.each(['before ACK', 'after ACK'])('retires an older selection read when Save starts, even if it returns %s', async (responseOrder) => {
        const save = pending();
        const olderRead = pending();
        let saves = 0;
        let reads = 0;
        const original = { schemaVersion: 4, name: 'Daily', description: 'Original', settings: { xBand: true } };
        const finishOlderRead = async () => {
            const json = vi.fn(async () => original);
            olderRead.resolve({ ok: true, json });
            await waitFor(() => expect(json).toHaveBeenCalledOnce());
            await act(async () => {});
        };
        const mock = install([
            { method: 'GET', match: '/api/v1/profile?name=Daily', respond: () =>
                ++reads === 1 ? jsonResponse(original) : olderRead.response },
            { method: 'POST', match: '/api/v1/profile', respond: () =>
                ++saves === 1 ? save.response : jsonResponse({ success: true }) }
        ]);
        const view = render(Page);
        try {
            const row = (await screen.findByText('Daily', { exact: true })).closest('.surface-panel');
            await fireEvent.click(within(row).getByRole('button', { name: 'Edit', exact: true }));
            await screen.findByText('Editing profile: Daily');
            await fireEvent.click(screen.getByLabelText('X Band'));
            const savedRow = screen.getByText('Daily', { exact: true }).closest('.surface-panel');
            await fireEvent.click(savedRow.closest('details').querySelector('summary'));
            await fireEvent.click(within(savedRow).getByRole('button', { name: 'Edit', exact: true }));
            await waitFor(() => expect(reads).toBe(2));
            await fireEvent.click(screen.getByRole('button', { name: 'Save Profile', exact: true }));
            await waitFor(() => expect(postCalls(mock, '/api/v1/profile')).toHaveLength(1));
            if (responseOrder === 'before ACK') await finishOlderRead();
            save.resolve(jsonResponse({ success: true }));
            await screen.findByText('Profile "Daily" saved');
            if (responseOrder === 'after ACK') await finishOlderRead();
            expect(screen.getByLabelText('X Band')).toBeDisabled();
            expect(screen.getByLabelText('X Band')).not.toBeChecked();
            const editor = within(document.getElementById('profile-editor'));
            await fireEvent.click(editor.getByRole('button', { name: 'Edit', exact: true }));
            await fireEvent.input(screen.getByLabelText('Description'), { target: { value: 'Only a new description' } });
            await fireEvent.click(editor.getByRole('button', { name: 'Save Profile', exact: true }));
            await waitFor(() => expect(postCalls(mock, '/api/v1/profile')).toHaveLength(2));
            const payloads = postCalls(mock, '/api/v1/profile').map(([, init]) => JSON.parse(init.body));
            expect(payloads[0].settings.xBand).toBe(false);
            expect(payloads[1]).toEqual({ ...payloads[0], description: 'Only a new description' });
            expect(reads).toBe(2);
        } finally { view.unmount(); }
    });

    it('keeps a new naming dialog and editor open when the identical old dialog save completes', async () => {
        const save = pending();
        const mock = install([{ method: 'POST', match: '/api/v1/profile', respond: () => save.response }]);
        const view = render(Page);
        try {
            await startNew();
            await fireEvent.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Cancel' }));
            await fireEvent.click(screen.getByRole('button', { name: 'Save as Profile' }));
            await fireEvent.input(screen.getByLabelText('Profile Name'), { target: { value: 'Unique' } });
            save.resolve(jsonResponse({ success: true }));
            await screen.findByText('Profile "Unique" saved');
            expect(screen.getByRole('dialog')).toBeInTheDocument();
            expect(screen.getByLabelText('Description')).toHaveValue('Unsent description');
            expect(screen.getByLabelText('X Band')).not.toBeChecked();
            expect(screen.getByRole('checkbox', { name: 'K Out-of-the-Box muting' })).toBeChecked();
            expect(postCalls(mock, '/api/v1/profile')).toHaveLength(1);
        } finally { view.unmount(); }
    });

    it.each(['lost response', 'unreadable error'])('retains all editable domains and sends once on %s', async (failure) => {
        const save = pending();
        const mock = install([{ method: 'POST', match: '/api/v1/profile', respond: () => save.response }]);
        const view = render(Page);
        try {
            await startNew();
            if (failure === 'lost response') save.reject(new Error('Disconnected'));
            else save.resolve({ ok: false, text: async () => { throw new Error('Disconnected body'); } });
            await screen.findByText('Connection error');
            expect(screen.getByLabelText('Profile Name')).toHaveValue('Unique');
            expect(screen.getByLabelText('Description')).toHaveValue('Unsent description');
            expect(screen.getByLabelText('X Band')).not.toBeChecked();
            expect(screen.getByRole('checkbox', { name: 'K Out-of-the-Box muting' })).toBeChecked();
            expect(within(screen.getByRole('dialog')).getByRole('button', { name: 'Save', exact: true })).toBeEnabled();
            expect(postCalls(mock, '/api/v1/profile')).toHaveLength(1);
        } finally { view.unmount(); }
    });
});

async function apply(page) {
    if (page === 'profiles') {
        const row = (await screen.findByText('Daily', { exact: true })).closest('.surface-panel');
        await fireEvent.click(within(row).getByRole('button', { name: 'Edit', exact: true }));
        await screen.findByText('Editing profile: Daily');
        await fireEvent.click(screen.getByRole('button', { name: 'Apply saved profile to V1' }));
    } else {
        const buttons = await screen.findAllByRole('button', { name: /Apply to V1/i });
        await waitFor(() => expect(buttons[0]).toBeEnabled());
        await fireEvent.click(buttons[0]);
    }
}

describe.each(['profiles', 'autopush'])('%s operation admission after migration', (page) => {
    const path = page === 'profiles' ? '/api/v1/apply' : '/api/autopush/push';
    it('retains the accepted identity after navigation without starting a detached poll', async () => {
        const operation = pending();
        const mock = install([{ method: 'POST', match: path, respond: () => operation.response }]);
        const view = render(page === 'profiles' ? Page : AutoPush);
        await apply(page);
        await waitFor(() => expect(postCalls(mock, path)).toHaveLength(1));
        view.unmount();
        await act(async () => operation.resolve(jsonResponse({
            success: true, queued: true, operationId: 77, state: 'pending_normal_boot', rebooting: true, target: 'normal'
        }, 202)));
        await waitFor(() => expect(window.localStorage.getItem('v1simple.detectorSettingsOperationId')).toBe('77'));
        expect(mock.mock.calls.filter(([url]) => url.includes('/api/autopush/status'))).toHaveLength(0);
        expect(postCalls(mock, path)).toHaveLength(1);
    });

    it.each(['409', 'lost response', 'invalid identity'])('does not retry or invent acceptance after %s', async (failure) => {
        const operation = pending();
        const mock = install([{ method: 'POST', match: path, respond: () => operation.response }]);
        const view = render(page === 'profiles' ? Page : AutoPush);
        try {
            await apply(page);
            if (failure === '409') operation.resolve(jsonResponse({ error: 'busy' }, 409));
            else if (failure === 'invalid identity') operation.resolve(jsonResponse({ success: true }, 202));
            else operation.reject(new Error('Disconnected'));
            await screen.findByText(failure === '409' ? 'busy' : failure === 'invalid identity'
                ? /returned an invalid operation identity/ : page === 'profiles' ? /connection was interrupted/ : 'Connection error');
            expect(window.localStorage.getItem('v1simple.detectorSettingsOperationId')).toBeNull();
            expect(postCalls(mock, path)).toHaveLength(1);
        } finally { view.unmount(); }
    });
});
