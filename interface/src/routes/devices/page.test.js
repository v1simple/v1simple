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
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/v1/devices/profile',
                respond: jsonResponse({ error: 'bad profile' }, 500)
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

        unmount();
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
