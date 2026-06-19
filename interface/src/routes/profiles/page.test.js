import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse, textResponse } from '../../test/fetch-mock.js';
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
			{ method: 'GET', match: '/api/v1/profiles', respond: jsonResponse({ profiles: [{ name: 'Daily Drive' }] }) },
			{
				method: 'GET',
				match: '/api/v1/current',
				respond: jsonResponse({ connected: true, settings: {} })
			},
			{ method: 'POST', match: '/api/v1/pull', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/v1/profile', respond: jsonResponse({ success: true }) },
		],
		jsonResponse({})
	);
}

describe('profiles route page', () => {
	beforeEach(() => {
		global.confirm = vi.fn(() => true);
	});

	afterEach(() => {
		vi.restoreAllMocks();
	});

	it('loads profiles and current settings', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('V1 Profiles');
		await screen.findByText('Daily Drive');
		await waitFor(() => {
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/profiles')).toBe(true);
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/current')).toBe(true);
		});

		unmount();
	});

	it('surfaces profile load failures without breaking the route', async () => {
		installFetchMock(
			[
				{ method: 'GET', match: '/api/v1/profiles', respond: () => Promise.reject(new Error('offline')) },
				{ method: 'GET', match: '/api/v1/current', respond: jsonResponse({ connected: false, settings: {} }) }
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await screen.findByText('V1 Profiles');
		await screen.findByText('Failed to load profiles');
		unmount();
	});

	it('opens and closes the save profile dialog', async () => {
		installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('V1 Profiles');
		const saveButtons = await screen.findAllByRole('button', { name: /^Save$/i });
		await fireEvent.click(saveButtons[0]);

		await screen.findByText('Save Profile');
		await fireEvent.click(screen.getByRole('button', { name: /cancel/i }));
		await waitFor(() => {
			expect(screen.queryByText('Save Profile')).toBeNull();
		});

		unmount();
	});

	it('saves a profile successfully from the save dialog', async () => {
		const fetchMock = installDefaultFetch([
			{ method: 'POST', match: '/api/v1/profile', respond: jsonResponse({ success: true }) }
		]);
		const { unmount } = render(Page);

		await screen.findByText('V1 Profiles');
		const saveButtons = await screen.findAllByRole('button', { name: /^Save$/i });
		await fireEvent.click(saveButtons[0]);
		const dialogTitle = await screen.findByText('Save Profile');
		const modal = dialogTitle.closest('.modal-box');
		await fireEvent.input(screen.getByLabelText('Profile Name'), { target: { value: 'Bench Profile' } });
		await fireEvent.click(within(modal).getByRole('button', { name: /^Save$/i }));

		await screen.findByText('Profile "Bench Profile" saved');
		expect(fetchMock.mock.calls.some(([url, init]) => url === '/api/v1/profile' && init?.method === 'POST')).toBe(true);
		unmount();
	});

	it('creates and saves a V1 profile while disconnected', async () => {
		let savedPayload;
		const fetchMock = installDefaultFetch([
			{ method: 'GET', match: '/api/v1/profiles', respond: jsonResponse({ profiles: [] }) },
			{ method: 'GET', match: '/api/v1/current', respond: jsonResponse({ connected: false, available: false }) },
			{
				method: 'POST',
				match: '/api/v1/profile',
				respond: ({ init }) => {
					savedPayload = JSON.parse(init.body);
					return jsonResponse({ success: true });
				}
			}
		]);
		const { unmount } = render(Page);

		await screen.findByText('V1 Disconnected');
		await fireEvent.click(screen.getByRole('button', { name: /new profile/i }));
		await screen.findByText('Creating new offline profile');

		const xBand = screen.getByLabelText('X Band');
		expect(xBand).toBeChecked();
		await fireEvent.click(xBand);
		await fireEvent.click(screen.getByRole('button', { name: /save as profile/i }));

		const dialogTitle = await screen.findByText('Save Profile');
		const modal = dialogTitle.closest('.modal-box');
		await fireEvent.input(screen.getByLabelText('Profile Name'), { target: { value: 'Offline Profile' } });
		await fireEvent.click(within(modal).getByRole('button', { name: /^Save$/i }));

		await screen.findByText('Profile "Offline Profile" saved');
		expect(fetchMock.mock.calls.some(([url, init]) => url === '/api/v1/profile' && init?.method === 'POST')).toBe(true);
		expect(savedPayload.name).toBe('Offline Profile');
		expect(savedPayload.settings.xBand).toBe(false);
		expect(savedPayload.settings.kBand).toBe(true);
		expect(savedPayload.settings.kaSensitivity).toBe(3);
		unmount();
	});

	it('shows offline authoring and disables V1 push/pull in maintenance mode', async () => {
		installDefaultFetch([
			{
				method: 'GET',
				match: '/api/status',
				respond: jsonResponse({ maintenanceBoot: true, maintenanceBootUptimeMs: 9000 })
			},
			{ method: 'GET', match: '/api/v1/current', respond: jsonResponse({ connected: false, available: false }) }
		]);
		const { unmount } = render(Page);

		await screen.findByText('Offline authoring');
		expect(screen.queryByText('V1 Disconnected')).not.toBeInTheDocument();
		expect(
			screen.getByText('V1 BLE is not running in maintenance mode. Create or edit saved profiles offline.')
		).toBeInTheDocument();
		expect(screen.getByRole('button', { name: /pull from v1/i })).toBeDisabled();
		expect(screen.getByRole('button', { name: /^push$/i })).toBeDisabled();
		expect(screen.getByText(/Read current V1 settings during normal runtime/)).toBeInTheDocument();
		expect(
			screen.getByText('Named snapshots you can edit offline; push to V1 during normal runtime.')
		).toBeInTheDocument();

		await fireEvent.click(screen.getByRole('button', { name: /new profile/i }));
		await screen.findByText('Creating new offline profile');
		expect(screen.getByRole('button', { name: /push to v1/i })).toBeDisabled();

		unmount();
	});

	it('shows API error message when save profile fails', async () => {
		installDefaultFetch([
			{ method: 'POST', match: '/api/v1/profile', respond: textResponse('bad save', 500) }
		]);
		const { unmount } = render(Page);

		await screen.findByText('V1 Profiles');
		const saveButtons = await screen.findAllByRole('button', { name: /^Save$/i });
		await fireEvent.click(saveButtons[0]);
		const dialogTitle = await screen.findByText('Save Profile');
		const modal = dialogTitle.closest('.modal-box');
		await fireEvent.input(screen.getByLabelText('Profile Name'), { target: { value: 'Broken Profile' } });
		await fireEvent.click(within(modal).getByRole('button', { name: /^Save$/i }));

		await screen.findByText('Failed to save: bad save');
		unmount();
	});

	it('shows API error message when pull fails', async () => {
		installDefaultFetch([
			{ method: 'POST', match: '/api/v1/pull', respond: jsonResponse({ error: 'bad' }, 500) }
		]);
		const { unmount } = render(Page);

		await screen.findByText('V1 Connected');
		const pullButton = await screen.findByRole('button', { name: /pull from v1/i });
		await fireEvent.click(pullButton);
		await screen.findByText('Failed to pull settings');

		unmount();
	});

	it('shows API error message when delete profile fails', async () => {
		installDefaultFetch([
			{ method: 'POST', match: '/api/v1/profile/delete', respond: jsonResponse({ error: 'Profile not found' }, 404) }
		]);
		const { unmount } = render(Page);

		await screen.findByText('Daily Drive');
		await fireEvent.click(screen.getByRole('button', { name: /^delete$/i }));

		await screen.findByText('Failed to delete: Profile not found');
		expect(screen.getByText('Daily Drive')).toBeInTheDocument();

		unmount();
	});
});
