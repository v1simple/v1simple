import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function installDefaultFetch(overrides = []) {
	return installFetchMock(
		[
			...overrides,
			{
				method: 'GET',
				match: '/api/status',
				respond: jsonResponse({ maintenanceBoot: false, maintenanceBootUptimeMs: 0 })
			},
			{
				method: 'GET',
				match: '/api/v1/devices',
				respond: jsonResponse({
					devices: [
						{
							address: 'AA:BB:CC:DD:EE:FF',
							name: 'Daily Driver',
							defaultProfile: 2,
							connected: true
						}
					]
				})
			},
			{
				method: 'GET',
				match: '/api/autopush/slots',
				respond: jsonResponse({
					slots: [{ name: 'Default' }, { name: 'Highway' }, { name: 'Comfort' }]
				})
			},
			{ method: 'POST', match: '/api/v1/devices/name', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/v1/devices/profile', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/v1/devices/delete', respond: jsonResponse({ success: true }) }
		],
		jsonResponse({})
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
			screen.getByText('Live V1 connection state is hidden in maintenance mode because BLE is not running.')
		).toBeInTheDocument();
		expect(await screen.findByRole('combobox')).toHaveValue('2');

		unmount();
	});

	it('keeps the rename form open when saving a device name fails', async () => {
		installDefaultFetch([
			{ method: 'POST', match: '/api/v1/devices/name', respond: jsonResponse({ error: 'bad name' }, 500) }
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

	it('restores the saved default profile when saving the override fails', async () => {
		installDefaultFetch([
			{ method: 'POST', match: '/api/v1/devices/profile', respond: jsonResponse({ error: 'bad profile' }, 500) }
		]);
		const { unmount } = render(Page);

		await screen.findByText('Daily Driver');
		const select = await screen.findByRole('combobox');
		expect(select).toHaveValue('2');

		await fireEvent.change(select, { target: { value: '1' } });

		await screen.findByText('Failed to save default profile.');
		await waitFor(() => {
			expect(screen.getByText('Will auto-push Highway (slot 2) on connect.')).toBeInTheDocument();
		});

		unmount();
	});

	it('keeps the device listed when delete fails', async () => {
		installDefaultFetch([
			{ method: 'POST', match: '/api/v1/devices/delete', respond: jsonResponse({ error: 'bad delete' }, 500) }
		]);
		const { unmount } = render(Page);

		await screen.findByText('Daily Driver');
		await fireEvent.click(screen.getByRole('button', { name: /^delete$/i }));

		await screen.findByText('Failed to remove device.');
		expect(screen.getByText('Daily Driver')).toBeInTheDocument();
		expect(screen.getByRole('button', { name: /^delete$/i })).toBeInTheDocument();

		unmount();
	});
});
