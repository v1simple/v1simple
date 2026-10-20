import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { installFixtureFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import fixtures from '../../test/fixtures/wifi-api.json';
import Page from './+page.svelte';

function snapshot(xBand) {
    const result = structuredClone(fixtures.scenarios.v1_profile_routes['GET /api/v1/snapshot'][0].body);
    const bytes = [xBand ? 255 : 254, 255, 255, 255, 255, 255];
    result.settings.xBand = xBand;
    result.settings.bytes = bytes;
    result.observations.userBytes.value = bytes;
    return result;
}

function terminalStatus(kind = 'apply_profile', state = 'succeeded') {
    const empty = { requested: false, sent: false, verified: false, outcome: 'not_requested', reason: 'none' };
    const components = Object.fromEntries(
        ['userSettings', 'display', 'mode', 'volume', 'customFrequencies', 'factoryReset']
            .map((name) => [name, { ...empty }])
    );
    components.userSettings = { requested: true, sent: true, verified: true, outcome: 'verified', reason: 'none' };
    if (kind === 'factory_reset') {
        components.factoryReset = { requested: true, sent: true, verified: false, outcome: 'sent', reason: 'factory_default_unverified' };
    }
    return {
        operationId: 42, kind, source: 'maintenance_ui', profileName: 'No X',
        targetAddress: 'AA:BB:CC:DD:EE:FF', returnToMaintenance: true,
        state, result: state, terminal: true, components,
        reason: state === 'succeeded' ? 'none' : kind === 'factory_reset'
            ? 'factory_reset_scope_unverified' : 'recapture_timed_out'
    };
}

function deferredResponse() {
    let resolve;
    const promise = new Promise((finish) => { resolve = finish; });
    return { promise, async finish(response) {
        await act(async () => { resolve(response); await promise; });
        await act();
    } };
}

function installProfiles(overrides) {
    return installFixtureFetchMock(['frontend_core_routes', 'v1_profile_routes'], [
        ...overrides,
        { method: 'GET', match: '/api/v1/profiles', respond: jsonResponse({
            schemaVersion: 4, profiles: [{ name: 'No X', description: '' }]
        }) },
        { method: 'GET', match: '/api/v1/profile?name=No%20X', respond: jsonResponse({
            schemaVersion: 4, name: 'No X', settings: { xBand: false }
        }) }
    ]);
}

beforeEach(() => {
    const values = new Map();
    Object.defineProperty(window, 'localStorage', { configurable: true, value: {
        getItem: (key) => values.get(key) ?? null,
        setItem: (key, value) => values.set(key, String(value)),
        removeItem: (key) => values.delete(key)
    } });
});
afterEach(() => vi.restoreAllMocks());

describe('captured settings after detector operations', () => {
    it('refreshes verified settings before a new captured draft without overwriting an open editor', async () => {
        const refresh = deferredResponse();
        let snapshotReads = 0;
        let savedPayload;
        installProfiles([
            { method: 'GET', match: '/api/v1/snapshot', respond: () =>
                ++snapshotReads === 1 ? jsonResponse(snapshot(true)) : refresh.promise },
            { method: 'POST', match: '/api/v1/apply', respond: jsonResponse({
                success: true, queued: true, operationId: 42, state: 'pending_normal_boot',
                rebooting: true, target: 'normal'
            }, 202) },
            { method: 'GET', match: '/api/autopush/status?operationId=42', respond: jsonResponse(terminalStatus()) },
            { method: 'POST', match: '/api/v1/profile', respond: ({ init }) => {
                savedPayload = JSON.parse(init.body);
                return jsonResponse({ success: true });
            } }
        ]);
        const { unmount } = render(Page);
        try {
            await screen.findByText('FF FF FF FF FF FF');
            const row = (await screen.findByText('No X')).closest('.surface-panel');
            await fireEvent.click(within(row).getByRole('button', { name: /^edit$/i }));
            await screen.findByText('Editing profile: No X');
            await fireEvent.click(screen.getByLabelText('X Band'));
            expect(screen.getByLabelText('X Band')).toBeChecked();
            await fireEvent.click(screen.getByRole('button', { name: /apply saved profile to v1/i }));
            await screen.findByText(/Captured settings will refresh when/);
            expect(screen.queryByRole('button', { name: /start draft from captured settings/i })).not.toBeInTheDocument();
            await screen.findByText('Applied and verified', {}, { timeout: 3000 });
            expect(snapshotReads).toBe(2);
            expect(screen.queryByRole('button', { name: /start draft from captured settings/i })).not.toBeInTheDocument();
            await refresh.finish(jsonResponse(snapshot(false)));
            await screen.findByText('FE FF FF FF FF FF');
            // A refresh updates the source, not the user's independent edits.
            expect(screen.getByLabelText('X Band')).toBeChecked();
            await fireEvent.click(screen.getByRole('button', { name: /start draft from captured settings/i }));
            expect(screen.getByLabelText('X Band')).not.toBeChecked();
            await fireEvent.click(screen.getByRole('button', { name: /save as profile/i }));
            await fireEvent.input(screen.getByLabelText('Profile Name'), { target: { value: 'Captured after apply' } });
            const dialog = screen.getByText('Save Profile').closest('.modal-box');
            await fireEvent.click(within(dialog).getByRole('button', { name: /^save$/i }));
            await screen.findByText('Profile "Captured after apply" saved');
            expect(savedPayload.settings.xBand).toBe(false);
            expect(savedPayload.settings.baseBytes).toEqual([254, 255, 255, 255, 255, 255]);
        } finally { unmount(); }
    });

    it.each([
        ['apply_profile', 'succeeded'], ['apply_profile', 'failed'], ['factory_reset', 'partial']
    ])('refreshes a resumed %s operation ending as %s and rejects a late initial snapshot', async (kind, state) => {
        window.localStorage.setItem('v1simple.detectorSettingsOperationId', '42');
        const initial = deferredResponse();
        let snapshotReads = 0;
        installProfiles([
            { method: 'GET', match: '/api/v1/snapshot', respond: () =>
                ++snapshotReads === 1 ? initial.promise : jsonResponse(snapshot(false)) },
            { method: 'GET', match: '/api/autopush/status?operationId=42', respond: jsonResponse(terminalStatus(kind, state)) }
        ]);
        const { unmount } = render(Page);
        try {
            await screen.findByText('FE FF FF FF FF FF');
            expect(snapshotReads).toBe(2);
            await initial.finish(jsonResponse(snapshot(true)));
            expect(screen.queryByText('FF FF FF FF FF FF')).not.toBeInTheDocument();
            await fireEvent.click(screen.getByRole('button', { name: /start draft from captured settings/i }));
            expect(screen.getByLabelText('X Band')).not.toBeChecked();
        } finally { unmount(); }
    });

    it('keeps captured actions unavailable when the terminal refresh fails', async () => {
        window.localStorage.setItem('v1simple.detectorSettingsOperationId', '42');
        const terminal = deferredResponse();
        let snapshotReads = 0;
        installProfiles([
            { method: 'GET', match: '/api/v1/snapshot', respond: () =>
                ++snapshotReads === 1 ? jsonResponse(snapshot(true)) : jsonResponse({}, 503) },
            { method: 'GET', match: '/api/autopush/status?operationId=42', respond: () => terminal.promise }
        ]);
        const { unmount } = render(Page);
        try {
            await screen.findByText('FF FF FF FF FF FF');
            await terminal.finish(jsonResponse(terminalStatus()));
            await waitFor(() => expect(snapshotReads).toBe(2));
            await screen.findByText(/No detector settings have been captured yet/);
            expect(screen.queryByText('FF FF FF FF FF FF')).not.toBeInTheDocument();
            expect(screen.queryByRole('button', { name: /start draft from captured settings/i })).not.toBeInTheDocument();
        } finally { unmount(); }
    });
});
