import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

import * as settingsLazyComponents from '$lib/features/settings/settingsLazyComponents.js';
import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

const defaultSlots = [
	{
		index: 0,
		ssid: 'HomeWifi',
		label: 'Garage',
		priority: 0,
		hasPassword: true,
		lastConnectedAtSec: 100,
		configured: true
	},
	{
		index: 1,
		ssid: 'PhoneHotspot',
		label: 'Phone',
		priority: 1,
		hasPassword: true,
		lastConnectedAtSec: 50,
		configured: true
	},
	{ index: 2, ssid: '', label: '', priority: 2, hasPassword: false, lastConnectedAtSec: 0, configured: false },
	{ index: 3, ssid: '', label: '', priority: 3, hasPassword: false, lastConnectedAtSec: 0, configured: false }
];

function jsonBodies(fetchMock, url, method = 'POST') {
	return fetchMock.mock.calls
		.filter(([requestUrl, init]) => requestUrl === url && (init?.method || 'GET') === method)
		.map(([, init]) => JSON.parse(init?.body || '{}'));
}

function createDeferred() {
	let resolve;
	let reject;
	const promise = new Promise((res, rej) => {
		resolve = res;
		reject = rej;
	});
	return { promise, resolve, reject };
}

function installDefaultFetch(overrides = [], { slots = defaultSlots, wifiStatus = undefined } = {}) {
	return installFetchMock(
		[
			...overrides,
			{
				method: 'GET',
				match: '/api/device/settings',
				respond: jsonResponse({ ap_ssid: 'V1' })
			},
			{
				method: 'GET',
				match: '/api/wifi/status',
				respond: jsonResponse(
					wifiStatus || {
						enabled: true,
						state: 'disconnected',
						savedSSID: '',
						connectedSSID: '',
						connectedSlotIndex: null,
						rssi: 0
					}
				)
			},
			{ method: 'GET', match: '/api/wifi/networks', respond: jsonResponse({ slots }) },
			{ method: 'GET', match: '/api/wifi/scan', respond: jsonResponse({ scanning: false, networks: [] }) },
			{ method: 'GET', match: '/api/status', respond: jsonResponse({ time: { valid: false } }) },
			{ method: 'POST', match: '/api/device/settings', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/wifi/enable', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/wifi/disconnect', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/wifi/scan', respond: jsonResponse({ scanning: false, networks: [] }) },
			{
				method: 'POST',
				match: (url) => url === '/api/wifi/networks/delete',
				respond: jsonResponse({ success: true })
			},
			{
				method: 'POST',
				match: (url) => url === '/api/wifi/networks/test',
				respond: jsonResponse({ success: true })
			},
			{
				method: 'POST',
				match: (url) => url === '/api/wifi/networks',
				respond: jsonResponse({ success: true, index: 0 })
			}
		],
		jsonResponse({})
	);
}

async function selectRestoreFile(contents, name = 'backup.json') {
	await screen.findByRole('button', { name: /restore from backup/i });
	const input = document.querySelector('input[type="file"]');
	expect(input).not.toBeNull();
	const file = new File([contents], name, { type: 'application/json' });
	await fireEvent.change(input, { target: { files: [file] } });
	return file;
}

async function startRestore(contents, name = 'backup.json') {
	await selectRestoreFile(contents, name);
	await fireEvent.click(await screen.findByRole('button', { name: /restore from backup/i }));
}

async function openManualEditor() {
	const buttons = await screen.findAllByRole('button', { name: /enter manually/i });
	await fireEvent.click(buttons[0]);
	await screen.findByText('Add Saved Network');
}

async function openScanModal() {
	const buttons = await screen.findAllByRole('button', { name: /pick from scan/i });
	await fireEvent.click(buttons[0]);
	await screen.findByText('Pick WiFi Network to Save');
}

describe('settings route page', () => {
	afterEach(() => {
		vi.useRealTimers();
		vi.restoreAllMocks();
		vi.unstubAllGlobals();
	});

	it('loads settings, wifi status, and saved network slots', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Settings');
		await screen.findByText('Saved Networks');
		await screen.findByText('Garage');
		await screen.findByText('Phone');
		await screen.findByText('Slot 3: Empty slot');
		expect(fetchMock.mock.calls.some(([url]) => url === '/api/device/settings')).toBe(true);
		expect(fetchMock.mock.calls.some(([url]) => url === '/api/wifi/status')).toBe(true);
		expect(fetchMock.mock.calls.some(([url]) => url === '/api/wifi/networks')).toBe(true);
		expect(screen.queryByText('Bluetooth Proxy')).not.toBeInTheDocument();

		unmount();
	});

	it('shows load error when settings endpoint fails', async () => {
		installFetchMock(
			[
				{
					method: 'GET',
					match: '/api/device/settings',
					respond: () => Promise.reject(new Error('boom'))
				},
				{ method: 'GET', match: '/api/wifi/status', respond: jsonResponse({ enabled: false, state: 'disabled' }) },
				{ method: 'GET', match: '/api/wifi/networks', respond: jsonResponse({ slots: [] }) },
				{ method: 'GET', match: '/api/status', respond: jsonResponse({ time: { valid: false } }) }
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await screen.findByText('Failed to load settings');
		unmount();
	});

	it('shows WiFi status load errors in the page alert', async () => {
		installFetchMock(
			[
				{ method: 'GET', match: '/api/device/settings', respond: jsonResponse({ ap_ssid: 'V1' }) },
				{ method: 'GET', match: '/api/wifi/status', respond: jsonResponse({ error: 'bad wifi' }, 500) },
				{ method: 'GET', match: '/api/wifi/networks', respond: jsonResponse({ slots: defaultSlots }) },
				{ method: 'GET', match: '/api/status', respond: jsonResponse({ time: { valid: false } }) }
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await screen.findByText('Failed to load WiFi status');
		expect(screen.getByText('Settings')).toBeInTheDocument();

		unmount();
	});

	it('manual add dialog accepts label, SSID, password, and priority', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await openManualEditor();
		await fireEvent.input(screen.getByLabelText('Label'), { target: { value: 'Car hotspot' } });
		await fireEvent.input(screen.getByLabelText('SSID'), { target: { value: 'CarAP' } });
		await fireEvent.input(screen.getByLabelText('Password'), { target: { value: 'secret123' } });
		await fireEvent.input(screen.getByLabelText('Priority'), { target: { value: '0' } });
		await fireEvent.click(screen.getByRole('button', { name: /^Save$/i }));

		await screen.findByText('Saved CarAP');
		const bodies = jsonBodies(fetchMock, '/api/wifi/networks');
		expect(bodies.at(-1)).toMatchObject({ ssid: 'CarAP', label: 'Car hotspot', password: 'secret123', priority: 0 });

		unmount();
	});

	it('toggles saved-network password visibility in the manual editor', async () => {
		installDefaultFetch();
		const { unmount } = render(Page);

		await openManualEditor();
		const password = screen.getByLabelText('Password');
		expect(password).toHaveAttribute('type', 'password');

		await fireEvent.click(screen.getByRole('button', { name: /^Show$/i }));
		expect(password).toHaveAttribute('type', 'text');

		await fireEvent.click(screen.getByRole('button', { name: /^Hide$/i }));
		expect(password).toHaveAttribute('type', 'password');

		unmount();
	});

	it('edit dialog keeps an existing password when password is left blank', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Garage');
		await fireEvent.click(screen.getAllByRole('button', { name: /^Edit$/i })[0]);
		await screen.findByText('Edit Saved Network');
		await fireEvent.input(screen.getByLabelText('Label'), { target: { value: 'Garage Updated' } });
		await fireEvent.click(screen.getByRole('button', { name: /^Save$/i }));

		await screen.findByText('Saved HomeWifi');
		const body = jsonBodies(fetchMock, '/api/wifi/networks').at(-1);
		expect(body).toMatchObject({ index: 0, ssid: 'HomeWifi', label: 'Garage Updated', priority: 0 });
		expect(body).not.toHaveProperty('password');

		unmount();
	});

	it('deletes a saved network slot', async () => {
		const fetchMock = installDefaultFetch();
		vi.stubGlobal('confirm', vi.fn(() => true));
		const { unmount } = render(Page);

		await screen.findByText('Garage');
		await fireEvent.click(screen.getAllByRole('button', { name: /^Delete$/i })[0]);

		await screen.findByText('Deleted HomeWifi');
		expect(jsonBodies(fetchMock, '/api/wifi/networks/delete').at(-1)).toEqual({ index: 0 });

		unmount();
	});

	it('starts a saved-network test connect', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Garage');
		await fireEvent.click(screen.getAllByRole('button', { name: /^Test$/i })[0]);

		await screen.findByText('Testing connection to HomeWifi...');
		expect(jsonBodies(fetchMock, '/api/wifi/networks/test').at(-1)).toEqual({ index: 0 });

		unmount();
	});

	it('reorders saved network priority with move controls', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Phone');
		await fireEvent.click(screen.getByRole('button', { name: /move phonehotspot up/i }));

		await screen.findByText('WiFi network priority updated');
		const bodies = jsonBodies(fetchMock, '/api/wifi/networks');
		expect(bodies).toEqual(
			expect.arrayContaining([
				expect.objectContaining({ index: 1, ssid: 'PhoneHotspot', priority: 0 }),
				expect.objectContaining({ index: 0, ssid: 'HomeWifi', priority: 1 })
			])
		);

		unmount();
	});

	it('pins the currently connected network with RSSI', async () => {
		installDefaultFetch([], {
			wifiStatus: {
				enabled: true,
				state: 'connected',
				connectedSSID: '',
				connectedSlotIndex: 1,
				ip: '192.168.1.50',
				rssi: -55
			}
		});
		const { unmount } = render(Page);

		const phone = await screen.findByText('Phone');
		const garage = await screen.findByText('Garage');
		expect(phone.compareDocumentPosition(garage) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
		expect(screen.getByText('Connected')).toBeInTheDocument();
		expect(screen.getByText(/Slot 2 \(Phone\)/)).toBeInTheDocument();
		expect(screen.getAllByText('-55 dBm').length).toBeGreaterThan(0);

		unmount();
	});

	it('saves a network picked from scan results', async () => {
		vi.useFakeTimers();
		const fetchMock = installDefaultFetch([
			{
				method: 'POST',
				match: '/api/wifi/scan',
				respond: jsonResponse({ scanning: true, networks: [] })
			},
			{
				method: 'GET',
				match: '/api/wifi/scan',
				respond: jsonResponse({ scanning: false, networks: [{ ssid: 'BenchAP', secure: true, rssi: -42 }] })
			}
		]);
		const { unmount } = render(Page);

		await openScanModal();
		await vi.advanceTimersByTimeAsync(1000);
		await fireEvent.click(await screen.findByRole('button', { name: /BenchAP/i }));
		await fireEvent.input(await screen.findByLabelText('Password'), { target: { value: 'bench-secret' } });
		await fireEvent.click(screen.getByRole('button', { name: /^Save Network$/i }));

		await screen.findByText('Saved BenchAP');
		expect(jsonBodies(fetchMock, '/api/wifi/networks').at(-1)).toMatchObject({
			ssid: 'BenchAP',
			password: 'bench-secret',
			priority: 2
		});

		unmount();
	});

	it('lazy-loads the WiFi scan modal on first open and reuses it after closing', async () => {
		const deferred = createDeferred();
		const wifiModalLoader = vi
			.spyOn(settingsLazyComponents, 'loadSettingsWifiModal')
			.mockReturnValue(deferred.promise);
		installDefaultFetch();
		const { unmount } = render(Page);

		expect(wifiModalLoader).not.toHaveBeenCalled();
		await fireEvent.click((await screen.findAllByRole('button', { name: /pick from scan/i }))[0]);

		expect(wifiModalLoader).toHaveBeenCalledTimes(1);
		await screen.findByText('Loading WiFi modal...');

		deferred.resolve(await import('$lib/features/settings/SettingsWifiModal.svelte'));
		await screen.findByText('Pick WiFi Network to Save');
		await fireEvent.click(screen.getByRole('button', { name: /^Close$/i }));
		await waitFor(() => {
			expect(screen.queryByText('Pick WiFi Network to Save')).toBeNull();
		});

		await fireEvent.click(screen.getAllByRole('button', { name: /pick from scan/i })[0]);
		await screen.findByText('Pick WiFi Network to Save');
		expect(wifiModalLoader).toHaveBeenCalledTimes(1);

		unmount();
	});

	it('shows WiFi scan polling errors in the page alert', async () => {
		vi.useFakeTimers();
		installDefaultFetch([
			{
				method: 'POST',
				match: '/api/wifi/scan',
				respond: jsonResponse({ scanning: true, networks: [] })
			},
			{
				method: 'GET',
				match: '/api/wifi/scan',
				respond: jsonResponse({ error: 'scan failed' }, 500)
			}
		]);
		const { unmount } = render(Page);

		await openScanModal();
		await vi.advanceTimersByTimeAsync(1000);

		await screen.findByText('Failed to update WiFi scan');

		unmount();
	});

	it('shows success message on save success', async () => {
		const fetchMock = installDefaultFetch([
			{ method: 'POST', match: '/api/device/settings', respond: jsonResponse({ success: true }) }
		]);
		const { unmount } = render(Page);

		const saveButton = await screen.findByRole('button', { name: /save settings/i });
		await fireEvent.click(saveButton);
		await screen.findByText('Settings saved! New AP credentials apply the next time the AP starts.');
		const postCall = fetchMock.mock.calls.find(
			([url, init]) => url === '/api/device/settings' && init?.method === 'POST'
		);
		expect(postCall).toBeTruthy();
		expect(postCall[1].body.has('proxy_ble')).toBe(false);
		expect(postCall[1].body.has('proxy_name')).toBe(false);
		unmount();
	});

	it('shows API error message on save failure', async () => {
		installDefaultFetch([
			{ method: 'POST', match: '/api/device/settings', respond: jsonResponse({ success: false }, 500) }
		]);
		const { unmount } = render(Page);

		const saveButton = await screen.findByRole('button', { name: /save settings/i });
		await fireEvent.click(saveButton);

		await screen.findByText('Failed to save settings');
		unmount();
	});

	it('rejects invalid backup JSON before posting restore', async () => {
		const fetchMock = installDefaultFetch();
		vi.stubGlobal('confirm', vi.fn(() => true));
		const { unmount } = render(Page);

		await startRestore('{not-json');

		await screen.findByText('Selected file is not valid JSON.');
		expect(fetchMock.mock.calls.some(([url]) => url === '/api/settings/restore')).toBe(false);

		unmount();
	});

	it('rejects unrecognized backup types before posting restore', async () => {
		const fetchMock = installDefaultFetch();
		vi.stubGlobal('confirm', vi.fn(() => true));
		const { unmount } = render(Page);

		await startRestore(JSON.stringify({ _type: 'wrong_backup_type' }));

		await screen.findByText('Selected file is not a V1 Simple settings backup.');
		expect(fetchMock.mock.calls.some(([url]) => url === '/api/settings/restore')).toBe(false);

		unmount();
	});

	it('posts valid backups to restore endpoint', async () => {
		const fetchMock = installDefaultFetch([
			{ method: 'POST', match: '/api/settings/restore', respond: jsonResponse({ success: true }) }
		]);
		vi.stubGlobal('confirm', vi.fn(() => true));
		const { unmount } = render(Page);
		const payload = JSON.stringify({ _type: 'v1simple_backup', profiles: [] });

		await startRestore(payload);

		await screen.findByText('Settings restored! Refresh to see changes.');
		expect(
			fetchMock.mock.calls.some(([url, init]) => url === '/api/settings/restore' && init?.body === payload)
		).toBe(true);

		unmount();
	});
});
