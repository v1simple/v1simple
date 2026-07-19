import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

import * as settingsLazyComponents from '$lib/features/settings/settingsLazyComponents.js';
import { apiFixtureMatchers, installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
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
    {
        index: 2,
        ssid: '',
        label: '',
        priority: 2,
        hasPassword: false,
        lastConnectedAtSec: 0,
        configured: false
    },
    {
        index: 3,
        ssid: '',
        label: '',
        priority: 3,
        hasPassword: false,
        lastConnectedAtSec: 0,
        configured: false
    }
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

function rejectUnexpectedRequest({ method, url }) {
    const path = new URL(String(url), 'http://v1simple.test').pathname;
    throw new Error(`Unexpected settings fixture request: ${method} ${path}`);
}

function installDefaultFetch(
    overrides = [],
    { slots = defaultSlots, wifiStatus = undefined } = {}
) {
    const wifiStatusMatchers =
        wifiStatus === undefined
            ? apiFixtureMatchers('wifi_status_default')
            : [
                  {
                      method: 'GET',
                      match: '/api/wifi/status',
                      respond: jsonResponse(wifiStatus)
                  }
              ];
    return installFetchMock(
        [
            ...overrides,
            ...wifiStatusMatchers,
            {
                method: 'GET',
                match: '/api/device/settings',
                respond: jsonResponse({ ap_ssid: 'V1' })
            },
            { method: 'GET', match: '/api/wifi/networks', respond: jsonResponse({ slots }) },
            {
                method: 'GET',
                match: '/api/wifi/scan',
                respond: jsonResponse({ scanning: false, networks: [] })
            },
            {
                method: 'GET',
                match: '/api/status',
                respond: jsonResponse({ time: { valid: false } })
            },
            {
                method: 'POST',
                match: '/api/device/settings',
                respond: jsonResponse({ success: true })
            },
            { method: 'POST', match: '/api/wifi/enable', respond: jsonResponse({ success: true }) },
            {
                method: 'POST',
                match: '/api/wifi/disconnect',
                respond: jsonResponse({ success: true })
            },
            {
                method: 'POST',
                match: '/api/wifi/scan',
                respond: jsonResponse({ scanning: false, networks: [] })
            },
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
        rejectUnexpectedRequest
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
        expect(screen.getByText('Disable AP Inactivity Timeout')).toBeInTheDocument();
        expect(
            screen.getByText(/separate maintenance-session.*shown in the banner/i)
        ).toBeInTheDocument();

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
                {
                    method: 'GET',
                    match: '/api/wifi/status',
                    respond: jsonResponse({ enabled: false, state: 'disabled' })
                },
                {
                    method: 'GET',
                    match: '/api/wifi/networks',
                    respond: jsonResponse({ slots: [] })
                },
                {
                    method: 'GET',
                    match: '/api/status',
                    respond: jsonResponse({ time: { valid: false } })
                }
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
                {
                    method: 'GET',
                    match: '/api/device/settings',
                    respond: jsonResponse({ ap_ssid: 'V1' })
                },
                {
                    method: 'GET',
                    match: '/api/wifi/status',
                    respond: jsonResponse({ error: 'bad wifi' }, 500)
                },
                {
                    method: 'GET',
                    match: '/api/wifi/networks',
                    respond: jsonResponse({ slots: defaultSlots })
                },
                {
                    method: 'GET',
                    match: '/api/status',
                    respond: jsonResponse({ time: { valid: false } })
                }
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
        await fireEvent.input(screen.getByLabelText('Password'), {
            target: { value: 'secret123' }
        });
        await fireEvent.input(screen.getByLabelText('Priority'), { target: { value: '0' } });
        await fireEvent.click(screen.getByRole('button', { name: /^Save$/i }));

        await screen.findByText('Saved CarAP');
        const bodies = jsonBodies(fetchMock, '/api/wifi/networks');
        expect(bodies.at(-1)).toMatchObject({
            ssid: 'CarAP',
            label: 'Car hotspot',
            password: 'secret123',
            priority: 0
        });

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
        await fireEvent.input(screen.getByLabelText('Label'), {
            target: { value: 'Garage Updated' }
        });
        await fireEvent.click(screen.getByRole('button', { name: /^Save$/i }));

        await screen.findByText('Saved HomeWifi');
        const body = jsonBodies(fetchMock, '/api/wifi/networks').at(-1);
        expect(body).toMatchObject({
            index: 0,
            ssid: 'HomeWifi',
            label: 'Garage Updated',
            priority: 0
        });
        expect(body).not.toHaveProperty('password');

        unmount();
    });

    it('deletes a saved network slot', async () => {
        const fetchMock = installDefaultFetch();
        vi.stubGlobal(
            'confirm',
            vi.fn(() => true)
        );
        const { unmount } = render(Page);

        await screen.findByText('Garage');
        await fireEvent.click(screen.getAllByRole('button', { name: /^Delete$/i })[0]);

        await screen.findByText('Deleted HomeWifi');
        expect(jsonBodies(fetchMock, '/api/wifi/networks/delete').at(-1)).toEqual({ index: 0 });

        unmount();
    });

    it('polls a saved-network test through terminal connection success', async () => {
        vi.useFakeTimers();
        let statusCalls = 0;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    if (statusCalls === 1) {
                        return jsonResponse({ enabled: true, state: 'disconnected' });
                    }
                    if (statusCalls === 2) {
                        return jsonResponse({
                            enabled: true,
                            state: 'connecting',
                            savedSSID: 'HomeWifi'
                        });
                    }
                    return jsonResponse({
                        enabled: true,
                        state: 'connected',
                        connectedSSID: 'HomeWifi',
                        connectedSlotIndex: 0,
                        rssi: -48
                    });
                }
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Garage');
        await fireEvent.click(screen.getAllByRole('button', { name: /^Test$/i })[0]);

        await screen.findByText('Testing connection to HomeWifi...');
        expect(screen.getAllByRole('button', { name: /^Test$/i })[1]).toBeDisabled();
        await vi.advanceTimersByTimeAsync(1000);
        await screen.findByText('Connected to HomeWifi');
        expect(jsonBodies(fetchMock, '/api/wifi/networks/test').at(-1)).toEqual({ index: 0 });

        unmount();
    });

    it('reports a saved-network test terminal failure', async () => {
        let statusCalls = 0;
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    return jsonResponse({
                        enabled: true,
                        state: statusCalls === 1 ? 'disconnected' : 'failed'
                    });
                }
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Garage');
        await fireEvent.click(screen.getAllByRole('button', { name: /^Test$/i })[0]);

        await screen.findByText('Could not connect to HomeWifi');
        expect(screen.getAllByRole('button', { name: /^Test$/i })[0]).toBeEnabled();

        unmount();
    });

    it('caps a stalled saved-network test at a 20-second wall-clock deadline', async () => {
        vi.useFakeTimers();
        let statusCalls = 0;
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: ({ init }) => {
                    statusCalls += 1;
                    if (statusCalls === 1) {
                        return jsonResponse({ enabled: true, state: 'disconnected' });
                    }

                    return new Promise((resolve, reject) => {
                        const rejectOnAbort = () => reject(new Error('status request aborted'));
                        if (init.signal?.aborted) {
                            rejectOnAbort();
                            return;
                        }
                        init.signal?.addEventListener('abort', rejectOnAbort, { once: true });
                    });
                }
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Garage');
        await fireEvent.click(screen.getAllByRole('button', { name: /^Test$/i })[0]);
        await screen.findByText('Testing connection to HomeWifi...');

        await vi.advanceTimersByTimeAsync(19_999);
        expect(
            screen.queryByText('Connection test timed out for HomeWifi')
        ).not.toBeInTheDocument();

        await vi.advanceTimersByTimeAsync(1);
        expect(screen.getByText('Connection test timed out for HomeWifi')).toBeInTheDocument();
        expect(screen.getAllByRole('button', { name: /^Test$/i })[0]).toBeEnabled();
        expect(statusCalls).toBeLessThanOrEqual(9);

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
        expect(
            phone.compareDocumentPosition(garage) & Node.DOCUMENT_POSITION_FOLLOWING
        ).toBeTruthy();
        expect(screen.getByText('Connected')).toBeInTheDocument();
        expect(screen.getByText(/Slot 2 \(Phone\)/)).toBeInTheDocument();
        expect(screen.getAllByText('-55 dBm').length).toBeGreaterThan(0);

        unmount();
    });

    it('saves a network picked from scan results', async () => {
        vi.useFakeTimers();
        const fetchMock = installDefaultFetch(apiFixtureMatchers('wifi_scan_success'));
        const { unmount } = render(Page);

        await openScanModal();
        await vi.advanceTimersByTimeAsync(1000);
        await vi.advanceTimersByTimeAsync(1000);
        await fireEvent.click(await screen.findByRole('button', { name: /BenchAP/i }));
        await fireEvent.input(await screen.findByLabelText('Password'), {
            target: { value: 'bench-secret' }
        });
        await fireEvent.click(screen.getByRole('button', { name: /^Save Network$/i }));

        await screen.findByText('Saved BenchAP');
        expect(jsonBodies(fetchMock, '/api/wifi/networks').at(-1)).toMatchObject({
            ssid: 'BenchAP',
            password: 'bench-secret',
            priority: 2
        });

        unmount();
    });

    it('uses captured enable success and status-refetch responses', async () => {
        const fetchMock = installDefaultFetch(apiFixtureMatchers('wifi_enable_success'));
        const { unmount } = render(Page);

        await screen.findByText(/WiFi client is disabled/i);
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');
        expect(toggle).not.toBeChecked();

        await fireEvent.click(toggle);

        await screen.findByText('WiFi client enabled');
        expect(toggle).toBeChecked();
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === '/api/wifi/enable' && init?.method === 'POST'
            )
        ).toBe(true);

        unmount();
    });

    it('keeps the captured enable state disabled after a failed connection attempt', async () => {
        const fetchMock = installDefaultFetch(apiFixtureMatchers('wifi_enable_failure'));
        const first = render(Page);

        await screen.findByText(/WiFi client is disabled/i);
        const firstCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const firstToggle = firstCard.querySelector('input[type="checkbox"]');
        expect(firstToggle).not.toBeChecked();

        await fireEvent.click(firstToggle);

        await screen.findByText('Failed to change WiFi setting');
        expect(firstToggle).not.toBeChecked();
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/networks')).toHaveLength(
            2
        );

        first.unmount();
    });

    it('reconciles WiFi status after the enable request rejects', async () => {
        let statusCalls = 0;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    return statusCalls === 1
                        ? jsonResponse({ enabled: false, state: 'disabled' })
                        : jsonResponse({ enabled: true, state: 'connecting' });
                }
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: () => Promise.reject(new Error('request lost'))
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText(/WiFi client is disabled/i);
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');

        await fireEvent.click(toggle);

        await screen.findByText('Connection error');
        expect(statusCalls).toBe(2);
        expect(toggle).toBeChecked();
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);

        unmount();
    });

    it('keeps a newer in-flight status when the forced reconciliation fails', async () => {
        const activeStatus = createDeferred();
        let statusCalls = 0;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    return statusCalls === 1
                        ? activeStatus.promise
                        : Promise.reject(new Error('forced status unavailable'));
                }
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: jsonResponse({ success: false }, 500)
            }
        ]);
        const { unmount } = render(Page);

        await waitFor(() => expect(statusCalls).toBe(1));
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');
        await fireEvent.click(toggle);

        activeStatus.resolve(jsonResponse({ enabled: true, state: 'connecting' }));

        await waitFor(() => expect(statusCalls).toBe(2));
        await screen.findByText('Failed to change WiFi setting');
        expect(toggle).toBeChecked();
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);

        unmount();
    });

    it('queues a fresh status read behind an older in-flight request', async () => {
        const staleStatus = createDeferred();
        let statusCalls = 0;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    return statusCalls === 1
                        ? staleStatus.promise
                        : jsonResponse({ enabled: false, state: 'disabled' });
                }
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: jsonResponse({ success: false }, 500)
            }
        ]);
        const { unmount } = render(Page);

        await waitFor(() => expect(statusCalls).toBe(1));
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');
        await fireEvent.click(toggle);

        staleStatus.resolve(jsonResponse({ enabled: true, state: 'connecting' }));

        await waitFor(() => expect(statusCalls).toBe(2));
        await screen.findByText('Failed to change WiFi setting');
        expect(toggle).not.toBeChecked();
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);

        unmount();
    });

    it('does not report enable success when status confirmation fails', async () => {
        let statusCalls = 0;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    return statusCalls === 1
                        ? jsonResponse({ enabled: false, state: 'disabled' })
                        : Promise.reject(new Error('status unavailable'));
                }
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: jsonResponse({ success: true })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText(/WiFi client is disabled/i);
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');

        await fireEvent.click(toggle);

        await screen.findByText('Failed to confirm WiFi setting');
        expect(statusCalls).toBe(2);
        expect(toggle).not.toBeChecked();
        expect(screen.queryByText('WiFi client enabled')).not.toBeInTheDocument();
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);

        unmount();
    });

    it('does not confirm disable or poison state from a non-boolean enabled field', async () => {
        let statusCalls = 0;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    return statusCalls === 1
                        ? jsonResponse({ enabled: true, state: 'connected' })
                        : jsonResponse({ enabled: null, state: 'disabled' });
                }
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: jsonResponse({ success: true })
            }
        ]);
        const { unmount } = render(Page);

        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');
        await waitFor(() => expect(toggle).toBeChecked());

        await fireEvent.click(toggle);

        await screen.findByText('Failed to confirm WiFi setting');
        expect(statusCalls).toBe(2);
        expect(toggle).toBeChecked();
        expect(screen.queryByText('WiFi client disabled')).not.toBeInTheDocument();
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);

        unmount();
    });

    it('restores the last authoritative toggle state when reconciliation fails', async () => {
        let statusCalls = 0;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    return statusCalls === 1
                        ? jsonResponse({ enabled: false, state: 'disabled' })
                        : Promise.reject(new Error('status unavailable'));
                }
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: jsonResponse({ success: false }, 500)
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText(/WiFi client is disabled/i);
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');

        await fireEvent.click(toggle);

        await screen.findByText('Failed to change WiFi setting');
        expect(statusCalls).toBe(2);
        expect(toggle).not.toBeChecked();
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);

        unmount();
    });

    it('disables the WiFi toggle while one mutation is pending', async () => {
        const enableResponse = createDeferred();
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: jsonResponse({ enabled: false, state: 'disabled' })
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: () => enableResponse.promise
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText(/WiFi client is disabled/i);
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');

        await fireEvent.click(toggle);
        const disabledWhilePending = toggle.disabled;
        const checkedWhilePending = toggle.checked;
        await fireEvent.click(toggle);
        const postCallsWhilePending = fetchMock.mock.calls.filter(
            ([url, init]) => url === '/api/wifi/enable' && init?.method === 'POST'
        ).length;

        enableResponse.resolve(jsonResponse({ success: false }, 500));
        await screen.findByText('Failed to change WiFi setting');

        expect(disabledWhilePending).toBe(true);
        expect(checkedWhilePending).toBe(false);
        expect(postCallsWhilePending).toBe(1);
        expect(toggle).not.toBeDisabled();

        unmount();
    });

    it('does not reconcile a deferred toggle after unmount', async () => {
        const enableResponse = createDeferred();
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: jsonResponse({ enabled: false, state: 'disabled' })
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: () => enableResponse.promise
            }
        ]);
        const view = render(Page);

        await screen.findByText(/WiFi client is disabled/i);
        await waitFor(() => {
            expect(
                fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/networks')
            ).toHaveLength(1);
        });
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');
        await fireEvent.click(toggle);

        const statusCallsBeforeUnmount = fetchMock.mock.calls.filter(
            ([url]) => url === '/api/wifi/status'
        ).length;
        const networkCallsBeforeUnmount = fetchMock.mock.calls.filter(
            ([url]) => url === '/api/wifi/networks'
        ).length;
        view.unmount();

        enableResponse.resolve(jsonResponse({ success: true }));
        await new Promise((resolve) => setTimeout(resolve, 0));

        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(
            statusCallsBeforeUnmount
        );
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/networks')).toHaveLength(
            networkCallsBeforeUnmount
        );
    });

    it('does not launch a queued status reconciliation after unmount', async () => {
        const activeStatus = createDeferred();
        let statusCalls = 0;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    return activeStatus.promise;
                }
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: jsonResponse({ success: true })
            }
        ]);
        const view = render(Page);

        await waitFor(() => expect(statusCalls).toBe(1));
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');
        await fireEvent.click(toggle);
        await waitFor(() => {
            expect(
                fetchMock.mock.calls.filter(
                    ([url, init]) => url === '/api/wifi/enable' && init?.method === 'POST'
                )
            ).toHaveLength(1);
        });

        view.unmount();
        activeStatus.resolve(jsonResponse({ enabled: false, state: 'disabled' }));
        await new Promise((resolve) => setTimeout(resolve, 0));

        expect(statusCalls).toBe(1);
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(1);
    });

    it('lazy-loads the WiFi scan modal on first open and reuses it after closing', async () => {
        const deferred = createDeferred();
        const wifiModalLoader = vi
            .spyOn(settingsLazyComponents, 'loadSettingsWifiModal')
            .mockReturnValue(deferred.promise);
        installDefaultFetch();
        const { unmount } = render(Page);

        expect(wifiModalLoader).not.toHaveBeenCalled();
        await fireEvent.click(
            (await screen.findAllByRole('button', { name: /pick from scan/i }))[0]
        );

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

    it('does not poll before the scan POST settles', async () => {
        vi.useFakeTimers();
        const scanPost = createDeferred();
        let getCalls = 0;
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/wifi/scan',
                respond: () => scanPost.promise
            },
            {
                method: 'GET',
                match: '/api/wifi/scan',
                respond: () => {
                    getCalls += 1;
                    return jsonResponse({
                        scanning: false,
                        networks: [{ ssid: 'DeferredAP', secure: true, rssi: -44 }]
                    });
                }
            }
        ]);
        const { unmount } = render(Page);

        await openScanModal();
        await vi.advanceTimersByTimeAsync(3000);
        expect(getCalls).toBe(0);

        scanPost.resolve(jsonResponse({ scanning: true, networks: [] }));
        await Promise.resolve();
        await vi.advanceTimersByTimeAsync(1000);

        expect(getCalls).toBe(1);
        await screen.findByRole('button', { name: /DeferredAP/i });

        await vi.advanceTimersByTimeAsync(2000);
        expect(getCalls).toBe(1);

        unmount();
    });

    it('ignores a stale scan response and polls the reopened scan', async () => {
        vi.useFakeTimers();
        let postCalls = 0;
        const reopenedPost = createDeferred();
        const staleGet = createDeferred();
        let getCalls = 0;
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/wifi/scan',
                respond: () => {
                    postCalls += 1;
                    return postCalls === 1
                        ? jsonResponse({ scanning: true, networks: [] })
                        : reopenedPost.promise;
                }
            },
            {
                method: 'GET',
                match: '/api/wifi/scan',
                respond: () => {
                    getCalls += 1;
                    if (getCalls === 1) return staleGet.promise;
                    return jsonResponse({
                        scanning: false,
                        networks: [{ ssid: 'ReopenedAP', secure: true, rssi: -42 }]
                    });
                }
            }
        ]);
        const { unmount } = render(Page);

        await openScanModal();
        await vi.advanceTimersByTimeAsync(1000);
        expect(getCalls).toBe(1);

        await fireEvent.click(screen.getByRole('button', { name: /^Close$/i }));
        await waitFor(() => {
            expect(screen.queryByText('Pick WiFi Network to Save')).not.toBeInTheDocument();
        });

        await openScanModal();
        staleGet.resolve(
            jsonResponse({
                scanning: false,
                networks: [{ ssid: 'OldAP', secure: true, rssi: -80 }]
            })
        );
        await Promise.resolve();
        await Promise.resolve();

        reopenedPost.resolve(jsonResponse({ scanning: true, networks: [] }));
        await Promise.resolve();
        await vi.advanceTimersByTimeAsync(1000);

        expect(getCalls).toBe(2);
        await screen.findByRole('button', { name: /ReopenedAP/i });
        expect(screen.queryByRole('button', { name: /OldAP/i })).not.toBeInTheDocument();

        unmount();
    });

    it('reports a non-success response when starting a WiFi scan', async () => {
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/wifi/scan',
                respond: jsonResponse({ success: false, message: 'scan unavailable' }, 503)
            }
        ]);
        const { unmount } = render(Page);

        const buttons = await screen.findAllByRole('button', { name: /pick from scan/i });
        await fireEvent.click(buttons[0]);
        await screen.findByText('Failed to start WiFi scan');
        expect(screen.queryByText('Scanning for networks...')).not.toBeInTheDocument();
        expect(screen.queryByText('Pick WiFi Network to Save')).not.toBeInTheDocument();

        unmount();
    });

    it('shows WiFi scan polling errors and clears them after a successful retry', async () => {
        vi.useFakeTimers();
        let postCalls = 0;
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/wifi/scan',
                respond: () => {
                    postCalls += 1;
                    return postCalls === 1
                        ? jsonResponse({ scanning: true, networks: [] })
                        : jsonResponse({
                              scanning: false,
                              networks: [{ ssid: 'RecoveredAP', secure: true, rssi: -38 }]
                          });
                }
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
        expect(screen.queryByText('Pick WiFi Network to Save')).not.toBeInTheDocument();

        const retryButtons = await screen.findAllByRole('button', { name: /pick from scan/i });
        await fireEvent.click(retryButtons[0]);

        await screen.findByRole('button', { name: /RecoveredAP/i });
        expect(screen.queryByText('Failed to update WiFi scan')).not.toBeInTheDocument();

        unmount();
    });

    it('does not claim a disconnect succeeded when the API rejects it', async () => {
        installDefaultFetch(
            [
                {
                    method: 'POST',
                    match: '/api/wifi/disconnect',
                    respond: jsonResponse({ success: false, message: 'disconnect failed' }, 500)
                }
            ],
            {
                wifiStatus: {
                    enabled: true,
                    state: 'connected',
                    connectedSSID: 'HomeWifi',
                    connectedSlotIndex: 0,
                    ip: '192.168.1.50',
                    rssi: -50
                }
            }
        );
        const { unmount } = render(Page);

        await fireEvent.click(await screen.findByRole('button', { name: /^Disconnect$/i }));

        await screen.findByText('Failed to disconnect');
        expect(screen.queryByText('Disconnected from WiFi')).not.toBeInTheDocument();

        unmount();
    });

    it('shows success message on save success', async () => {
        const fetchMock = installDefaultFetch([
            {
                method: 'POST',
                match: '/api/device/settings',
                respond: jsonResponse({ success: true })
            }
        ]);
        const { unmount } = render(Page);

        const saveButton = await screen.findByRole('button', { name: /save settings/i });
        await fireEvent.click(saveButton);
        await screen.findByText(
            'Settings saved! New AP credentials apply the next time the AP starts.'
        );
        const postCall = fetchMock.mock.calls.find(
            ([url, init]) => url === '/api/device/settings' && init?.method === 'POST'
        );
        expect(postCall).toBeTruthy();
        expect(postCall[1].body.has('proxy_ble')).toBe(false);
        expect(postCall[1].body.has('proxy_name')).toBe(false);
        expect(postCall[1].body.get('powerOffSdLog')).toBe('false');
        unmount();
    });

    it('exposes and saves shutdown evidence logging in the released settings page', async () => {
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/device/settings',
                respond: jsonResponse({ ap_ssid: 'V1', powerOffSdLog: false })
            }
        ]);
        const { unmount } = render(Page);

        const toggle = await screen.findByRole('checkbox', { name: /record shutdown evidence/i });
        expect(toggle).not.toBeChecked();
        expect(screen.getByRole('link', { name: 'Logs page' })).toHaveAttribute('href', '/logs');

        await fireEvent.click(toggle);
        await fireEvent.click(screen.getByRole('button', { name: /save settings/i }));
        await screen.findByText(
            'Settings saved! New AP credentials apply the next time the AP starts.'
        );

        const postCall = fetchMock.mock.calls.find(
            ([url, init]) => url === '/api/device/settings' && init?.method === 'POST'
        );
        expect(postCall[1].body.get('powerOffSdLog')).toBe('true');
        unmount();
    });

    it('shows API error message on save failure', async () => {
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/device/settings',
                respond: jsonResponse({ success: false }, 500)
            }
        ]);
        const { unmount } = render(Page);

        const saveButton = await screen.findByRole('button', { name: /save settings/i });
        await fireEvent.click(saveButton);

        await screen.findByText('Failed to save settings');
        unmount();
    });

    it('rejects invalid backup JSON before posting restore', async () => {
        const fetchMock = installDefaultFetch();
        vi.stubGlobal(
            'confirm',
            vi.fn(() => true)
        );
        const { unmount } = render(Page);

        await startRestore('{not-json');

        await screen.findByText('Selected file is not valid JSON.');
        expect(fetchMock.mock.calls.some(([url]) => url === '/api/settings/restore')).toBe(false);

        unmount();
    });

    it('rejects unrecognized backup types before posting restore', async () => {
        const fetchMock = installDefaultFetch();
        vi.stubGlobal(
            'confirm',
            vi.fn(() => true)
        );
        const { unmount } = render(Page);

        await startRestore(JSON.stringify({ _type: 'wrong_backup_type' }));

        await screen.findByText('Selected file is not a V1 Simple settings backup.');
        expect(fetchMock.mock.calls.some(([url]) => url === '/api/settings/restore')).toBe(false);

        unmount();
    });

    it('consumes captured restore success and refetches settings plus WiFi state', async () => {
        const fetchMock = installDefaultFetch(apiFixtureMatchers('settings_restore_success'));
        vi.stubGlobal(
            'confirm',
            vi.fn(() => true)
        );
        const { unmount } = render(Page);
        const payload = JSON.stringify({ _type: 'v1simple_backup', profiles: [] });

        await startRestore(payload);

        await screen.findByText('Settings restored and reloaded.');
        await waitFor(() => expect(screen.getByLabelText('AP Name')).toHaveValue('RestoredAP'));
        await screen.findByText(/SSID: RestoredWifi/);
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === '/api/settings/restore' && init?.body === payload
            )
        ).toBe(true);
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/device/settings')).toHaveLength(
            2
        );
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/networks')).toHaveLength(
            2
        );

        unmount();
    });
});
