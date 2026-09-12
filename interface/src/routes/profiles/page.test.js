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

    it('loads saved profiles and the persisted non-live V1 snapshot', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('V1 Profiles');
        await screen.findByText('Daily Drive');
        await waitFor(() => {
            expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/profiles')).toBe(true);
            expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/snapshot')).toBe(true);
            expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/current')).toBe(false);
        });

        expect(await screen.findByText('Previous boot · not live')).toBeInTheDocument();
        expect(screen.getByText('FF FF FF FF FF FF')).toBeInTheDocument();

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

    it('preserves detector policy when editing a saved profile', async () => {
        let savedPayload;
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/v1/profile?name=Daily%20Drive',
                respond: jsonResponse({
                    schemaVersion: 2,
                    name: 'Daily Drive',
                    description: 'Existing metadata',
                    detector: {
                        userSettings: 'value',
                        mode: { policy: 'value', value: 2 },
                        display: 'off',
                        volume: { policy: 'temporary', main: 7, muted: 2 },
                        bluetoothLed: 'unchanged',
                        customFrequencies: 'unchanged'
                    },
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
        expect(savedPayload.schemaVersion).toBe(2);
        expect(savedPayload.detector).toEqual({
            userSettings: 'value',
            mode: { policy: 'value', value: 2 },
            display: 'off',
            volume: { policy: 'temporary', main: 7, muted: 2 },
            bluetoothLed: 'unchanged',
            customFrequencies: 'unchanged'
        });
        expect(savedPayload).not.toHaveProperty('displayOn');
        expect(savedPayload).not.toHaveProperty('mainVolume');
        unmount();
    });

    it('makes profile authoring read-only while ownership migration is pending', async () => {
        installDefaultFetch([{
            method: 'GET',
            match: '/api/v1/profiles',
            respond: jsonResponse({
                schemaVersion: 1,
                detectorConfigurationOwner: 'legacy-slot',
                profiles: [{ name: 'Daily Drive', description: 'Legacy' }]
            })
        }]);
        const { unmount } = render(Page);

        await screen.findByText(/profile settings migration is still pending/i);
        expect(screen.queryByRole('button', { name: /new profile/i })).not.toBeInTheDocument();
        expect(screen.queryByRole('button', { name: /^edit$/i })).not.toBeInTheDocument();
        expect(screen.getByRole('button', { name: /start draft from captured settings/i })).toBeDisabled();
        unmount();
    });

    it.each(['another profile', 'the same profile'])(
        'keeps newer edits to %s when a previous save completes',
        async (nextEditor) => {
            const profiles = ['Alpha', 'Beta'].map((name) => ({
                name, description: `${name} original`, displayOn: true,
                settings: { xBand: true }
            }));
            let finishSave;
            let savedPayload;
            installDefaultFetch([
                { method: 'GET', match: '/api/v1/profiles', respond: jsonResponse({ profiles }) },
                ...profiles.map((profile) => ({
                    method: 'GET', match: `/api/v1/profile?name=${profile.name}`,
                    respond: jsonResponse(profile)
                })),
                {
                    method: 'POST', match: '/api/v1/profile',
                    respond: ({ init }) => {
                        savedPayload = JSON.parse(init.body);
                        return new Promise((resolve) => { finishSave = resolve; });
                    }
                }
            ]);
            const { unmount } = render(Page);
            try {
                const alpha = (await screen.findByText('Alpha')).closest('.surface-panel');
                await fireEvent.click(within(alpha).getByRole('button', { name: /^edit$/i }));
                await screen.findByText('Editing profile: Alpha');
                await fireEvent.input(screen.getByLabelText('Description'), {
                    target: { value: 'Alpha submitted description' }
                });
                await fireEvent.click(screen.getByRole('button', { name: /^save profile$/i }));
                await waitFor(() => expect(finishSave).toBeTypeOf('function'));

                const nextName = nextEditor === 'another profile' ? 'Beta' : 'Alpha';
                if (nextName === 'Beta') {
                    const beta = screen.getByText('Beta').closest('.surface-panel');
                    await fireEvent.click(within(beta).getByRole('button', { name: /^edit$/i }));
                    await screen.findByText('Editing profile: Beta');
                }
                await fireEvent.click(screen.getByLabelText('X Band'));
                await fireEvent.input(screen.getByLabelText('Description'), {
                    target: { value: 'New unsaved description' }
                });
                finishSave(jsonResponse({ success: true }));

                await screen.findByText('Profile "Alpha" saved');
                expect(savedPayload).toMatchObject({
                    name: 'Alpha', description: 'Alpha submitted description',
                    settings: { xBand: true }
                });
                expect(screen.getByText(`Editing profile: ${nextName}`)).toBeInTheDocument();
                expect(screen.getByLabelText('X Band')).not.toBeChecked();
                expect(screen.getByLabelText('X Band')).toBeEnabled();
                expect(screen.getByLabelText('Description')).toHaveValue('New unsaved description');
                expect(screen.getByText('Alpha').closest('.surface-panel'))
                    .toHaveTextContent('Alpha submitted description');
                expect(screen.getByText('Beta').closest('.surface-panel'))
                    .toHaveTextContent('Beta original');

                await fireEvent.click(screen.getByRole('button', { name: /^cancel$/i }));
                expect(screen.getByLabelText('X Band')).toBeChecked();
                expect(screen.getByLabelText('X Band')).toBeDisabled();
            } finally {
                unmount();
            }
        }
    );

    it('serializes save submissions while preserving newer draft edits', async () => {
        const pending = [];
        const submitted = [];
        installDefaultFetch([
            {
                method: 'GET', match: '/api/v1/profile?name=Daily%20Drive',
                respond: jsonResponse({ name: 'Daily Drive', settings: { xBand: true } })
            },
            {
                method: 'POST', match: '/api/v1/profile',
                respond: ({ init }) => {
                    submitted.push(JSON.parse(init.body));
                    return new Promise((resolve) => pending.push(resolve));
                }
            }
        ]);
        const { unmount } = render(Page);
        try {
            const row = (await screen.findByText('Daily Drive')).closest('.surface-panel');
            await fireEvent.click(within(row).getByRole('button', { name: /^edit$/i }));
            await screen.findByText('Editing profile: Daily Drive');
            await fireEvent.input(screen.getByLabelText('Description'), {
                target: { value: 'Earlier save' }
            });
            const save = screen.getByRole('button', { name: /^save profile$/i });
            await fireEvent.click(save);
            await waitFor(() => expect(pending).toHaveLength(1));
            await fireEvent.input(screen.getByLabelText('Description'), {
                target: { value: 'Latest save' }
            });
            await fireEvent.click(screen.getByLabelText('X Band'));
            await fireEvent.click(save);
            expect(submitted).toHaveLength(1);
            expect(save).toBeDisabled();
            expect(save).toHaveTextContent('Saving Daily Drive...');
            expect(screen.getByLabelText('Description')).toBeEnabled();
            expect(screen.getByLabelText('X Band')).toBeEnabled();

            pending[0](jsonResponse({ success: true }));
            await screen.findByText('Profile "Daily Drive" saved');
            expect(screen.getByLabelText('Description')).toHaveValue('Latest save');
            expect(screen.getByLabelText('X Band')).not.toBeChecked();
            const nextSave = screen.getByRole('button', { name: /^save profile$/i });
            expect(nextSave).toBeEnabled();
            await fireEvent.click(nextSave);
            await waitFor(() => expect(pending).toHaveLength(2));
            expect(submitted[1]).toMatchObject({
                name: 'Daily Drive', description: 'Latest save', settings: { xBand: false }
            });
            pending[1](jsonResponse({ success: true }));
            await waitFor(() => {
                expect(screen.getByText('Daily Drive').closest('.surface-panel'))
                    .toHaveTextContent('Latest save');
                expect(screen.queryByText('Editing profile: Daily Drive')).toBeNull();
            });
        } finally {
            for (const resolve of pending) resolve(jsonResponse({ success: true }));
            unmount();
        }
    });

    it.each(['editor', 'dialog'])('releases the %s save button when an error body stalls', async (entry) => {
        let signal;
        let body;
        installDefaultFetch([{
            method: 'GET', match: '/api/v1/profile?name=Daily%20Drive',
            respond: jsonResponse({ name: 'Daily Drive', settings: { xBand: true } })
        }, {
            method: 'POST', match: '/api/v1/profile',
            respond: ({ init }) => {
                signal = init.signal;
                return new Response(new ReadableStream({
                    start(controller) {
                        body = controller;
                        controller.enqueue(new TextEncoder().encode('Incomplete error'));
                        signal.addEventListener('abort', () => {
                            controller.error(new DOMException('Aborted', 'AbortError'));
                        }, { once: true });
                    }
                }), { status: 503 });
            }
        }]);
        const { unmount } = render(Page);
        try {
            const row = (await screen.findByText('Daily Drive')).closest('.surface-panel');
            let save;
            if (entry === 'editor') {
                await fireEvent.click(within(row).getByRole('button', { name: /^edit$/i }));
                await screen.findByText('Editing profile: Daily Drive');
                save = screen.getByRole('button', { name: /^save profile$/i });
            } else {
                await fireEvent.click(screen.getByRole('button', { name: /new profile/i }));
                await fireEvent.click(screen.getByRole('button', { name: /save as profile/i }));
                const modal = (await screen.findByText('Save Profile')).closest('.modal-box');
                await fireEvent.input(screen.getByLabelText('Profile Name'), {
                    target: { value: 'Timeout Profile' }
                });
                save = within(modal).getByRole('button', { name: /^save$/i });
            }
            vi.useFakeTimers();
            await fireEvent.click(save);
            await vi.advanceTimersByTimeAsync(0);
            expect(save).toBeDisabled();
            await vi.advanceTimersByTimeAsync(5000);
            expect(signal.aborted).toBe(true);
            await waitFor(() => {
                expect(screen.getByText('Connection error')).toBeInTheDocument();
                expect(save).toBeEnabled();
            });
        } finally {
            body?.error(new Error('Test finished'));
            vi.useRealTimers();
            unmount();
        }
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
        expect(screen.getByLabelText('Mute-to-Muted Volume')).toBeChecked();
        const autoMute = screen.getByLabelText('X, K, Ku Automute');
        expect(within(autoMute).getByRole('option', { name: 'On' })).toHaveValue('2');
        expect(within(autoMute).getByRole('option', { name: 'Advanced' })).toHaveValue('1');
        await fireEvent.change(autoMute, { target: { value: '2' } });
        await fireEvent.click(xBand);
        await fireEvent.click(screen.getByText('Photo Radar'));
        await fireEvent.click(screen.getByLabelText('DriveSafe™ 3D'));
        await fireEvent.click(screen.getByLabelText('DriveSafe™ 3DHD'));
        await fireEvent.click(screen.getByLabelText('Ekin'));
        await fireEvent.click(screen.getByLabelText('Gatso RT4'));
        await fireEvent.click(screen.getByLabelText('Intersection Management Filter'));
        await screen.findByText(
            'Intersection Management suppresses DriveSafe 3D, DriveSafe 3DHD, and Ekin alerts while enabled. Those saved settings are not changed.'
        );
        expect(screen.getByLabelText('DriveSafe™ 3D')).toBeChecked();
        expect(screen.getByLabelText('DriveSafe™ 3DHD')).toBeChecked();
        expect(screen.getByLabelText('Ekin')).toBeChecked();
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
        expect(savedPayload.settings.muteToMuteVolume).toBe(true);
        expect(savedPayload.settings.autoMute).toBe(2);
        expect(savedPayload.settings.kaSensitivity).toBe(3);
        expect(savedPayload.settings.driveSafe3D).toBe(true);
        expect(savedPayload.settings.driveSafe3DHD).toBe(true);
        expect(savedPayload.settings.ekin).toBe(true);
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
        expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/snapshot')).toBe(true);
        expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/pull')).toBe(false);
        expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/push')).toBe(false);

        unmount();
    });

    it('starts an offline draft from captured bytes without changing the detector', async () => {
        let savedPayload;
        installDefaultFetch([
            {
                method: 'POST', match: '/api/v1/profile', respond: ({ init }) => {
                    savedPayload = JSON.parse(init.body);
                    return jsonResponse({ success: true });
                }
            }
        ]);
        const { unmount } = render(Page);

        await fireEvent.click(await screen.findByRole('button', { name: /start draft from captured settings/i }));
        await screen.findByText('Draft started from the last observed V1 user bytes. No detector changes were made.');
        expect(screen.getByText('Creating new offline profile')).toBeInTheDocument();
        expect(screen.getByLabelText('X Band')).toBeChecked();

        await fireEvent.click(screen.getByLabelText('X Band'));
        await fireEvent.click(screen.getByRole('button', { name: /save as profile/i }));
        const modal = (await screen.findByText('Save Profile')).closest('.modal-box');
        await fireEvent.input(screen.getByLabelText('Profile Name'), {
            target: { value: 'Captured draft' }
        });
        await fireEvent.click(within(modal).getByRole('button', { name: /^save$/i }));
        await screen.findByText('Profile "Captured draft" saved');
        expect(savedPayload.settings.baseBytes).toEqual([255, 255, 255, 255, 255, 255]);
        expect(savedPayload.settings.xBand).toBe(false);
        unmount();
    });

    it('keeps partial capture fields explicitly unavailable', async () => {
        installDefaultFetch([{
            method: 'GET', match: '/api/v1/snapshot', respond: jsonResponse({
                available: true,
                firmware: { available: false, value: null },
                capabilities: { versionKnown: false },
                observations: {
                    userBytes: { available: false, value: null },
                    mode: { available: false, value: null },
                    displayOn: { available: false, value: null },
                    currentVolume: { available: false, main: null, muted: null },
                    savedVolume: { available: false, main: null, muted: null }
                },
                provenance: { captureTimedOut: true }
            })
        }]);
        const { unmount } = render(Page);

        await screen.findByText('The connection capture timed out. Available values are preserved; missing values remain unknown.');
        expect(screen.getByText('Firmware capabilities are unknown; the captured bytes are shown without feature claims.')).toBeInTheDocument();
        expect(screen.getByRole('button', { name: /start draft from captured settings/i })).toBeDisabled();
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
