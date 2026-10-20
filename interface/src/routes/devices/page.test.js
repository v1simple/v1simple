import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { installFixtureFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function installDefaultFetch(overrides = []) {
    return installFixtureFetchMock(
        ['frontend_core_routes', 'v1_device_routes', 'autopush_routes'],
        overrides
    );
}

describe('devices route page', () => {
    beforeEach(() => {
        global.confirm = vi.fn(() => true);
    });

    afterEach(() => {
        vi.restoreAllMocks();
    });

    it('loads saved devices and opens the rename form', async () => {
        installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('Saved V1 Devices');
        await screen.findByText('Daily Driver');
        await screen.findByText('Connected');

        expect(await screen.findByRole('combobox')).toHaveValue('2');

        await fireEvent.click(screen.getByRole('button', { name: /^rename$/i }));

        expect(await screen.findByDisplayValue('Daily Driver')).toBeInTheDocument();
        expect(screen.getByRole('button', { name: /^save$/i })).toBeInTheDocument();

        unmount();
    });

    it('hides live connected badges in maintenance mode', async () => {
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/status',
                respond: jsonResponse({ maintenanceBoot: true, maintenanceBootUptimeMs: 12000 })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Daily Driver');
        expect(screen.queryByText('Connected')).not.toBeInTheDocument();
        expect(
            screen.getByText(
                'Live V1 connection state is hidden in maintenance mode because BLE is not running.'
            )
        ).toBeInTheDocument();
        expect(await screen.findByRole('combobox')).toHaveValue('2');

        unmount();
    });

    it('keeps the rename form open when saving a device name fails', async () => {
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/v1/devices/name',
                respond: jsonResponse({ error: 'bad name' }, 500)
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Daily Driver');
        await fireEvent.click(screen.getByRole('button', { name: /^rename$/i }));
        await fireEvent.change(await screen.findByDisplayValue('Daily Driver'), {
            target: { value: 'Track Car' }
        });
        await fireEvent.click(screen.getByRole('button', { name: /^save$/i }));

        await screen.findByText('Failed to save device name.');
        expect(screen.getByDisplayValue('Track Car')).toBeInTheDocument();
        expect(screen.getByRole('button', { name: /^save$/i })).toBeInTheDocument();

        unmount();
    });

    it('displays the submitted name when the input changes during a pending save', async () => {
        let finishSave;
        const pendingSave = new Promise((resolve) => (finishSave = resolve));
        const fetchMock = installDefaultFetch([
            { method: 'POST', match: '/api/v1/devices/name', respond: () => pendingSave }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Daily Driver');
        await fireEvent.click(screen.getByRole('button', { name: /^rename$/i }));
        const input = await screen.findByDisplayValue('Daily Driver');
        await fireEvent.input(input, { target: { value: '  SentName  ' } });
        await fireEvent.click(screen.getByRole('button', { name: /^save$/i }));
        await fireEvent.input(input, { target: { value: 'NeverSentName' } });
        finishSave(jsonResponse({ success: true }));

        await screen.findByText('Device name saved.');
        expect(screen.getByRole('heading', { name: 'SentName' })).toBeInTheDocument();
        expect(screen.queryByText('NeverSentName')).not.toBeInTheDocument();
        const saveCall = fetchMock.mock.calls.find(
            ([url, init]) => url === '/api/v1/devices/name' && init?.method === 'POST'
        );
        expect(saveCall[1].body.get('name')).toBe('SentName');
        unmount();
    });

    it.each([false, true])('blocks keyboard rename resubmission until completion, then permits retry (other device completed: %s)', async (completeOtherDevice) => {
        const deviceA = { address: 'AA:BB:CC:DD:EE:01', name: 'Car A', defaultProfile: 0 };
        const deviceB = { address: 'AA:BB:CC:DD:EE:02', name: 'Car B', defaultProfile: 0 };
        let finishFirstBody;
        let renameCalls = 0;
        const firstBody = new Promise((resolve) => { finishFirstBody = resolve; });
        const fetchMock = installDefaultFetch([
            { method: 'GET', match: '/api/v1/devices',
                respond: jsonResponse({ devices: [deviceA, deviceB] }) },
            { method: 'POST', match: '/api/v1/devices/name', respond: () => {
                renameCalls += 1;
                return renameCalls === 1
                    ? { ok: false, status: 500, json: () => firstBody }
                    : jsonResponse({ success: true });
            } },
            { method: 'POST', match: '/api/v1/devices/profile',
                respond: jsonResponse({ success: true }) }
        ]);
        const { unmount } = render(Page);
        await screen.findByText('Car A');
        const cardA = screen.getByText('Car A').closest('.surface-card');
        const cardB = screen.getByText('Car B').closest('.surface-card');
        await fireEvent.click(within(cardA).getByRole('button', { name: 'Rename' }));
        const input = within(cardA).getByRole('textbox');
        await fireEvent.input(input, { target: { value: 'First name' } });
        await fireEvent.keyDown(input, { key: 'Enter' });
        await waitFor(() => expect(renameCalls).toBe(1));
        expect(within(cardA).getByRole('button', { name: 'Save' })).toBeDisabled();

        if (completeOtherDevice) {
            await fireEvent.change(within(cardB).getByRole('combobox'), { target: { value: '1' } });
            await screen.findByText('Default profile updated.');
        }
        expect(within(cardA).getByRole('button', { name: 'Save' })).toBeDisabled();
        await fireEvent.change(within(cardA).getByRole('combobox'), { target: { value: '1' } });
        expect(fetchMock.mock.calls.filter(
            ([url, init]) => url === '/api/v1/devices/profile' && init.body.get('address') === deviceA.address
        )).toHaveLength(0);
        await fireEvent.input(input, { target: { value: 'Retry name' } });
        await fireEvent.keyDown(input, { key: 'Enter' });
        const pendingCalls = renameCalls;
        finishFirstBody({ error: 'storage unavailable' });
        await screen.findByText('Failed to save device name.');
        expect(pendingCalls).toBe(1);
        expect(within(cardA).getByRole('textbox')).toHaveValue('Retry name');

        await fireEvent.keyDown(input, { key: 'Enter' });
        await screen.findByText('Device name saved.');
        expect(within(cardA).getByRole('heading')).toHaveTextContent('Retry name');
        const posts = fetchMock.mock.calls.filter(
            ([url, init]) => url === '/api/v1/devices/name' && init?.method === 'POST'
        );
        expect(posts).toHaveLength(2);
        expect(posts.map(([, init]) => init.body.get('name'))).toEqual(['First name', 'Retry name']);
        unmount();
    });

    it.each([
        [200, { success: true }, 'Device name saved.'],
        [202, { success: true, mirrorSyncPending: true },
            'Device name saved on primary storage; mirror sync is pending.']
    ])('preserves another device rename draft when a pending save completes with %i', async (status, body, message) => {
        const deviceA = { address: 'AA:BB:CC:DD:EE:01', name: 'Car A', defaultProfile: 0 };
        const deviceB = { address: 'AA:BB:CC:DD:EE:02', name: 'Car B', defaultProfile: 0 };
        let finishSave;
        const pendingSave = new Promise((resolve) => (finishSave = resolve));
        const fetchMock = installDefaultFetch([
            { method: 'GET', match: '/api/v1/devices',
                respond: jsonResponse({ devices: [deviceA, deviceB] }) },
            { method: 'POST', match: '/api/v1/devices/name', respond: () => pendingSave }
        ]);
        const { unmount } = render(Page);
        await screen.findByText('Car A');
        const cardA = screen.getByText('Car A').closest('.surface-card');
        const cardB = screen.getByText('Car B').closest('.surface-card');
        await fireEvent.click(within(cardA).getByRole('button', { name: 'Rename' }));
        await fireEvent.input(within(cardA).getByRole('textbox'), { target: { value: 'Saved A' } });
        await fireEvent.click(within(cardA).getByRole('button', { name: 'Save' }));
        expect(within(cardA).getByRole('button', { name: 'Save' })).toBeDisabled();

        await fireEvent.click(within(cardB).getByRole('button', { name: 'Rename' }));
        await fireEvent.input(within(cardB).getByRole('textbox'), { target: { value: 'Unsaved B draft' } });
        finishSave(jsonResponse(body, status));
        await screen.findByText(message);

        expect(within(cardA).getByRole('heading')).toHaveTextContent('Saved A');
        expect(within(cardB).getByRole('textbox')).toHaveValue('Unsaved B draft');
        expect(within(cardB).getByRole('button', { name: 'Save' })).toBeEnabled();
        const posts = fetchMock.mock.calls.filter(
            ([url, init]) => url === '/api/v1/devices/name' && init?.method === 'POST'
        );
        expect(posts).toHaveLength(1);
        expect(posts[0][1].body.get('address')).toBe(deviceA.address);
        expect(posts[0][1].body.get('name')).toBe('Saved A');
        unmount();
    });

    it('preserves a new editor session for the same device after a pending rename was cancelled', async () => {
        let finishSave;
        const pendingSave = new Promise((resolve) => (finishSave = resolve));
        installDefaultFetch([
            { method: 'GET', match: '/api/v1/devices', respond: jsonResponse({ devices: [
                { address: 'AA:BB:CC:DD:EE:01', name: 'Car A', defaultProfile: 0 },
                { address: 'AA:BB:CC:DD:EE:02', name: 'Car B', defaultProfile: 0 }
            ] }) },
            { method: 'POST', match: '/api/v1/devices/name', respond: () => pendingSave },
            { method: 'POST', match: '/api/v1/devices/profile', respond: jsonResponse({ success: true }) }
        ]);
        const { unmount } = render(Page);
        await screen.findByText('Car A');
        const cardA = screen.getByText('Car A').closest('.surface-card');
        const cardB = screen.getByText('Car B').closest('.surface-card');
        await fireEvent.click(within(cardA).getByRole('button', { name: 'Rename' }));
        await fireEvent.input(within(cardA).getByRole('textbox'), { target: { value: 'Saved A' } });
        await fireEvent.click(within(cardA).getByRole('button', { name: 'Save' }));
        await fireEvent.keyDown(within(cardA).getByRole('textbox'), { key: 'Escape' });

        // An independent device operation can finish while the rename is still pending.
        await fireEvent.change(within(cardB).getByRole('combobox'), { target: { value: '1' } });
        await screen.findByText('Default profile updated.');
        await fireEvent.click(within(cardA).getByRole('button', { name: 'Rename' }));
        await fireEvent.input(within(cardA).getByRole('textbox'), { target: { value: 'New A draft' } });
        finishSave(jsonResponse({ success: true }));
        await screen.findByText('Device name saved.');

        expect(within(cardA).getByRole('textbox')).toHaveValue('New A draft');
        await fireEvent.click(within(cardA).getByRole('button', { name: 'Cancel' }));
        expect(within(cardA).getByRole('heading')).toHaveTextContent('Saved A');
        unmount();
    });

    it('restores the saved default profile when saving the override fails', async () => {
        let attempts = 0;
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/v1/devices/profile',
                respond: () => ++attempts === 1
                    ? jsonResponse({ error: 'bad profile' }, 500)
                    : jsonResponse({ success: true })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Daily Driver');
        const select = await screen.findByRole('combobox');
        expect(select).toHaveValue('2');

        await fireEvent.change(select, { target: { value: '1' } });

        await screen.findByText('Failed to save default profile.');
        await waitFor(() => {
            expect(
                screen.getByText('Will auto-push Highway (slot 2) on connect.')
            ).toBeInTheDocument();
        });

        await waitFor(() => expect(select).toBeEnabled());
        await fireEvent.change(select, { target: { value: '1' } });
        await screen.findByText('Default profile updated.');
        expect(select).toHaveValue('1');
        expect(attempts).toBe(2);

        unmount();
    });

    it('keeps one device busy while another completes and rejects profile reentry through the owner', async () => {
        const addressA = 'AA:BB:CC:DD:EE:01';
        const addressB = 'AA:BB:CC:DD:EE:02';
        let finishFirstBody;
        let aCalls = 0;
        let storedProfile = 0;
        const firstBody = new Promise((resolve) => { finishFirstBody = resolve; });
        const fetchMock = installDefaultFetch([
            { method: 'GET', match: '/api/v1/devices', respond: jsonResponse({ devices: [
                { address: addressA, name: 'Car A', defaultProfile: 0 },
                { address: addressB, name: 'Car B', defaultProfile: 0 }
            ] }) },
            { method: 'POST', match: '/api/v1/devices/profile', respond: ({ init }) => {
                if (init.body.get('address') === addressA) {
                    aCalls += 1;
                    storedProfile = Number(init.body.get('profile'));
                    if (aCalls === 1) return { ok: true, status: 200, json: () => firstBody };
                }
                return jsonResponse({ success: true });
            } }
        ]);
        const { unmount } = render(Page);
        try {
            await screen.findByText('Car A');
            const cardA = screen.getByText('Car A').closest('.surface-card');
            const cardB = screen.getByText('Car B').closest('.surface-card');
            const selectA = within(cardA).getByRole('combobox');
            const selectB = within(cardB).getByRole('combobox');
            await fireEvent.change(selectA, { target: { value: '1' } });
            await waitFor(() => expect(aCalls).toBe(1));
            expect(selectA).toBeDisabled();
            expect(selectB).toBeEnabled();
            await fireEvent.change(selectB, { target: { value: '1' } });
            await screen.findByText('Default profile updated.');
            const aStillDisabled = selectA.disabled;
            // A synthetic event also exercises admission beneath disabled controls.
            await fireEvent.change(selectA, { target: { value: '2' } });
            const callsWhilePending = aCalls;
            finishFirstBody({ success: true });
            await waitFor(() => expect(selectA).toBeEnabled());
            expect(aStillDisabled).toBe(true);
            expect(callsWhilePending).toBe(1);
            expect(storedProfile).toBe(1);
            expect(selectA).toHaveValue(String(storedProfile));

            await fireEvent.change(selectA, { target: { value: '2' } });
            await waitFor(() => expect(aCalls).toBe(2));
            await waitFor(() => expect(selectA).toBeEnabled());
            expect(selectA).toHaveValue(String(storedProfile));
            const posts = fetchMock.mock.calls.filter(
                ([url, init]) => url === '/api/v1/devices/profile' && init.body.get('address') === addressA
            );
            expect(posts.map(([, init]) => init.body.get('profile'))).toEqual(['1', '2']);
        } finally {
            finishFirstBody({ success: true });
            unmount();
        }
    });

    it('retains a rename draft while a profile save blocks mutations for that device', async () => {
        let finishProfile;
        const pendingProfile = new Promise((resolve) => { finishProfile = resolve; });
        const fetchMock = installDefaultFetch([
            { method: 'POST', match: '/api/v1/devices/profile', respond: () => pendingProfile },
            { method: 'POST', match: '/api/v1/devices/name', respond: jsonResponse({ success: true }) }
        ]);
        const { unmount } = render(Page);
        try {
            await screen.findByText('Daily Driver');
            await fireEvent.click(screen.getByRole('button', { name: 'Rename' }));
            const input = screen.getByRole('textbox');
            await fireEvent.input(input, { target: { value: 'Retained draft' } });
            await fireEvent.change(screen.getByRole('combobox'), { target: { value: '1' } });
            expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
            await fireEvent.keyDown(input, { key: 'Enter' });
            const renamePosts = () => fetchMock.mock.calls.filter(
                ([url, init]) => url === '/api/v1/devices/name' && init?.method === 'POST'
            );
            const blockedCalls = renamePosts().length;
            finishProfile(jsonResponse({ success: true }));
            await screen.findByText('Default profile updated.');
            expect(blockedCalls).toBe(0);
            expect(screen.getByRole('textbox')).toHaveValue('Retained draft');
            expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();
            await fireEvent.keyDown(input, { key: 'Enter' });
            await screen.findByText('Device name saved.');
            expect(screen.getByRole('heading', { name: 'Retained draft' })).toBeInTheDocument();
            expect(renamePosts()).toHaveLength(1);
        } finally {
            finishProfile(jsonResponse({ success: true }));
            unmount();
        }
    });

    it('discloses pending mirror sync after the primary name update commits', async () => {
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/v1/devices/name',
                respond: jsonResponse({ success: true, mirrorSyncPending: true }, 202)
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Daily Driver');
        await fireEvent.click(screen.getByRole('button', { name: /^rename$/i }));
        await fireEvent.input(await screen.findByDisplayValue('Daily Driver'), {
            target: { value: 'Track Car' }
        });
        await fireEvent.click(screen.getByRole('button', { name: /^save$/i }));

        await screen.findByText('Device name saved on primary storage; mirror sync is pending.');
        expect(await screen.findByRole('heading', { name: 'Track Car' })).toBeInTheDocument();
        expect(screen.queryByText('Device name saved.')).not.toBeInTheDocument();

        unmount();
    });

    it('discloses pending mirror sync after the primary profile update commits', async () => {
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/v1/devices/profile',
                respond: jsonResponse({ success: true, mirrorSyncPending: true }, 202)
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Daily Driver');
        const select = await screen.findByRole('combobox');
        await fireEvent.change(select, { target: { value: '1' } });

        await screen.findByText('Default profile saved on primary storage; mirror sync is pending.');
        expect(select).toHaveValue('1');
        expect(screen.queryByText('Default profile updated.')).not.toBeInTheDocument();

        unmount();
    });

    it('blocks removal during a profile save and releases admission after cancelled removal', async () => {
        let finishProfile;
        const pendingProfile = new Promise((resolve) => { finishProfile = resolve; });
        const fetchMock = installDefaultFetch([
            { method: 'POST', match: '/api/v1/devices/profile', respond: () => pendingProfile },
            { method: 'POST', match: '/api/v1/devices/delete', respond: jsonResponse({ success: true }) }
        ]);
        const { unmount } = render(Page);
        try {
            await screen.findByText('Daily Driver');
            await fireEvent.change(screen.getByRole('combobox'), { target: { value: '1' } });
            const remove = screen.getByRole('button', { name: 'Delete' });
            expect(remove).toBeDisabled();
            // Dispatch directly to verify the owner also refuses the mutation.
            await fireEvent.click(remove);
            expect(global.confirm).not.toHaveBeenCalled();
            finishProfile(jsonResponse({ success: true }));
            await screen.findByText('Default profile updated.');
            expect(remove).toBeEnabled();

            global.confirm.mockReturnValueOnce(false);
            await fireEvent.click(remove);
            expect(remove).toBeEnabled();
            await fireEvent.click(remove);
            await screen.findByText('Device removed.');
            expect(fetchMock.mock.calls.filter(
                ([url, init]) => url === '/api/v1/devices/delete' && init?.method === 'POST'
            )).toHaveLength(1);
        } finally {
            finishProfile(jsonResponse({ success: true }));
            unmount();
        }
    });

    it.each([
        ['failed profile', false],
        ['manual refresh', false],
        ['manual refresh', true]
    ])('preserves a newer B commit while %s reloads A (B already pending: %s)', async (trigger, alreadyPending) => {
        const addressA = 'AA:BB:CC:DD:EE:01';
        const addressB = 'AA:BB:CC:DD:EE:02';
        const durable = [
            { address: addressA, name: 'Car A', defaultProfile: 0 },
            { address: addressB, name: 'Car B', defaultProfile: 0 }
        ];
        let getCount = 0;
        let finishRefresh;
        let finishB;
        const pendingRefresh = new Promise((resolve) => { finishRefresh = resolve; });
        installDefaultFetch([
            { method: 'GET', match: '/api/v1/devices', respond: () => {
                getCount += 1;
                const snapshot = structuredClone({ devices: durable });
                return getCount === 2
                    ? { ok: true, status: 200, json: async () => { await pendingRefresh; return snapshot; } }
                    : jsonResponse(snapshot);
            } },
            { method: 'POST', match: '/api/v1/devices/profile', respond: ({ init }) => {
                if (init.body.get('address') === addressA) {
                    return jsonResponse({ error: 'storage unavailable' }, 500);
                }
                const commit = () => {
                    durable[1].defaultProfile = Number(init.body.get('profile'));
                    return jsonResponse({ success: true });
                };
                if (!alreadyPending) return commit();
                return new Promise((resolve) => { finishB = () => resolve(commit()); });
            } }
        ]);
        const { unmount } = render(Page);
        try {
            await screen.findByText('Car A');
            const selectA = within(screen.getByText('Car A').closest('.surface-card')).getByRole('combobox');
            const selectB = within(screen.getByText('Car B').closest('.surface-card')).getByRole('combobox');
            if (alreadyPending) await fireEvent.change(selectB, { target: { value: '2' } });
            if (trigger === 'failed profile') {
                await fireEvent.change(selectA, { target: { value: '1' } });
            } else {
                durable[0].defaultProfile = 3;
                await fireEvent.click(screen.getByRole('button', { name: 'Refresh' }));
            }
            await waitFor(() => expect(getCount).toBe(2));
            if (alreadyPending) finishB();
            else await fireEvent.change(selectB, { target: { value: '2' } });
            await screen.findByText('Default profile updated.');
            await waitFor(() => expect(selectB).toBeEnabled());
            expect(selectB).toHaveValue('2');
            finishRefresh();
            await waitFor(() => expect(selectA).toHaveValue(String(durable[0].defaultProfile)));
            await waitFor(() => expect(selectA).toBeEnabled());
            expect(durable[1].defaultProfile).toBe(2);
            expect(selectB).toHaveValue('2');

            // A later fresh snapshot remains authoritative after the race is over.
            durable[1].defaultProfile = 1;
            await fireEvent.click(screen.getByRole('button', { name: 'Refresh' }));
            await waitFor(() => expect(selectB).toHaveValue('1'));
        } finally {
            finishB?.();
            finishRefresh();
            unmount();
        }
    });

    it('does not restore a removed device from a pending manual refresh', async () => {
        const devices = [
            { address: 'AA:BB:CC:DD:EE:01', name: 'Car A', defaultProfile: 0 },
            { address: 'AA:BB:CC:DD:EE:02', name: 'Car B', defaultProfile: 0 }
        ];
        let getCount = 0;
        let finishRefresh;
        const pendingRefresh = new Promise((resolve) => { finishRefresh = resolve; });
        installDefaultFetch([
            { method: 'GET', match: '/api/v1/devices', respond: () => {
                getCount += 1;
                const snapshot = structuredClone({ devices });
                if (getCount === 1) return jsonResponse(snapshot);
                snapshot.devices[0].defaultProfile = 3;
                return { ok: true, status: 200, json: async () => { await pendingRefresh; return snapshot; } };
            } },
            { method: 'POST', match: '/api/v1/devices/delete', respond: jsonResponse({ success: true }) }
        ]);
        const { unmount } = render(Page);
        try {
            await screen.findByText('Car B');
            await fireEvent.click(screen.getByRole('button', { name: 'Refresh' }));
            await waitFor(() => expect(getCount).toBe(2));
            const cardB = screen.getByText('Car B').closest('.surface-card');
            await fireEvent.click(within(cardB).getByRole('button', { name: 'Delete' }));
            await screen.findByText('Device removed.');
            expect(screen.queryByText('Car B')).not.toBeInTheDocument();
            finishRefresh();
            await waitFor(() => expect(within(screen.getByText('Car A').closest('.surface-card'))
                .getByRole('combobox')).toHaveValue('3'));
            expect(screen.queryByText('Car B')).not.toBeInTheDocument();
        } finally {
            finishRefresh();
            unmount();
        }
    });

    it('keeps the newest refresh when overlapping snapshots arrive in reverse order', async () => {
        let getCount = 0;
        let finishOlder;
        let finishNewer;
        const older = new Promise((resolve) => { finishOlder = resolve; });
        const newer = new Promise((resolve) => { finishNewer = resolve; });
        const snapshot = (name, profile) => ({ devices: [
            { address: 'AA:BB:CC:DD:EE:01', name, defaultProfile: profile }
        ] });
        installDefaultFetch([
            { method: 'GET', match: '/api/v1/devices', respond: () => {
                getCount += 1;
                if (getCount === 1) return jsonResponse(snapshot('Initial', 0));
                return getCount === 2
                    ? { ok: true, status: 200, json: async () => { await older; return snapshot('Older', 1); } }
                    : { ok: true, status: 200, json: async () => { await newer; return snapshot('Newest', 2); } };
            } }
        ]);
        const { unmount } = render(Page);
        try {
            await screen.findByText('Initial');
            await fireEvent.click(screen.getByRole('button', { name: 'Refresh' }));
            await waitFor(() => expect(getCount).toBe(2));
            await fireEvent.click(screen.getByRole('button', { name: 'Refresh' }));
            await waitFor(() => expect(getCount).toBe(3));
            finishNewer();
            await screen.findByText('Newest');
            finishOlder();
            await new Promise((resolve) => setTimeout(resolve, 0));
            expect(screen.getByText('Newest')).toBeInTheDocument();
            expect(screen.getByRole('combobox')).toHaveValue('2');
        } finally {
            finishOlder();
            finishNewer();
            unmount();
        }
    });

    it('keeps the device listed when delete fails', async () => {
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/v1/devices/delete',
                respond: jsonResponse({ error: 'bad delete' }, 500)
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Daily Driver');
        await fireEvent.click(screen.getByRole('button', { name: /^delete$/i }));

        await screen.findByText('Failed to remove device.');
        expect(screen.getByText('Daily Driver')).toBeInTheDocument();
        expect(screen.getByRole('button', { name: /^delete$/i })).toBeInTheDocument();

        unmount();
    });

    it('keeps the device listed while a durable delete operation is pending', async () => {
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/v1/devices/delete',
                respond: jsonResponse({ success: true, operationPending: true }, 202)
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Daily Driver');
        await fireEvent.click(screen.getByRole('button', { name: /^delete$/i }));

        await screen.findByText('Device removal is pending; recovery will continue automatically.');
        expect(screen.getByText('Daily Driver')).toBeInTheDocument();
        expect(screen.getByRole('button', { name: /^delete$/i })).toBeInTheDocument();
        expect(screen.queryByText('Device removed.')).not.toBeInTheDocument();

        unmount();
    });
});
