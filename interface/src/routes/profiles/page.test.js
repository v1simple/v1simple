import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import {
    installFixtureFetchMock,
    installFetchMock,
    jsonResponse,
    textResponse
} from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function installDefaultFetch(overrides = []) {
    return installFixtureFetchMock(['frontend_core_routes', 'v1_profile_routes'], overrides);
}

describe('profiles route page', () => {
    beforeEach(() => {
        global.confirm = vi.fn(() => true);
    });

    afterEach(() => {
        vi.restoreAllMocks();
    });

    it('loads saved profiles without requesting unreachable live V1 state', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('V1 Profiles');
        await screen.findByText('Daily Drive');
        await waitFor(() => {
            expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/profiles')).toBe(true);
            expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/current')).toBe(false);
        });

        unmount();
    });

    it('requests a saved profile with its encoded query name', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        const dailyDriveRow = (await screen.findByText('Daily Drive')).closest('.surface-panel');
        await fireEvent.click(within(dailyDriveRow).getByRole('button', { name: /^edit$/i }));

        await waitFor(() => {
            expect(
                fetchMock.mock.calls.some(([url]) => url === '/api/v1/profile?name=Daily%20Drive')
            ).toBe(true);
        });

        unmount();
    });

    it('preserves display and volume metadata when editing a saved profile', async () => {
        let savedPayload;
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/v1/profile?name=Daily%20Drive',
                respond: jsonResponse({
                    name: 'Daily Drive',
                    description: 'Existing metadata',
                    displayOn: false,
                    mainVolume: 7,
                    mutedVolume: 2,
                    settings: { xBand: true }
                })
            },
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

        const dailyDriveRow = (await screen.findByText('Daily Drive')).closest('.surface-panel');
        await fireEvent.click(within(dailyDriveRow).getByRole('button', { name: /^edit$/i }));
        await screen.findByText('Editing profile: Daily Drive');
        await fireEvent.click(screen.getByRole('button', { name: /^save profile$/i }));

        await screen.findByText('Profile "Daily Drive" saved');
        expect(savedPayload.description).toBe('Existing metadata');
        expect(savedPayload.displayOn).toBe(false);
        expect(savedPayload.mainVolume).toBe(7);
        expect(savedPayload.mutedVolume).toBe(2);
        unmount();
    });

    it('surfaces profile load failures without breaking the route', async () => {
        installFetchMock(
            [
                {
                    method: 'GET',
                    match: '/api/v1/profiles',
                    respond: () => Promise.reject(new Error('offline'))
                }
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
        await fireEvent.click(screen.getByRole('button', { name: /new profile/i }));
        await fireEvent.click(screen.getByRole('button', { name: /save as profile/i }));

        const dialogTitle = await screen.findByText('Save Profile');
        const modal = dialogTitle.closest('.modal-box');
        await fireEvent.click(within(modal).getByRole('button', { name: /cancel/i }));
        await waitFor(() => {
            expect(screen.queryByText('Save Profile')).toBeNull();
        });

        unmount();
    });

    it('saves a profile successfully from the save dialog', async () => {
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/v1/profiles',
                respond: jsonResponse({ profiles: [{ name: 'Bench Profile' }] })
            },
            { method: 'POST', match: '/api/v1/profile', respond: jsonResponse({ success: true }) }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('V1 Profiles');
        await fireEvent.click(screen.getByRole('button', { name: /new profile/i }));
        await fireEvent.click(screen.getByRole('button', { name: /save as profile/i }));
        const dialogTitle = await screen.findByText('Save Profile');
        const modal = dialogTitle.closest('.modal-box');
        await fireEvent.input(screen.getByLabelText('Profile Name'), {
            target: { value: 'Bench Profile' }
        });
        await fireEvent.click(within(modal).getByRole('button', { name: /^Save$/i }));

        await screen.findByText('Profile "Bench Profile" saved');
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === '/api/v1/profile' && init?.method === 'POST'
            )
        ).toBe(true);
        unmount();
    });

    it('creates and saves a V1 profile while disconnected', async () => {
        let savedPayload;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/v1/profiles',
                respond: () =>
                    jsonResponse({ profiles: savedPayload ? [{ name: savedPayload.name }] : [] })
            },
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

        await screen.findByText('Offline authoring');
        await fireEvent.click(screen.getByRole('button', { name: /new profile/i }));
        await screen.findByText('Creating new offline profile');

        const xBand = screen.getByLabelText('X Band');
        expect(xBand).toBeChecked();
        await fireEvent.click(xBand);
        await fireEvent.click(screen.getByText('Photo Radar'));
        await fireEvent.click(screen.getByLabelText('Gatso RT4'));
        await fireEvent.click(screen.getByLabelText('Intersection Management Filter'));
        await fireEvent.click(screen.getByRole('button', { name: /save as profile/i }));

        const dialogTitle = await screen.findByText('Save Profile');
        const modal = dialogTitle.closest('.modal-box');
        await fireEvent.input(screen.getByLabelText('Profile Name'), {
            target: { value: 'Offline Profile' }
        });
        await fireEvent.click(within(modal).getByRole('button', { name: /^Save$/i }));

        await screen.findByText('Profile "Offline Profile" saved');
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === '/api/v1/profile' && init?.method === 'POST'
            )
        ).toBe(true);
        expect(savedPayload.name).toBe('Offline Profile');
        expect(savedPayload.settings.xBand).toBe(false);
        expect(savedPayload.settings.kBand).toBe(true);
        expect(savedPayload.settings.kaSensitivity).toBe(3);
        expect(savedPayload.settings.gatsoRT4).toBe(true);
        expect(savedPayload.settings.photoIntersectionFilter).toBe(true);
        unmount();
    });

    it('uses the confirmed save to update a catalog that was initially stale', async () => {
        installDefaultFetch([
            { method: 'GET', match: '/api/v1/profiles', respond: jsonResponse({ profiles: [] }) },
            { method: 'POST', match: '/api/v1/profile', respond: jsonResponse({ success: true }) }
        ]);
        const { unmount } = render(Page);

        await fireEvent.click(await screen.findByRole('button', { name: /new profile/i }));
        await fireEvent.click(screen.getByRole('button', { name: /save as profile/i }));
        const modal = (await screen.findByText('Save Profile')).closest('.modal-box');
        await fireEvent.input(screen.getByLabelText('Profile Name'), {
            target: { value: 'Unconfirmed' }
        });
        await fireEvent.click(within(modal).getByRole('button', { name: /^Save$/i }));

        await screen.findByText('Profile "Unconfirmed" saved');
        expect(screen.getByText('Unconfirmed')).toBeInTheDocument();
        unmount();
    });

    it('offers only maintenance-reachable authoring actions', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('Offline authoring');
        expect(screen.queryByRole('button', { name: /pull from v1/i })).not.toBeInTheDocument();
        expect(screen.queryByRole('button', { name: /^push$/i })).not.toBeInTheDocument();
        expect(
            screen.getByText(/Assign a saved profile on the Auto-Push page/)
        ).toBeInTheDocument();

        await fireEvent.click(screen.getByRole('button', { name: /new profile/i }));
        await screen.findByText('Creating new offline profile');
        expect(screen.queryByRole('button', { name: /push to v1/i })).not.toBeInTheDocument();
        expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/current')).toBe(false);
        expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/pull')).toBe(false);
        expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/push')).toBe(false);

        unmount();
    });

    it('shows API error message when save profile fails', async () => {
        installDefaultFetch([
            { method: 'POST', match: '/api/v1/profile', respond: textResponse('bad save', 500) }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('V1 Profiles');
        await fireEvent.click(screen.getByRole('button', { name: /new profile/i }));
        await fireEvent.click(screen.getByRole('button', { name: /save as profile/i }));
        const dialogTitle = await screen.findByText('Save Profile');
        const modal = dialogTitle.closest('.modal-box');
        await fireEvent.input(screen.getByLabelText('Profile Name'), {
            target: { value: 'Broken Profile' }
        });
        await fireEvent.click(within(modal).getByRole('button', { name: /^Save$/i }));

        await screen.findByText('Failed to save: bad save');
        unmount();
    });

    it('shows API error message when delete profile fails', async () => {
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/v1/profile/delete',
                respond: jsonResponse({ error: 'Profile not found' }, 404)
            }
        ]);
        const { unmount } = render(Page);

        const dailyDriveRow = (await screen.findByText('Daily Drive')).closest('.surface-panel');
        await fireEvent.click(within(dailyDriveRow).getByRole('button', { name: /^delete$/i }));

        await screen.findByText('Failed to delete: Profile not found');
        expect(screen.getByText('Daily Drive')).toBeInTheDocument();

        unmount();
    });
});
