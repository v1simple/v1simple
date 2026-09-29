import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { installFixtureFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';
import wifiApiFixtures from '../../test/fixtures/wifi-api.json';

function installDefaultFetch(overrides = []) {
    return installFixtureFetchMock(
        ['frontend_core_routes', 'autopush_routes', 'v1_profile_routes'],
        [
            ...overrides,
            { method: 'GET', match: '/api/autopush/slots', respond: jsonResponse({
                ...wifiApiFixtures.scenarios.autopush_routes['GET /api/autopush/slots'][0].body,
                schemaVersion: 4
            }) }
        ]
    );
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

const maintenanceStatus = {
    method: 'GET', match: '/api/status',
    respond: jsonResponse({ maintenanceBoot: true, maintenanceBootUptimeMs: 9000 })
};

const queuedOperation = {
    method: 'POST', match: '/api/autopush/push',
    respond: jsonResponse({
        success: true, queued: true, operationId: 42,
        state: 'pending_normal_boot', rebooting: true, target: 'normal'
    }, 202)
};

function pendingJson(signal) {
    let controller;
    const abort = () => controller.error(new DOMException('Aborted', 'AbortError'));
    return new Response(new ReadableStream({
        start(streamController) {
            controller = streamController;
            controller.enqueue(new TextEncoder().encode('{'));
            signal.addEventListener('abort', abort, { once: true });
        }
    }));
}

describe('autopush route page', () => {
    beforeEach(() => {
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
    });

    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
    });

    it('surfaces a non-OK slots response instead of showing a silent empty page', async () => {
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/autopush/slots',
                respond: jsonResponse({ error: 'slots unavailable' }, 503)
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Failed to load slots');
        expect(screen.queryByText('Global default')).not.toBeInTheDocument();

        unmount();
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
        expect(screen.getByRole('button', { name: /^save slot$/i })).toBeInTheDocument();
        expect(screen.getByLabelText('Slot name')).toHaveValue('Default');
        expect(screen.getByLabelText('Alert persistence value (seconds)')).toHaveValue(1);
        expect(screen.getByText('Override profile settings').closest('details')).not.toHaveAttribute('open');

        unmount();
    });

    it('assigns and saves a visible color for a named slot', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('Highway');
        await fireEvent.click(screen.getAllByRole('button', { name: /^edit$/i })[0]);
        await fireEvent.click(screen.getByRole('button', { name: 'Choose Default color' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Blue' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
        await fireEvent.click(screen.getByRole('button', { name: /^save slot$/i }));

        await screen.findByText('Slot saved!');
        const saveCall = fetchMock.mock.calls.find(
            ([url, init]) => url === '/api/autopush/slot' && init?.method === 'POST'
        );
        expect(saveCall[1].body.get('color')).toBe('31');
        expect(saveCall[1].body.get('name')).toBe('DEFAULT');
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
        await fireEvent.click(await screen.findByRole('button', { name: /^save slot$/i }));

        await screen.findByText('Failed to save');
        expect(screen.getByRole('button', { name: /^save slot$/i })).toBeInTheDocument();
        expect(screen.getByLabelText('Profile')).toBeInTheDocument();

        unmount();
    });

    it('shows a volume-policy conflict returned by the device', async () => {
        installDefaultFetch([{
            method: 'POST', match: '/api/autopush/slot',
            respond: jsonResponse({ error: 'Volume override requires a profile volume policy' }, 409)
        }]);
        const { unmount } = render(Page);

        await screen.findByText('Highway');
        await fireEvent.click(screen.getAllByRole('button', { name: /^edit$/i })[0]);
        await fireEvent.click(screen.getByRole('button', { name: /^save slot$/i }));

        await screen.findByText('Volume override requires a profile volume policy');
        expect(screen.getByRole('button', { name: /^save slot$/i })).toBeInTheDocument();
        unmount();
    });

    it('submits mixed-case slot names in the firmware ASCII-uppercase form', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('Highway');
        await fireEvent.click(screen.getAllByRole('button', { name: /^edit$/i })[0]);
        await fireEvent.input(screen.getByLabelText('Slot name'), {
            target: { value: 'Commute é' }
        });
        await fireEvent.click(screen.getByRole('button', { name: /^save slot$/i }));

        await screen.findByText('Slot saved!');
        const saveCall = fetchMock.mock.calls.find(
            ([url, init]) => url === '/api/autopush/slot' && init?.method === 'POST'
        );
        expect(saveCall[1].body.get('name')).toBe('COMMUTE é');
        unmount();
    });

    it('discards draft edits when Cancel is pressed', async () => {
        installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('Highway');
        await fireEvent.click(screen.getAllByRole('button', { name: /^edit$/i })[0]);
        const nameInput = screen.getByLabelText('Slot name');
        await fireEvent.input(nameInput, { target: { value: 'Changed Draft' } });
        await fireEvent.click(screen.getByRole('button', { name: /^cancel$/i }));

        expect(screen.getByRole('heading', { name: 'Default' })).toBeInTheDocument();
        expect(screen.queryByDisplayValue('Changed Draft')).not.toBeInTheDocument();

        unmount();
    });

    it('saves slot modifiers without editing the shared profile', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('Highway');
        await fireEvent.click(screen.getAllByRole('button', { name: /^edit$/i })[0]);
        expect(screen.getByLabelText('Profile')).toBeInTheDocument();
        expect(screen.getByText('Alert persistence (seconds)')).toBeInTheDocument();
        expect(screen.getByLabelText('Priority arrow only')).toBeInTheDocument();
        expect(screen.getByText('Override profile volume')).toBeInTheDocument();
        expect(screen.getByLabelText('V1 detector display')).toBeInTheDocument();
        expect(screen.queryByText('Logic Mode')).not.toBeInTheDocument();
        await fireEvent.click(screen.getByRole('button', { name: /^save slot$/i }));
        await screen.findByText('Slot saved!');

        const saveCall = fetchMock.mock.calls.find(
            ([url, init]) => url === '/api/autopush/slot' && init?.method === 'POST'
        );
        expect(saveCall).toBeTruthy();
        const body = saveCall[1].body;
        expect(body.getAll('slot')).toEqual(['0']);
        expect(body.get('profile')).toBe('Road Trip');
        expect(body.get('alertPersist')).toBe('1');
        expect(body.get('priorityArrowOnly')).toBe('false');
        expect(body.has('mode')).toBe(false);
        expect(body.get('volumeConfigured')).toBe('false');
        expect(body.has('volume')).toBe(false);
        expect(body.has('muteVol')).toBe(false);
        expect(body.get('darkModeConfigured')).toBe('false');
        expect(body.has('darkMode')).toBe(false);
        expect(body.has('muteToZero')).toBe(false);

        unmount();
    });

    it.each([
        ['Display off — Bluetooth light follows profile', 'on', 'true', 'true'],
        ['Display on', 'off', 'true', 'false'],
        ['Use profile', 'profile', 'false', null]
    ])('saves %s with the existing override payload', async (label, value, configured, darkMode) => {
        const fetchMock = installDefaultFetch([{
            method: 'GET', match: '/api/v1/profile?name=Road%20Trip',
            respond: jsonResponse({ detector: { volume: { policy: 'temporary' } } })
        }]);
        const { unmount } = render(Page);

        await screen.findByText('Highway');
        await fireEvent.click(screen.getAllByRole('button', { name: /^edit$/i })[0]);
        await fireEvent.click(screen.getByText('Override profile settings'));
        expect(screen.getByRole('option', { name: label })).toHaveValue(value);
        expect(screen.getByRole('link', { name: 'V1 Profiles' })).toHaveAttribute('href', '/profiles');
        await fireEvent.click(screen.getByLabelText('Override profile volume'));
        await fireEvent.input(screen.getByLabelText('Main volume (0–9)'), { target: { value: '0' } });
        await fireEvent.input(screen.getByLabelText('Muted volume (0–9)'), { target: { value: '2' } });
        // Begin with an override so Use profile also proves that clearing it omits darkMode.
        await fireEvent.change(screen.getByLabelText('V1 detector display'), { target: { value: 'on' } });
        await fireEvent.change(screen.getByLabelText('V1 detector display'), { target: { value } });
        await fireEvent.click(screen.getByLabelText('Priority arrow only'));
        await fireEvent.input(screen.getByLabelText('Alert persistence value (seconds)'), { target: { value: '4' } });
        await fireEvent.click(screen.getByRole('button', { name: /^save slot$/i }));

        await screen.findByText('Slot saved!');
        const body = fetchMock.mock.calls.find(
            ([url, init]) => url === '/api/autopush/slot' && init?.method === 'POST'
        )[1].body;
        expect(body.get('volumeConfigured')).toBe('true');
        expect(body.get('volume')).toBe('0');
        expect(body.get('muteVol')).toBe('2');
        expect(body.get('darkModeConfigured')).toBe(configured);
        expect(body.get('darkMode')).toBe(darkMode);
        expect(body.get('profile')).toBe('Road Trip');
        expect(body.get('clearProfile')).toBe('false');
        expect(body.get('priorityArrowOnly')).toBe('true');
        expect(body.get('alertPersist')).toBe('4');
        expect(fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST'))
            .toHaveLength(1);
        unmount();
    });

    it('shows saved overrides immediately and can return volume to the profile', async () => {
        const fetchMock = installDefaultFetch([{
            method: 'GET', match: '/api/autopush/slots',
            respond: jsonResponse({
                schemaVersion: 4, enabled: true, activeSlot: 0,
                slots: [{
                    name: 'Default', profile: 'Road Trip', color: 31,
                    volumeConfigured: true, volume: 6, muteVolume: 2,
                    darkModeConfigured: true, darkMode: false,
                    alertPersist: 5, priorityArrowOnly: true
                }]
            })
        }]);
        const { unmount } = render(Page);
        await screen.findByText('Road Trip');
        await fireEvent.click(screen.getByRole('button', { name: /^edit$/i }));

        expect(screen.getByText('Override profile settings').closest('details')).toHaveAttribute('open');
        expect(screen.getByLabelText('Main volume (0–9)')).toHaveValue(6);
        expect(screen.getByLabelText('Muted volume (0–9)')).toHaveValue(2);
        expect(screen.getByLabelText('V1 detector display')).toHaveValue('off');
        await fireEvent.click(screen.getByLabelText('Override profile volume'));
        await fireEvent.click(screen.getByRole('button', { name: /^save slot$/i }));
        await screen.findByText('Slot saved!');

        const body = fetchMock.mock.calls.find(
            ([url, init]) => url === '/api/autopush/slot' && init?.method === 'POST'
        )[1].body;
        expect(body.get('volumeConfigured')).toBe('false');
        expect(body.has('volume')).toBe(false);
        expect(body.has('muteVol')).toBe(false);
        expect(body.get('darkModeConfigured')).toBe('true');
        expect(body.get('darkMode')).toBe('false');
        expect(body.get('alertPersist')).toBe('5');
        expect(body.get('priorityArrowOnly')).toBe('true');
        unmount();
    });

    it('makes pending-migration slots read-only while keeping durable slot Apply available', async () => {
        const fetchMock = installDefaultFetch([
            maintenanceStatus,
            queuedOperation,
            {
                method: 'GET',
                match: '/api/autopush/slots',
                respond: jsonResponse({
                    schemaVersion: 1,
                    detectorConfigurationOwner: 'legacy-slot',
                    enabled: true,
                    activeSlot: 1,
                    slots: [
                        {
                            name: 'Default',
                            profile: 'Road Trip',
                            mode: 2,
                            color: 1,
                            volumeConfigured: false,
                            volume: 255,
                            muteVolume: 255,
                            darkMode: false,
                            muteToZero: false,
                            alertPersist: 2,
                            priorityArrowOnly: true
                        }
                    ]
                })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText(/profile settings migration is still pending/i);
        const edit = screen.getByRole('button', { name: /^edit$/i });
        expect(edit).toBeDisabled();
        for (const activate of screen.getAllByRole('button', { name: /^set as default$/i })) {
            expect(activate).toBeDisabled();
        }
        const push = screen.getByRole('button', { name: /apply to v1/i });
        await waitFor(() => expect(push).toBeEnabled());
        await fireEvent.click(push);
        await screen.findByText(/Slot 1 queued as operation 42/);
        expect(fetchMock.mock.calls.some(
            ([url, init]) => url === '/api/autopush/push' && init?.method === 'POST'
        )).toBe(true);

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
        await fireEvent.click(screen.getAllByRole('button', { name: /^set as default$/i })[0]);

        await screen.findByText('Failed to set default slot');
        expect(screen.getByText('Highway')).toBeInTheDocument();
        expect(screen.getAllByText('Global default')).toHaveLength(1);
        expect(screen.getAllByRole('button', { name: /^set as default$/i })).toHaveLength(2);

        unmount();
    });

    it('sets the default with the 1-based slot number without applying the slot', async () => {
        const fetchMock = installDefaultFetch([
            {
                method: 'POST',
                match: '/api/autopush/activate',
                respond: jsonResponse({ success: true })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Highway');
        await fireEvent.click(screen.getAllByRole('button', { name: /^set as default$/i })[0]);

        // User-facing slot numbers are one-based.
        await screen.findByText('Slot 1 set as default');
        expect(screen.getAllByText('Global default')).toHaveLength(1);
        const posts = fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST');
        expect(posts).toHaveLength(1);
        expect(posts[0][0]).toBe('/api/autopush/activate');
        expect(posts[0][1].body.get('slot')).toBe('0');
        expect(posts[0][1].body.get('enable')).toBe('true');

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

    it('labels a stale profile reference missing and prevents saving it unchanged', async () => {
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/v1/profiles',
                respond: jsonResponse({ profiles: [] })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Road Trip (missing)');
        await fireEvent.click(screen.getAllByRole('button', { name: /^edit$/i })[0]);
        expect(await screen.findByRole('option', { name: 'Road Trip (missing)' })).toBeInTheDocument();
        expect(screen.getByRole('button', { name: /^save slot$/i })).toBeDisabled();

        unmount();
    });

    it('labels the active slot as the global default and explains device overrides', async () => {
        installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('Global default');
        expect(
            screen.getByText(
                /Each slot uses a saved V1 profile.*unless that detector has its own slot selected in/s
            )
        ).toBeInTheDocument();

        unmount();
    });

    it('enables target-bound durable Apply and keeps slot configuration available in maintenance', async () => {
        const fetchMock = installDefaultFetch([
            maintenanceStatus,
            queuedOperation
        ]);
        const { unmount } = render(Page);

        await screen.findByText(
            'Apply to V1 uses the captured detector. V1Simple restarts to apply and check its settings, then returns here with the result.'
        );
        expect(await screen.findAllByRole('button', { name: /apply to v1/i })).toHaveLength(3);
        expect(screen.getAllByRole('button', { name: /apply to v1/i })[0]).toBeEnabled();
        expect(screen.getAllByRole('button', { name: /apply to v1/i })[1]).toBeEnabled();
        expect(screen.getAllByRole('button', { name: /apply to v1/i })[2]).toBeDisabled();
        for (const button of screen.getAllByRole('button', { name: /^set as default$/i })) {
            expect(button).toBeEnabled();
        }

        await fireEvent.click(screen.getAllByRole('button', { name: /^set as default$/i })[0]);
        await screen.findByText('Slot 1 set as default');
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === '/api/autopush/activate' && init?.method === 'POST'
            )
        ).toBe(true);

        await fireEvent.click(screen.getAllByRole('button', { name: /^edit$/i })[0]);
        expect(screen.getByRole('button', { name: /^save slot$/i })).toBeEnabled();
        await fireEvent.click(screen.getByRole('button', { name: /^save slot$/i }));
        await screen.findByText('Slot saved!');
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === '/api/autopush/slot' && init?.method === 'POST'
            )
        ).toBe(true);

        unmount();
    });

    it('defensively refuses slot Apply outside maintenance without posting', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText(
            'Apply to V1 is available in maintenance mode. Auto-Push still applies the selected slot when your V1 connects during normal use.'
        );
        const pushButton = screen.getAllByRole('button', { name: /apply to v1/i })[0];
        expect(pushButton).toBeDisabled();

        // Exercise the handler guard independently from the disabled UI state.
        pushButton.disabled = false;
        await fireEvent.click(pushButton);
        await screen.findByText(
            'Apply to V1 is available in maintenance mode, where V1Simple can restart and return with the result.'
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
            'Apply to V1 is unavailable because device runtime mode could not be verified.'
        );
        await screen.findByText('Highway');
        const pushButtons = screen.getAllByRole('button', { name: /apply to v1/i });
        for (const button of pushButtons) {
            expect(button).toBeDisabled();
        }

        // Exercise the handler guard independently from the disabled UI state.
        pushButtons[0].disabled = false;
        await fireEvent.click(pushButtons[0]);
        await screen.findByText(
            'Apply to V1 is unavailable until device runtime mode can be verified.'
        );
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === '/api/autopush/push' && init?.method === 'POST'
            )
        ).toBe(false);

        unmount();
    });

    it('prefers a backend human message when durable Apply admission fails', async () => {
        installDefaultFetch([
            maintenanceStatus,
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
        const pushButton = screen.getAllByRole('button', { name: /apply to v1/i })[0];
        await waitFor(() => expect(pushButton).toBeEnabled());
        await fireEvent.click(pushButton);

        await screen.findByText('Connect the V1 before pushing settings.');
        expect(screen.queryByText('v1_not_connected')).not.toBeInTheDocument();

        unmount();
    });

    it('starts target-bound Apply with an exact durable identity rather than claiming success', async () => {
        const fetchMock = installDefaultFetch([maintenanceStatus, queuedOperation]);
        const { unmount } = render(Page);

        await screen.findByText('Highway');
        const pushButton = screen.getAllByRole('button', { name: /apply to v1/i })[0];
        await waitFor(() => expect(pushButton).toBeEnabled());
        await fireEvent.click(pushButton);

        await screen.findByText(/Slot 1 queued as operation 42 for AA:BB:CC:DD:EE:FF/);
        const call = fetchMock.mock.calls.find(
            ([url, init]) => url === '/api/autopush/push' && init?.method === 'POST'
        );
        expect(call[1].headers['Content-Type']).toContain('application/x-www-form-urlencoded');
        expect(call[1].body.get('slot')).toBe('0');
        expect(call[1].body.get('address')).toBe('AA:BB:CC:DD:EE:FF');
        expect(window.localStorage.getItem('v1simple.detectorSettingsOperationId')).toBe('42');
        expect(screen.queryByText(/Settings pushed/i)).not.toBeInTheDocument();

        unmount();
    });

    it('resumes and renders only the exact durable slot operation terminal', async () => {
        window.localStorage.setItem('v1simple.detectorSettingsOperationId', '42');
        installDefaultFetch([
            maintenanceStatus,
            {
                method: 'GET', match: '/api/autopush/status?operationId=42',
                respond: jsonResponse({
                    operationId: 42, kind: 'apply_slot', source: 'maintenance_ui', slot: 0,
                    targetAddress: 'AA:BB:CC:DD:EE:FF', returnToMaintenance: false,
                    state: 'succeeded', reason: 'none', terminal: true, result: 'succeeded',
                    components: emptyOperationComponents({ display: {
                        requested: true, sent: false, verified: true,
                        outcome: 'unchanged', reason: 'none'
                    } })
                })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Detector operation 42');
        expect(screen.getByText('apply_slot · target AA:BB:CC:DD:EE:FF · source maintenance_ui'))
            .toBeInTheDocument();
        expect(screen.getByText('unchanged · none')).toBeInTheDocument();
        expect(window.localStorage.getItem('v1simple.detectorSettingsOperationId')).toBeNull();
        unmount();
    });

    it('times out a stalled operation-start body and permits a safe retry', async () => {
        let attempts = 0;
        installDefaultFetch([
            maintenanceStatus,
            {
                method: 'POST', match: '/api/autopush/push',
                respond: ({ init }) => {
                    attempts += 1;
                    return attempts === 1 ? pendingJson(init.signal) : jsonResponse({
                        success: true, queued: true, operationId: 42,
                        state: 'pending_normal_boot', rebooting: true, target: 'normal'
                    }, 202);
                }
            }
        ]);
        const { unmount } = render(Page);
        const push = (await screen.findAllByRole('button', { name: /apply to v1/i }))[0];
        await waitFor(() => expect(push).toBeEnabled());
        vi.useFakeTimers();
        void fireEvent.click(push);
        await vi.advanceTimersByTimeAsync(5000);
        expect(screen.getByText('Connection error')).toBeInTheDocument();

        await fireEvent.click(push);
        await vi.advanceTimersByTimeAsync(0);
        expect(screen.getByText(/Slot 1 queued as operation 42/)).toBeInTheDocument();
        expect(attempts).toBe(2);
        unmount();
    });

    it('times out a stalled status body and resumes the same exact identity', async () => {
        vi.useFakeTimers();
        window.localStorage.setItem('v1simple.detectorSettingsOperationId', '42');
        let attempts = 0;
        installDefaultFetch([
            maintenanceStatus,
            {
                method: 'GET', match: '/api/autopush/status?operationId=42',
                respond: ({ init }) => {
                    attempts += 1;
                    if (attempts === 1) return pendingJson(init.signal);
                    return jsonResponse({
                        operationId: 42, kind: 'apply_slot', source: 'maintenance_ui', slot: 0,
                        targetAddress: 'AA:BB:CC:DD:EE:FF', returnToMaintenance: false,
                        state: 'succeeded', reason: 'none', terminal: true, result: 'succeeded',
                        components: emptyOperationComponents()
                    });
                }
            }
        ]);
        const { unmount } = render(Page);
        await vi.advanceTimersByTimeAsync(5000);
        await vi.advanceTimersByTimeAsync(1500);
        expect(screen.getByText('Detector operation 42')).toBeInTheDocument();
        expect(attempts).toBe(2);
        expect(window.localStorage.getItem('v1simple.detectorSettingsOperationId')).toBeNull();
        unmount();
    });

    it('discards an older poll response after a new operation identity is accepted', async () => {
        window.localStorage.setItem('v1simple.detectorSettingsOperationId', '41');
        let resolveOldPoll;
        installDefaultFetch([
            maintenanceStatus,
            queuedOperation,
            {
                method: 'GET', match: '/api/autopush/status?operationId=41',
                respond: () => new Promise((resolve) => { resolveOldPoll = resolve; })
            }
        ]);
        const { unmount } = render(Page);
        const push = (await screen.findAllByRole('button', { name: /apply to v1/i }))[0];
        await waitFor(() => expect(push).toBeEnabled());
        await fireEvent.click(push);
        await screen.findByText(/Slot 1 queued as operation 42/);

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
});
