import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

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
            {
                method: 'POST',
                match: '/api/autopush/activate',
                respond: jsonResponse({ success: true })
            },
            {
                method: 'POST',
                match: '/api/autopush/push',
                respond: jsonResponse({ success: true })
            },
            {
                method: 'POST',
                match: '/api/autopush/slot',
                respond: jsonResponse({ success: true })
            }
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
        await screen.findByText('Global default');

        await fireEvent.click(screen.getAllByRole('button', { name: /^edit$/i })[0]);

        expect(await screen.findByLabelText('Profile')).toBeInTheDocument();
        expect(screen.getByText('Alert persistence (seconds)')).toBeInTheDocument();
        expect(screen.getByRole('button', { name: /^save$/i })).toBeInTheDocument();

        unmount();
    });

    it('keeps the slot editor open when saving a slot fails', async () => {
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/autopush/slot',
                respond: jsonResponse({ error: 'bad save' }, 500)
            }
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
            {
                method: 'POST',
                match: '/api/autopush/activate',
                respond: jsonResponse({ error: 'bad activate' }, 500)
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Highway');
        await fireEvent.click(screen.getAllByRole('button', { name: /^activate$/i })[0]);

        await screen.findByText('Failed to activate');
        expect(screen.getByText('Highway')).toBeInTheDocument();
        expect(screen.getAllByText('Global default')).toHaveLength(1);
        expect(screen.getAllByRole('button', { name: /^activate$/i })).toHaveLength(2);

        unmount();
    });

    it('announces activation with the 1-based slot number', async () => {
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/autopush/activate',
                respond: jsonResponse({ success: true })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Highway');
        await fireEvent.click(screen.getAllByRole('button', { name: /^activate$/i })[0]);

        // Slot index 0 must announce as "Slot 1" (regression pin: toasts were
        // 0-based in an earlier build).
        await screen.findByText('Slot 1 activated');
        expect(screen.getAllByText('Global default')).toHaveLength(1);

        unmount();
    });

    it('shows an error when profiles fail to load', async () => {
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/v1/profiles',
                respond: jsonResponse({ error: 'bad profiles' }, 500)
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Failed to load profiles');
        await screen.findByText('Highway');
        expect(screen.getByText('Auto-Push Profiles')).toBeInTheDocument();
        expect(screen.getByText('Global default')).toBeInTheDocument();

        unmount();
    });

    it('labels the active slot as the global default and explains device overrides', async () => {
        installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('Global default');
        expect(
            screen.getByText(
                'Auto-Push sends V1 settings when you connect during normal runtime. The global default slot is used unless a saved V1 device override selects another slot.'
            )
        ).toBeInTheDocument();

        unmount();
    });

    it('disables live pushes but keeps saved slot configuration available in maintenance', async () => {
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/status',
                respond: jsonResponse({ maintenanceBoot: true, maintenanceBootUptimeMs: 9000 })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText(
            'Live V1 pushes are unavailable in maintenance mode. You can still edit, save, and select the global default slot for normal runtime.'
        );
        expect(screen.getAllByRole('button', { name: /push now/i })).toHaveLength(3);
        for (const button of screen.getAllByRole('button', { name: /push now/i })) {
            expect(button).toBeDisabled();
        }
        for (const button of screen.getAllByRole('button', { name: /^activate$/i })) {
            expect(button).toBeEnabled();
        }

        await fireEvent.click(screen.getAllByRole('button', { name: /^activate$/i })[0]);
        await screen.findByText('Slot 1 activated');
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === '/api/autopush/activate' && init?.method === 'POST'
            )
        ).toBe(true);

        await fireEvent.click(screen.getAllByRole('button', { name: /^edit$/i })[0]);
        expect(screen.getByRole('button', { name: /^save$/i })).toBeEnabled();
        await fireEvent.click(screen.getByRole('button', { name: /^save$/i }));
        await screen.findByText('Slot saved!');
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === '/api/autopush/slot' && init?.method === 'POST'
            )
        ).toBe(true);

        unmount();
    });

    it('defensively refuses a live push without posting in maintenance mode', async () => {
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/status',
                respond: jsonResponse({ maintenanceBoot: true, maintenanceBootUptimeMs: 9000 })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText(
            'Live V1 pushes are unavailable in maintenance mode. You can still edit, save, and select the global default slot for normal runtime.'
        );
        const pushButton = screen.getAllByRole('button', { name: /push now/i })[0];
        expect(pushButton).toBeDisabled();

        // Exercise the handler guard independently from the disabled UI state.
        pushButton.disabled = false;
        await fireEvent.click(pushButton);
        await screen.findByText(
            'Push Now is unavailable in maintenance mode; no settings were sent to the V1.'
        );
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === '/api/autopush/push' && init?.method === 'POST'
            )
        ).toBe(false);

        unmount();
    });

    it('fails closed when runtime mode cannot be verified', async () => {
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/status',
                respond: jsonResponse({ error: 'status unavailable' }, 503)
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText(
            'Live V1 pushes are unavailable because device runtime mode could not be verified.'
        );
        await screen.findByText('Highway');
        const pushButtons = screen.getAllByRole('button', { name: /push now/i });
        for (const button of pushButtons) {
            expect(button).toBeDisabled();
        }

        // Exercise the handler guard independently from the disabled UI state.
        pushButtons[0].disabled = false;
        await fireEvent.click(pushButtons[0]);
        await screen.findByText(
            'Push Now is unavailable until device runtime mode can be verified.'
        );
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === '/api/autopush/push' && init?.method === 'POST'
            )
        ).toBe(false);

        unmount();
    });

    it('prefers a backend human message when a live push fails', async () => {
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/autopush/push',
                respond: jsonResponse(
                    {
                        error: 'v1_not_connected',
                        message: 'Connect the V1 before pushing settings.'
                    },
                    409
                )
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Highway');
        const pushButton = screen.getAllByRole('button', { name: /push now/i })[0];
        await waitFor(() => expect(pushButton).toBeEnabled());
        await fireEvent.click(pushButton);

        await screen.findByText('Connect the V1 before pushing settings.');
        expect(screen.queryByText('v1_not_connected')).not.toBeInTheDocument();

        unmount();
    });
});
