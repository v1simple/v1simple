import { fireEvent, render, screen } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function installDefaultFetch(overrides = []) {
	return installFetchMock(
		[
			...overrides,
			{
				method: 'GET',
				match: '/api/autopush/slots',
				respond: jsonResponse({
					enabled: true,
					activeSlot: 1,
					slots: [
						{
							name: 'Default',
							profile: 'Road Trip',
							mode: 2,
							volume: 6,
							muteVolume: 2,
							darkMode: false,
							muteToZero: false,
							alertPersist: 1,
							priorityArrowOnly: false
						},
						{
							name: 'Highway',
							profile: 'Quiet Commute',
							mode: 3,
							volume: 8,
							muteVolume: 2,
							darkMode: true,
							muteToZero: false,
							alertPersist: 2,
							priorityArrowOnly: true
						},
						{
							name: 'Comfort',
							profile: '',
							mode: 0,
							volume: 4,
							muteVolume: 1,
							darkMode: false,
							muteToZero: true,
							alertPersist: 0,
							priorityArrowOnly: false
						}
					]
				})
			},
			{
				method: 'GET',
				match: '/api/v1/profiles',
				respond: jsonResponse({
					profiles: [{ name: 'Road Trip' }, { name: 'Quiet Commute' }]
				})
			},
			{ method: 'POST', match: '/api/autopush/activate', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/autopush/push', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/autopush/slot', respond: jsonResponse({ success: true }) }
		],
		jsonResponse({})
	);
}

describe('autopush route page', () => {
	afterEach(() => {
		vi.restoreAllMocks();
	});

	it('loads slots and opens the slot editor', async () => {
		installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Auto-Push Profiles');
		await screen.findByText('Highway');
		await screen.findByText('Active');

		await fireEvent.click(screen.getAllByRole('button', { name: /^edit$/i })[0]);

		expect(await screen.findByLabelText('Profile')).toBeInTheDocument();
		expect(screen.getByText('Alert persistence (seconds)')).toBeInTheDocument();
		expect(screen.getByRole('button', { name: /^save$/i })).toBeInTheDocument();

		unmount();
	});

	it('keeps the slot editor open when saving a slot fails', async () => {
		installDefaultFetch([
			{ method: 'POST', match: '/api/autopush/slot', respond: jsonResponse({ error: 'bad save' }, 500) }
		]);
		const { unmount } = render(Page);

		await screen.findByText('Highway');
		await fireEvent.click(screen.getAllByRole('button', { name: /^edit$/i })[0]);
		await fireEvent.click(await screen.findByRole('button', { name: /^save$/i }));

		await screen.findByText('Failed to save');
		expect(screen.getByRole('button', { name: /^save$/i })).toBeInTheDocument();
		expect(screen.getByLabelText('Profile')).toBeInTheDocument();

		unmount();
	});

	it('keeps the previous active slot when activation fails', async () => {
		installDefaultFetch([
			{ method: 'POST', match: '/api/autopush/activate', respond: jsonResponse({ error: 'bad activate' }, 500) }
		]);
		const { unmount } = render(Page);

		await screen.findByText('Highway');
		await fireEvent.click(screen.getAllByRole('button', { name: /^activate$/i })[0]);

		await screen.findByText('Failed to activate');
		expect(screen.getByText('Highway')).toBeInTheDocument();
		expect(screen.getAllByText('Active')).toHaveLength(1);
		expect(screen.getAllByRole('button', { name: /^activate$/i })).toHaveLength(2);

		unmount();
	});

	it('shows an error when profiles fail to load', async () => {
		installDefaultFetch([
			{ method: 'GET', match: '/api/v1/profiles', respond: jsonResponse({ error: 'bad profiles' }, 500) }
		]);
		const { unmount } = render(Page);

		await screen.findByText('Failed to load profiles');
		await screen.findByText('Highway');
		expect(screen.getByText('Auto-Push Profiles')).toBeInTheDocument();
		expect(screen.getByText('Active')).toBeInTheDocument();

		unmount();
	});
});
