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

function profileCatalog(profiles) {
    return {
        schemaVersion: 3,
        detectorConfigurationOwner: 'profile',
        profiles
    };
}

function emptyOperationComponents(overrides = {}) {
    const empty = {
        requested: false, sent: false, verified: false,
        outcome: 'not_requested', reason: 'none'
    };
    return Object.fromEntries(
        ['userSettings', 'display', 'mode', 'volume', 'customFrequencies', 'factoryReset']
            .map((name) => [name, { ...empty, ...(overrides[name] || {}) }])
    );
}

describe('profiles route page', () => {
    beforeEach(() => {
        global.confirm = vi.fn(() => true);
        const storage = new Map();
        Object.defineProperty(window, 'localStorage', {
            configurable: true,
            value: {
                getItem: (key) => storage.has(key) ? storage.get(key) : null,
                setItem: (key, value) => storage.set(key, String(value)),
                removeItem: (key) => storage.delete(key),
                clear: () => storage.clear()
            }
        });
        window.localStorage.clear();
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

    it('copies a profile under a new name with detector settings intact', async () => {
        let savedPayload;
        installDefaultFetch([
            {
                method: 'GET', match: '/api/v1/profile?name=Daily%20Drive',
                respond: jsonResponse({
                    schemaVersion: 3, name: 'Daily Drive', description: 'Existing metadata',
                    detector: {
                        userSettings: 'value', mode: { policy: 'value', value: 2 },
                        display: 'off', volume: { policy: 'saved', main: 7, muted: 2 },
                        customFrequencies: { policy: 'unchanged' }
                    },
                    settings: { xBand: true }
                })
            },
            {
                method: 'POST', match: '/api/v1/profile',
                respond: ({ init }) => {
                    savedPayload = JSON.parse(init.body);
                    return jsonResponse({ success: true });
                }
            }
        ]);
        const { unmount } = render(Page);

        const row = (await screen.findByText('Daily Drive')).closest('.surface-panel');
        await fireEvent.click(within(row).getByRole('button', { name: 'Copy' }));
        expect(await screen.findByLabelText('Profile Name')).toHaveValue('Daily Drive copy');
        expect(screen.getByLabelText('Main volume (0–9)')).toHaveValue(7);
        const modal = screen.getByText('Save Profile').closest('.modal-box');
        await fireEvent.click(within(modal).getByRole('button', { name: 'Save' }));

        await screen.findByText('Profile "Daily Drive copy" saved');
        expect(savedPayload.name).toBe('Daily Drive copy');
        expect(savedPayload.description).toBe('Existing metadata');
        expect(savedPayload.detector.display).toBe('off');
        expect(savedPayload.detector.volume).toMatchObject({ policy: 'saved', main: 7, muted: 2 });
        unmount();
    });

    it('preserves detector policy when editing a saved profile', async () => {
        let savedPayload;
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/v1/profile?name=Daily%20Drive',
                respond: jsonResponse({
                    schemaVersion: 3,
                    name: 'Daily Drive',
                    description: 'Existing metadata',
                    detector: {
                        userSettings: 'value',
                        mode: { policy: 'value', value: 2 },
                        display: 'off',
                        volume: {
                            policy: 'temporary',
                            main: 7,
                            muted: 2,
                            feedback: 'none',
                            disconnect: 'restore_saved'
                        },
                        bluetoothLed: 'unchanged',
                        customFrequencies: { policy: 'unchanged' }
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
        expect(savedPayload.schemaVersion).toBe(3);
        expect(savedPayload.detector).toEqual({
            userSettings: 'value',
            mode: { policy: 'value', value: 2 },
            display: 'off',
            volume: {
                policy: 'temporary',
                main: 7,
                muted: 2,
                feedback: 'none',
                disconnect: 'restore_saved'
            },
            bluetoothLed: 'unchanged',
            customFrequencies: { policy: 'unchanged' }
        });
        expect(savedPayload).not.toHaveProperty('displayOn');
        expect(savedPayload).not.toHaveProperty('mainVolume');
        unmount();
    });

    it('requires at least one authored band for an enabled legacy custom-frequency setting', async () => {
        installDefaultFetch([{
            method: 'GET',
            match: '/api/v1/profile?name=Daily%20Drive',
            respond: jsonResponse({
                schemaVersion: 3,
                name: 'Daily Drive',
                detector: { customFrequencies: { policy: 'unchanged' } },
                settings: { customFreqs: true }
            })
        }]);
        const { unmount } = render(Page);

        const dailyDriveRow = (await screen.findByText('Daily Drive')).closest('.surface-panel');
        await fireEvent.click(within(dailyDriveRow).getByRole('button', { name: /^edit$/i }));

        expect(await screen.findByText(/cannot be enabled until this profile owns at least one K or Ka range/i))
            .toBeInTheDocument();
        await fireEvent.click(screen.getByRole('button', { name: /add k range/i }));
        expect(screen.getByLabelText('Custom 0 lower MHz')).toHaveValue(23910);
        expect(screen.queryByText(/cannot be enabled until this profile owns/i)).not.toBeInTheDocument();
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

        await screen.findByText(/profile migration is pending/i);
        expect(screen.queryByRole('button', { name: /new profile/i })).not.toBeInTheDocument();
        expect(screen.queryByRole('button', { name: /^edit$/i })).not.toBeInTheDocument();
        expect(screen.getByRole('button', { name: /^delete$/i })).toBeInTheDocument();
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
                { method: 'GET', match: '/api/v1/profiles', respond: jsonResponse(profileCatalog(profiles)) },
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

    it('keeps detector edits made while an earlier profile save is pending', async () => {
        const pending = [];
        const submitted = [];
        installDefaultFetch([
            {
                method: 'GET', match: '/api/v1/profile?name=Daily%20Drive',
                respond: jsonResponse({
                    name: 'Daily Drive',
                    description: 'Detector draft',
                    detector: {
                        userSettings: 'value',
                        mode: { policy: 'unchanged' },
                        display: 'unchanged',
                        volume: {
                            policy: 'temporary', main: 3, muted: 1,
                            feedback: 'none', disconnect: 'restore_saved'
                        },
                        bluetoothLed: 'unchanged',
                        customFrequencies: {
                            policy: 'value',
                            definitions: [
                                { index: 0, lowerMHz: 24050, upperMHz: 24100 },
                                { index: 1, lowerMHz: 34000, upperMHz: 34100 }
                            ]
                        }
                    },
                    settings: { xBand: true }
                })
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
            const save = screen.getByRole('button', { name: /^save profile$/i });
            await fireEvent.click(save);
            await waitFor(() => expect(pending).toHaveLength(1));

            await fireEvent.change(screen.getByLabelText('Volume policy'), {
                target: { value: 'saved' }
            });
            await fireEvent.input(screen.getByLabelText('Custom 0 lower MHz'), {
                target: { value: '24060' }
            });
            expect(submitted[0].detector).toMatchObject({
                volume: { policy: 'temporary', main: 3, muted: 1 },
                customFrequencies: {
                    policy: 'value',
                    definitions: [
                        { index: 0, lowerMHz: 24050, upperMHz: 24100 },
                        { index: 1, lowerMHz: 34000, upperMHz: 34100 }
                    ]
                }
            });

            pending[0](jsonResponse({ success: true }));
            await screen.findByText('Profile "Daily Drive" saved');
            expect(screen.getByText('Editing profile: Daily Drive')).toBeInTheDocument();
            expect(screen.getByLabelText('Volume policy')).toHaveValue('saved');
            expect(screen.getByLabelText('Custom 0 lower MHz')).toHaveValue(24060);

            await fireEvent.click(screen.getByRole('button', { name: /^save profile$/i }));
            await waitFor(() => expect(pending).toHaveLength(2));
            expect(submitted[1].detector).toMatchObject({
                volume: { policy: 'saved', main: 3, muted: 1 },
                customFrequencies: {
                    policy: 'value',
                    definitions: [
                        { index: 0, lowerMHz: 24060, upperMHz: 24100 },
                        { index: 1, lowerMHz: 34000, upperMHz: 34100 }
                    ]
                }
            });
            pending[1](jsonResponse({ success: true }));
            await waitFor(() => {
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
                respond: jsonResponse(profileCatalog([{ name: 'Bench Profile' }]))
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

    it('enforces the firmware 64-byte UTF-8 name limit', async () => {
        const fetchMock = installDefaultFetch([
            { method: 'POST', match: '/api/v1/profile', respond: jsonResponse({ success: true }) }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('V1 Profiles');
        await fireEvent.click(screen.getByRole('button', { name: /new profile/i }));
        await fireEvent.click(screen.getByRole('button', { name: /save as profile/i }));
        const modal = (await screen.findByText('Save Profile')).closest('.modal-box');
        await fireEvent.input(screen.getByLabelText('Profile Name'), {
            target: { value: 'é'.repeat(33) }
        });
        await fireEvent.click(within(modal).getByRole('button', { name: /^Save$/i }));
        await screen.findByText('Profile name exceeds 64 UTF-8 bytes');
        expect(fetchMock.mock.calls.some(([url, init]) => url === '/api/v1/profile' && init?.method === 'POST'))
            .toBe(false);

        await fireEvent.input(screen.getByLabelText('Profile Name'), {
            target: { value: 'é'.repeat(32) }
        });
        await fireEvent.click(within(modal).getByRole('button', { name: /^Save$/i }));
        await screen.findByText(`Profile "${'é'.repeat(32)}" saved`);
        unmount();
    });

    it('uses firmware ASCII-only case folding for collision checks', async () => {
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/v1/profiles',
                respond: jsonResponse(profileCatalog([{ name: 'É', description: '' }]))
            },
            { method: 'POST', match: '/api/v1/profile', respond: jsonResponse({ success: true }) }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('É');
        await fireEvent.click(screen.getByRole('button', { name: /new profile/i }));
        await fireEvent.click(screen.getByRole('button', { name: /save as profile/i }));
        const modal = (await screen.findByText('Save Profile')).closest('.modal-box');
        await fireEvent.input(screen.getByLabelText('Profile Name'), { target: { value: 'é' } });
        await fireEvent.click(within(modal).getByRole('button', { name: /^Save$/i }));

        await screen.findByText('Profile "é" saved');
        expect(fetchMock.mock.calls.some(([url, init]) => url === '/api/v1/profile' && init?.method === 'POST'))
            .toBe(true);
        unmount();
    });

    it('creates and saves a V1 profile while disconnected', async () => {
        let savedPayload;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/v1/profiles',
                respond: () =>
                    jsonResponse(profileCatalog(savedPayload ? [{ name: savedPayload.name }] : []))
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
        expect(savedPayload.detector.customFrequencies).toEqual({
            policy: 'value',
            definitions: [
                { index: 0, lowerMHz: 23910, upperMHz: 24250 },
                { index: 1, lowerMHz: 33400, upperMHz: 36002 }
            ]
        });
        unmount();
    });

    it('presents the seven profile sections in the screenshot order without dropping extra controls', async () => {
        installDefaultFetch();
        const { unmount } = render(Page);

        await fireEvent.click(await screen.findByRole('button', { name: /new profile/i }));
        const editor = (await screen.findByText('Creating new offline profile')).closest('.surface-card');
        const sectionNames = [...editor.querySelectorAll('summary')]
            .map((summary) => summary.textContent.trim());

        expect(sectionNames).toEqual([
            'Bands',
            'Mute Control',
            'Photo Radar',
            'Special',
            'SAVVY Settings',
            'Custom Frequencies',
            'In-the-Box Options'
        ]);
        expect(within(screen.getByText('Bands').closest('details')).getByLabelText('Ku Band'))
            .toBeInTheDocument();
        const photo = screen.getByText('Photo Radar').closest('details');
        expect(within(photo).getByLabelText('DriveSafe™ 3D')).toBeInTheDocument();
        expect(within(photo).getByLabelText('Ekin')).toBeInTheDocument();

        unmount();
    });

    it('keeps the custom-frequency enable and authored ranges together while disabled', async () => {
        installDefaultFetch();
        const { unmount } = render(Page);

        await fireEvent.click(await screen.findByRole('button', { name: /new profile/i }));
        const custom = screen.getByText('Custom Frequencies').closest('details');
        await fireEvent.click(custom.querySelector('summary'));

        const enabled = within(custom).getByLabelText('Enable Custom Frequencies');
        const kLower = within(custom).getByLabelText('Custom 0 lower MHz');
        const kaLower = within(custom).getByLabelText('Custom 1 lower MHz');
        expect(enabled).not.toBeChecked();
        expect(kLower).toHaveValue(23910);
        expect(kaLower).toHaveValue(33400);
        expect(kLower).toBeEnabled();
        expect(kaLower).toBeEnabled();

        await fireEvent.click(enabled);
        await fireEvent.click(enabled);
        expect(enabled).not.toBeChecked();
        expect(kLower).toHaveValue(23910);
        expect(kaLower).toHaveValue(33400);

        unmount();
    });

    it('labels SAVVY and In-the-Box as unavailable without presenting fake controls', async () => {
        installDefaultFetch();
        const { unmount } = render(Page);

        await fireEvent.click(await screen.findByRole('button', { name: /new profile/i }));
        const savvy = screen.getByText('SAVVY Settings').closest('details');
        const inTheBox = screen.getByText('In-the-Box Options').closest('details');
        await fireEvent.click(savvy.querySelector('summary'));
        await fireEvent.click(inTheBox.querySelector('summary'));

        expect(within(savvy).getByText(/SAVVY accessory controls are unavailable/i))
            .toBeInTheDocument();
        expect(within(inTheBox).getByText(/In-the-Box profile controls are unavailable/i))
            .toBeInTheDocument();
        expect(savvy.querySelector('.collapse-content input, .collapse-content select, .collapse-content button'))
            .toBeNull();
        expect(inTheBox.querySelector('.collapse-content input, .collapse-content select, .collapse-content button'))
            .toBeNull();

        unmount();
    });

    it('does not present the per-slot persistence overlay as Valentine profile Alert Persistence', async () => {
        installDefaultFetch();
        const { unmount } = render(Page);

        await fireEvent.click(await screen.findByRole('button', { name: /new profile/i }));
        const special = screen.getByText('Special').closest('details');
        await fireEvent.click(special.querySelector('summary'));

        expect(within(special).getByText(/Valentine profile Alert Persistence is not mapped yet/i))
            .toBeInTheDocument();
        expect(within(special).queryByRole('checkbox', { name: /Alert Persistence/i }))
            .not.toBeInTheDocument();

        unmount();
    });

    it.each([
        {
            label: 'K-only',
            removeIndex: 1,
            relinquishedBand: 'Ka',
            expectedDefinition: { index: 0, lowerMHz: 23910, upperMHz: 24250 }
        },
        {
            label: 'Ka-only',
            removeIndex: 0,
            relinquishedBand: 'K',
            expectedDefinition: { index: 0, lowerMHz: 33400, upperMHz: 36002 }
        }
    ])('saves a $label authored set and takes the other band from the fresh DUT table', async ({
        label, removeIndex, relinquishedBand, expectedDefinition
    }) => {
        let savedPayload;
        installDefaultFetch([{
            method: 'POST', match: '/api/v1/profile', respond: ({ init }) => {
                savedPayload = JSON.parse(init.body);
                return jsonResponse({ success: true });
            }
        }]);
        const { unmount } = render(Page);

        await fireEvent.click(await screen.findByRole('button', { name: /new profile/i }));
        const custom = screen.getByText('Custom Frequencies').closest('details');
        const remove = within(custom).getByRole('button', {
            name: new RegExp(`remove custom frequency ${removeIndex} and relinquish ${relinquishedBand} ownership`, 'i')
        });
        await fireEvent.click(remove);
        expect(within(custom).getByText('Fresh DUT ranges on Apply')).toBeInTheDocument();
        expect(within(custom).getByText('Profile-owned')).toBeInTheDocument();

        await fireEvent.click(screen.getByRole('button', { name: /save as profile/i }));
        const modal = (await screen.findByText('Save Profile')).closest('.modal-box');
        await fireEvent.input(screen.getByLabelText('Profile Name'), {
            target: { value: `${label} frequencies` }
        });
        await fireEvent.click(within(modal).getByRole('button', { name: /^save$/i }));

        await screen.findByText(`Profile "${label} frequencies" saved`);
        expect(savedPayload.detector.customFrequencies).toEqual({
            policy: 'value',
            definitions: [expectedDefinition]
        });
        unmount();
    });

    it('blocks enabling Custom Frequencies when the profile owns no ranges', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await fireEvent.click(await screen.findByRole('button', { name: /new profile/i }));
        const custom = screen.getByText('Custom Frequencies').closest('details');
        await fireEvent.click(within(custom).getByRole('button', { name: /remove custom frequency 1/i }));
        await fireEvent.click(within(custom).getByRole('button', { name: /remove custom frequency 0/i }));
        expect(within(custom).getAllByText('Fresh DUT ranges on Apply')).toHaveLength(2);

        await fireEvent.click(within(custom).getByLabelText('Enable Custom Frequencies'));
        expect(await within(custom).findByText(
            'Custom Frequencies cannot be enabled until this profile owns at least one K or Ka range.'
        )).toBeInTheDocument();

        await fireEvent.click(screen.getByRole('button', { name: /save as profile/i }));
        const modal = (await screen.findByText('Save Profile')).closest('.modal-box');
        await fireEvent.input(screen.getByLabelText('Profile Name'), { target: { value: 'No ranges' } });
        await fireEvent.click(within(modal).getByRole('button', { name: /^save$/i }));
        expect(fetchMock.mock.calls.some(([url, init]) =>
            url === '/api/v1/profile' && init?.method === 'POST')).toBe(false);
        unmount();
    });

    it.each([
        ['crosses the K/Ka gap', 29900, 30100],
        ['sits above the published K section', 25000, 26000],
        ['sits below the published Ka section', 30000, 30100]
    ])('rejects a range that %s before ownership inference', async (_, lowerMHz, upperMHz) => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await fireEvent.click(await screen.findByRole('button', { name: /new profile/i }));
        const custom = screen.getByText('Custom Frequencies').closest('details');
        await fireEvent.input(within(custom).getByLabelText('Custom 0 lower MHz'), {
            target: { value: String(lowerMHz) }
        });
        await fireEvent.input(within(custom).getByLabelText('Custom 0 upper MHz'), {
            target: { value: String(upperMHz) }
        });

        expect(await within(custom).findByText(
            'Every range must fit inside the published Gen2 K or Ka sweep section.'
        )).toBeInTheDocument();
        expect(within(custom).getByText('Invalid')).toBeInTheDocument();

        await fireEvent.click(screen.getByRole('button', { name: /save as profile/i }));
        const modal = (await screen.findByText('Save Profile')).closest('.modal-box');
        await fireEvent.input(screen.getByLabelText('Profile Name'), { target: { value: 'Crossed range' } });
        await fireEvent.click(within(modal).getByRole('button', { name: /^save$/i }));
        expect(fetchMock.mock.calls.some(([url, init]) =>
            url === '/api/v1/profile' && init?.method === 'POST')).toBe(false);
        unmount();
    });

    it('uses the confirmed save to update a catalog that was initially stale', async () => {
        installDefaultFetch([
            { method: 'GET', match: '/api/v1/profiles', respond: jsonResponse(profileCatalog([])) },
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
        expect(screen.getByText(/start a target-bound operation/i)).toBeInTheDocument();

        await fireEvent.click(screen.getByRole('button', { name: /new profile/i }));
        await screen.findByText('Creating new offline profile');
        expect(screen.queryByRole('button', { name: /push to v1/i })).not.toBeInTheDocument();
        expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/current')).toBe(false);
        expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/snapshot')).toBe(true);
        expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/pull')).toBe(false);
        expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/push')).toBe(false);

        unmount();
    });

    it('queues the saved profile for the exact captured detector and retains its operation id', async () => {
        let submitted;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET', match: '/api/v1/profile?name=Daily%20Drive',
                respond: jsonResponse({ name: 'Daily Drive', settings: { xBand: true } })
            },
            {
                method: 'POST', match: '/api/v1/apply', respond: ({ init }) => {
                    submitted = init;
                    return jsonResponse({
                        success: true, queued: true, operationId: 42,
                        state: 'pending_normal_boot', rebooting: true, target: 'normal'
                    }, 202);
                }
            }
        ]);
        const { unmount } = render(Page);
        const row = (await screen.findByText('Daily Drive')).closest('.surface-panel');
        await fireEvent.click(within(row).getByRole('button', { name: /^edit$/i }));
        await screen.findByText('Editing profile: Daily Drive');
        await fireEvent.click(screen.getByRole('button', {
            name: /apply saved profile to captured v1/i
        }));

        await screen.findByText(/queued as operation 42/i);
        expect(submitted.body).toBeInstanceOf(URLSearchParams);
        expect(submitted.body.get('profile')).toBe('Daily Drive');
        expect(submitted.body.get('address')).toBe('AA:BB:CC:DD:EE:FF');
        expect(submitted.headers['X-V1Simple-Request']).toBe('maintenance-ui');
        expect(window.localStorage.getItem('v1simple.detectorSettingsOperationId')).toBe('42');
        expect(fetchMock).toHaveBeenCalled();
        unmount();
    });

    it('does not let an older poll response clobber a newly accepted profile operation', async () => {
        window.localStorage.setItem('v1simple.detectorSettingsOperationId', '41');
        let resolveOldPoll;
        installDefaultFetch([
            {
                method: 'GET', match: '/api/autopush/status?operationId=41',
                respond: () => new Promise((resolve) => { resolveOldPoll = resolve; })
            },
            {
                method: 'GET', match: '/api/v1/profile?name=Daily%20Drive',
                respond: jsonResponse({ name: 'Daily Drive', settings: { xBand: true } })
            },
            {
                method: 'POST', match: '/api/v1/apply',
                respond: jsonResponse({
                    success: true, queued: true, operationId: 42,
                    state: 'pending_normal_boot', rebooting: true, target: 'normal'
                }, 202)
            }
        ]);
        const { unmount } = render(Page);
        const row = (await screen.findByText('Daily Drive')).closest('.surface-panel');
        await fireEvent.click(within(row).getByRole('button', { name: /^edit$/i }));
        await screen.findByText('Editing profile: Daily Drive');
        await fireEvent.click(screen.getByRole('button', {
            name: /apply saved profile to captured v1/i
        }));
        await screen.findByText(/queued as operation 42/i);

        resolveOldPoll(jsonResponse({
            error: 'operation_mismatch', requestedOperationId: 41, currentOperationId: 42
        }, 409));
        await Promise.resolve();
        await Promise.resolve();
        expect(window.localStorage.getItem('v1simple.detectorSettingsOperationId')).toBe('42');
        expect(screen.getByText('Detector operation 42')).toBeInTheDocument();
        expect(screen.queryByText(/Saved operation 41 does not match/i)).not.toBeInTheDocument();
        unmount();
    });

    it('resumes and renders only the exact durable terminal operation', async () => {
        window.localStorage.setItem('v1simple.detectorSettingsOperationId', '42');
        const fetchMock = installDefaultFetch([{
            method: 'GET',
            match: '/api/autopush/status?operationId=42',
            respond: jsonResponse({
                operationId: 42,
                kind: 'apply_profile',
                source: 'maintenance_ui',
                profileName: 'Daily Drive',
                targetAddress: 'AA:BB:CC:DD:EE:FF',
                returnToMaintenance: false,
                state: 'partial',
                reason: 'recapture_timed_out',
                terminal: true,
                result: 'partial',
                components: emptyOperationComponents({
                    volume: {
                        requested: true, sent: true, verified: false,
                        outcome: 'timeout', reason: 'volume_timeout'
                    }
                })
            })
        }]);
        const { unmount } = render(Page);

        await screen.findByText('Detector operation 42');
        expect(screen.getByText('apply_profile · target AA:BB:CC:DD:EE:FF · source maintenance_ui'))
            .toBeInTheDocument();
        expect(screen.getByText('partial')).toBeInTheDocument();
        expect(screen.getByText('timeout · volume_timeout')).toBeInTheDocument();
        expect(fetchMock.mock.calls.some(([url]) =>
            url === '/api/autopush/status?operationId=42')).toBe(true);
        expect(window.localStorage.getItem('v1simple.detectorSettingsOperationId')).toBeNull();
        unmount();
    });

    it('rejects a stale operation identity instead of rendering another job', async () => {
        window.localStorage.setItem('v1simple.detectorSettingsOperationId', '42');
        installDefaultFetch([{
            method: 'GET', match: '/api/autopush/status?operationId=42',
            respond: jsonResponse({
                error: 'operation_mismatch', requestedOperationId: 42, currentOperationId: 43
            }, 409)
        }]);
        const { unmount } = render(Page);
        await screen.findByText(/does not match the detector's current operation/i);
        expect(screen.queryByLabelText('Detector operation status')).not.toBeInTheDocument();
        expect(window.localStorage.getItem('v1simple.detectorSettingsOperationId')).toBeNull();
        unmount();
    });

    it('rejects malformed component truth instead of rendering false success', async () => {
        window.localStorage.setItem('v1simple.detectorSettingsOperationId', '42');
        installDefaultFetch([{
            method: 'GET', match: '/api/autopush/status?operationId=42',
            respond: jsonResponse({
                operationId: 42, kind: 'apply_profile', source: 'maintenance_ui',
                profileName: 'Daily Drive', targetAddress: 'AA:BB:CC:DD:EE:FF',
                returnToMaintenance: false, state: 'succeeded', reason: 'none',
                terminal: true, result: 'succeeded',
                components: emptyOperationComponents({ volume: {
                    requested: true, sent: 'yes', verified: true,
                    outcome: 'verified', reason: 'none'
                } })
            })
        }]);
        const { unmount } = render(Page);
        await screen.findByText(/malformed or mismatched status/i);
        expect(screen.queryByLabelText('Detector operation status')).not.toBeInTheDocument();
        unmount();
    });

    it('requires explicit factory-reset text and submits the exact captured target', async () => {
        let submitted;
        installDefaultFetch([
            {
                method: 'GET', match: '/api/v1/snapshot', respond: jsonResponse({
                    address: 'AA:BB:CC:DD:EE:FF', available: true,
                    firmware: { available: true, value: 41039 },
                    capabilities: {
                        versionKnown: true, gen2: true,
                        detectorFactoryResetWorkflowAvailable: true,
                        supportedUserByteCount: 6, savedVolume: true, settings: {}
                    },
                    observations: { userBytes: { available: true, value: [255,255,255,255,255,255] } },
                    settings: { bytes: [255,255,255,255,255,255] }
                })
            },
            {
                method: 'POST', match: '/api/v1/factory-reset', respond: ({ init }) => {
                    submitted = init;
                    return jsonResponse({
                        success: true, queued: true, operationId: 77,
                        state: 'pending_normal_boot', rebooting: true, target: 'normal'
                    }, 202);
                }
            }
        ]);
        const { unmount } = render(Page);
        const reset = await screen.findByRole('button', { name: /factory reset captured v1/i });
        expect(reset).toBeDisabled();
        await fireEvent.input(screen.getByLabelText(/type reset v1 to confirm/i), {
            target: { value: 'RESET V1' }
        });
        expect(reset).toBeEnabled();
        await fireEvent.click(reset);
        await screen.findByText(/queued as operation 77/i);
        expect(submitted.body.get('address')).toBe('AA:BB:CC:DD:EE:FF');
        expect(submitted.body.get('confirm')).toBe('RESET V1');
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

        expect(await screen.findByText(/prefills the observed current numbers, but leaves volume unchanged/i))
            .toBeInTheDocument();
        await fireEvent.click(await screen.findByRole('button', { name: /start draft from captured settings/i }));
        await screen.findByText('Draft started from the last observed V1 user bytes. No detector changes were made.');
        expect(screen.getByText('Creating new offline profile')).toBeInTheDocument();
        expect(screen.getByLabelText('X Band')).toBeChecked();
        expect(screen.getByLabelText('Volume policy')).toHaveValue('unchanged');

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
                address: 'AA:BB:CC:DD:EE:FF',
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

    it('does not present future firmware as protocol-qualified', async () => {
        installDefaultFetch([{
            method: 'GET', match: '/api/v1/snapshot', respond: jsonResponse({
                available: true,
                address: 'AA:BB:CC:DD:EE:FF',
                firmware: { available: true, value: 50000 },
                capabilities: {
                    versionKnown: true,
                    gen2: false,
                    supportedUserByteCount: 0
                },
                observations: {
                    userBytes: { available: true, value: [255, 255, 255, 255, 255, 255] }
                },
                provenance: { captureTimedOut: false }
            })
        }]);
        const { unmount } = render(Page);

        await screen.findByText('This firmware version is outside the qualified Gen2 range; captured bytes are shown without feature claims.');
        expect(screen.queryByText(/Firmware-qualified:/)).toBeNull();
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
