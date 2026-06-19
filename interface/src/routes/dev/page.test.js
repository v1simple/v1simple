import { fireEvent, render, screen } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function createStorageMock() {
	const data = new Map();

	return {
		getItem(key) {
			return data.has(key) ? data.get(key) : null;
		},
		setItem(key, value) {
			data.set(String(key), String(value));
		},
		removeItem(key) {
			data.delete(String(key));
		}
	};
}

function installDefaultFetch(overrides = []) {
	return installFetchMock(
		[
			...overrides,
			{
				method: 'GET',
				match: '/api/device/settings',
				respond: jsonResponse({
					powerOffSdLog: false
				})
			},
			{ method: 'POST', match: '/api/device/settings', respond: jsonResponse({ success: true }) }
		],
		jsonResponse({})
	);
}

describe('dev route page', () => {
	beforeEach(() => {
		const localStorageMock = createStorageMock();
		const sessionStorageMock = createStorageMock();
		Object.defineProperty(window, 'localStorage', { configurable: true, value: localStorageMock });
		Object.defineProperty(window, 'sessionStorage', { configurable: true, value: sessionStorageMock });
		Object.defineProperty(globalThis, 'localStorage', { configurable: true, value: localStorageMock });
		Object.defineProperty(globalThis, 'sessionStorage', { configurable: true, value: sessionStorageMock });
		global.confirm = vi.fn(() => true);
	});

	afterEach(() => {
		vi.restoreAllMocks();
	});

	it('loads development settings without polling debug endpoints', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Development Settings');
		await screen.findByText('Power-Off SD Log');
		expect(screen.getByText('Runtime Diagnostics')).toBeInTheDocument();
		expect(screen.getByText(/HTTP debug metrics, scenario playback, and perf-file endpoints are disabled/i)).toBeInTheDocument();
		expect(fetchMock.mock.calls.some(([url]) => String(url).startsWith('/api/debug/'))).toBe(false);

		unmount();
	});

	it('saves only dev-owned device settings', async () => {
		const fetchMock = installDefaultFetch([
			{
				method: 'GET',
				match: '/api/device/settings',
				respond: jsonResponse({
					powerOffSdLog: true,
					alpEnabled: true,
					alpSdLogEnabled: true
				})
			}
		]);
		const { unmount } = render(Page);

		await screen.findByText('Power-Off SD Log');
		await fireEvent.click(screen.getByRole('checkbox', { name: /i understand the risks/i }));
		await fireEvent.click(screen.getByRole('button', { name: /save settings/i }));

		await screen.findByText('Settings saved!');
		const postCall = fetchMock.mock.calls.find(
			([url, init]) => url === '/api/device/settings' && init?.method === 'POST'
		);
		expect(postCall?.[1]?.body?.get('powerOffSdLog')).toBe('true');
		expect(postCall?.[1]?.body?.has('alpEnabled')).toBe(false);
		expect(postCall?.[1]?.body?.has('alpSdLogEnabled')).toBe(false);

		unmount();
	});

	it('shows NVS diagnostics when present', async () => {
		installDefaultFetch([
			{
				method: 'GET',
				match: '/api/device/settings',
				respond: jsonResponse({
					powerOffSdLog: false,
					proxy_ble: true,
					nvsDiag: {
						healthy: true,
						ns: 'settings_a',
						valid: 1,
						ver: 2,
						bright: 180,
						proxy: 1,
						autoPush: 0
					}
				})
			}
		]);
		const { unmount } = render(Page);

		await screen.findByText('NVS Persistence');
		expect(screen.getByText('settings_a')).toBeInTheDocument();
		expect(screen.getByText('In-memory proxy_ble=true')).toBeInTheDocument();

		unmount();
	});
});
