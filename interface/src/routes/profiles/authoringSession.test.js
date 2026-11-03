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
