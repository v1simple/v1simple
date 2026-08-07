import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../test/fetch-mock.js';
import Page from './+page.svelte';

function installDefaultFetch(overrides = []) {
    return installFetchMock(
        [
            ...overrides,
            {
                method: 'GET',
                match: '/api/status',
                respond: jsonResponse({
                    wifi: {
                        sta_connected: true,
                        ap_active: false,
                        sta_ip: '192.168.1.23',
                        ap_ip: '192.168.4.1',
                        ssid: 'GarageNet',
                        rssi: -54
                    },
                    device: {
                        uptime: 3665,
                        heap_free: 49152,
                        hostname: 'v1simple',
                        firmware_version: '1.2.3'
                    },
                    maintenanceBoot: false,
                    maintenanceBootUptimeMs: 0,
                    v1_connected: true,
                    alert: null
                })
            },
            {
                method: 'GET',
                match: '/api/device/settings',
                respond: jsonResponse({ proxy_ble: false, proxy_name: 'V1-Proxy' })
            },
            {
                method: 'GET',
                match: '/api/obd/config',
                respond: jsonResponse({ enabled: false })
            }
        ],
        jsonResponse({})
    );
}

function countCalls(fetchMock, url) {
    return fetchMock.mock.calls.filter(([requestUrl]) => requestUrl === url).length;
}

describe('dashboard route page', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
    });

    it('loads shared runtime status on mount', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('Connected');
        await screen.findByText('GarageNet • -54 dBm');
        await waitFor(() => {
            expect(countCalls(fetchMock, '/api/status')).toBeGreaterThanOrEqual(1);
        });

        unmount();
    });

    it('polls status every 3s through the shared runtime module', async () => {
        vi.useFakeTimers();
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await Promise.resolve();
        expect(countCalls(fetchMock, '/api/status')).toBe(1);

        await vi.advanceTimersByTimeAsync(9000);

        expect(countCalls(fetchMock, '/api/status')).toBe(4);

        unmount();
    });

    it('shows a shared status api error when /api/status returns 500', async () => {
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/status',
                respond: jsonResponse({ error: 'nope' }, 500)
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('API error');
        expect(countCalls(fetchMock, '/api/status')).toBeGreaterThanOrEqual(1);

        unmount();
    });

    it('shows a shared status connection error when /api/status throws', async () => {
        const fetchMock = installFetchMock(
            [
                {
                    method: 'GET',
                    match: '/api/status',
                    respond: () => {
                        throw new Error('network down');
                    }
                }
            ],
            jsonResponse({})
        );
        const { unmount } = render(Page);

        await screen.findByText('Connection lost');
        expect(countCalls(fetchMock, '/api/status')).toBeGreaterThanOrEqual(1);

        unmount();
    });

    it('does not render live alert details from runtime status', async () => {
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/status',
                respond: jsonResponse({
                    wifi: {
                        sta_connected: true,
                        ap_active: false,
                        sta_ip: '192.168.1.23',
                        ap_ip: '192.168.4.1',
                        ssid: 'GarageNet',
                        rssi: -54
                    },
                    device: {
                        uptime: 3665,
                        heap_free: 49152,
                        hostname: 'v1simple',
                        firmware_version: '1.2.3'
                    },
                    maintenanceBoot: false,
                    maintenanceBootUptimeMs: 0,
                    v1_connected: true,
                    alert: {
                        active: true,
                        band: 'Ka',
                        frequency: 34700,
                        strength: 7
                    }
                })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Live radar/ALP alerts stay on the LCD');
        expect(screen.queryByText('Ka')).not.toBeInTheDocument();
        expect(screen.queryByText('34700 MHz')).not.toBeInTheDocument();
        expect(screen.queryByText(/Strength:/)).not.toBeInTheDocument();

        unmount();
    });

    it('hides runtime-only dashboard cards in maintenance mode', async () => {
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/status',
                respond: jsonResponse({
                    wifi: {
                        sta_connected: false,
                        ap_active: true,
                        sta_ip: '',
                        ap_ip: '192.168.35.5',
                        ssid: 'V1-Simple',
                        rssi: 0
                    },
                    device: {
                        uptime: 42,
                        heap_free: 49152,
                        hostname: 'v1simple',
                        firmware_version: '1.2.3'
                    },
                    maintenanceBoot: true,
                    maintenanceBootUptimeMs: 125000,
                    v1_connected: false,
                    alert: null
                })
            }
        ]);
        const { unmount } = render(Page);

        await waitFor(() => {
            expect(screen.queryByText('Valentine One')).not.toBeInTheDocument();
        });
        expect(
            screen.queryByText('V1 BLE is paused; profile work is offline.')
        ).not.toBeInTheDocument();
        expect(screen.queryByText('Inactive in maintenance mode')).not.toBeInTheDocument();
        expect(screen.queryByText('Maintenance session • 48 KB free')).not.toBeInTheDocument();
        expect(screen.queryByText('2m 5s')).not.toBeInTheDocument();
        expect(await screen.findByText('Display only saved')).toBeInTheDocument();
        expect(screen.getByText(/Saved mode applies on next normal boot/)).toBeInTheDocument();
        expect(screen.getByRole('button', { name: /save operating mode/i })).toBeDisabled();

        unmount();
    });

    it('shows explicit display-only, OBD, and proxy mode choices with TLDR copy', async () => {
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/device/settings',
                respond: jsonResponse({ proxy_ble: true, proxy_name: 'V1-Proxy' })
            },
            {
                method: 'GET',
                match: '/api/obd/config',
                respond: jsonResponse({ enabled: false })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Proxy / App saved');
        expect(screen.getByText(/owns the local display and logging only/)).toBeInTheDocument();
        expect(screen.getByText(/V1 Simple owns local speed mute/)).toBeInTheDocument();
        expect(screen.getByText(/relays raw V1 data/)).toBeInTheDocument();
        expect(screen.getByText('Proxy / App').closest('button')).toHaveAttribute(
            'aria-pressed',
            'true'
        );
        expect(screen.getByLabelText('Proxy Name')).toHaveValue('V1-Proxy');

        unmount();
    });

    it('saves the proxy advertising name from the dashboard mode card', async () => {
        let deviceSettings = { proxy_ble: true, proxy_name: 'V1-Proxy' };
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/device/settings',
                respond: () => jsonResponse(deviceSettings)
            },
            {
                method: 'GET',
                match: '/api/obd/config',
                respond: jsonResponse({ enabled: false })
            },
            {
                method: 'POST',
                match: '/api/device/settings',
                respond: ({ init }) => {
                    deviceSettings = {
                        ...deviceSettings,
                        proxy_name: init.body.get('proxy_name') || deviceSettings.proxy_name
                    };
                    return jsonResponse({ success: true });
                }
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Proxy / App saved');
        const input = await screen.findByLabelText('Proxy Name');
        await fireEvent.input(input, { target: { value: 'Garage Proxy' } });
        await fireEvent.click(screen.getByRole('button', { name: /save proxy name/i }));

        await screen.findByText('Proxy name saved.');
        const posts = fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST');
        expect(posts).toHaveLength(1);
        expect(posts[0][0]).toBe('/api/device/settings');
        expect(posts[0][1].body.get('proxy_name')).toBe('Garage Proxy');
        expect(posts[0][1].body.has('proxy_ble')).toBe(false);
        expect(await screen.findByDisplayValue('Garage Proxy')).toBeInTheDocument();

        unmount();
    });

    it('shows standalone mode truthfully and saves it by disabling proxy and OBD', async () => {
        let deviceSettings = { proxy_ble: true, proxy_name: 'V1-Proxy' };
        let obdSettings = { enabled: false };
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/device/settings',
                respond: () => jsonResponse(deviceSettings)
            },
            {
                method: 'GET',
                match: '/api/obd/config',
                respond: () => jsonResponse(obdSettings)
            },
            {
                method: 'POST',
                match: '/api/device/settings',
                respond: ({ init }) => {
                    deviceSettings = {
                        ...deviceSettings,
                        proxy_ble: init.body.get('proxy_ble') === 'true',
                        proxy_name: init.body.get('proxy_name') || deviceSettings.proxy_name
                    };
                    return jsonResponse({ success: true });
                }
            },
            {
                method: 'POST',
                match: '/api/obd/config',
                respond: ({ init }) => {
                    obdSettings = { ...obdSettings, ...JSON.parse(init.body) };
                    return jsonResponse({ success: true });
                }
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Proxy / App saved');
        await fireEvent.click(screen.getByRole('button', { name: /Display only/ }));

        await screen.findByText('Display only pending save');
        await screen.findByText('Display only is selected but not saved yet.');
        expect(fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST')).toHaveLength(0);
        await fireEvent.click(screen.getByRole('button', { name: /save operating mode/i }));

        await screen.findByText('Display only mode saved. Reboot normally to apply.');
        await screen.findByText('Display only saved');
        const posts = fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST');
        expect(posts.map(([url]) => url)).toEqual(['/api/device/settings', '/api/obd/config']);
        expect(posts[0][1].body.get('proxy_ble')).toBe('false');
        expect(JSON.parse(posts[1][1].body)).toEqual({ enabled: false });

        unmount();
    });

    it('saves Proxy / App mode explicitly by disabling OBD before enabling proxy', async () => {
        let deviceSettings = { proxy_ble: false, proxy_name: 'V1-Proxy' };
        let obdSettings = { enabled: true };
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/device/settings',
                respond: () => jsonResponse(deviceSettings)
            },
            {
                method: 'GET',
                match: '/api/obd/config',
                respond: () => jsonResponse(obdSettings)
            },
            {
                method: 'POST',
                match: '/api/obd/config',
                respond: ({ init }) => {
                    obdSettings = { ...obdSettings, ...JSON.parse(init.body) };
                    return jsonResponse({ success: true });
                }
            },
            {
                method: 'POST',
                match: '/api/device/settings',
                respond: ({ init }) => {
                    deviceSettings = {
                        ...deviceSettings,
                        proxy_ble: init.body.get('proxy_ble') === 'true',
                        proxy_name: init.body.get('proxy_name') || deviceSettings.proxy_name
                    };
                    return jsonResponse({ success: true });
                }
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('OBD saved');
        await fireEvent.click(screen.getByRole('button', { name: /Proxy \/ App/ }));

        await screen.findByText('Proxy / App pending save');
        await screen.findByText('Proxy / App is selected but not saved yet.');
        expect(fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST')).toHaveLength(0);
        await fireEvent.click(screen.getByRole('button', { name: /save operating mode/i }));

        await screen.findByText('Proxy / App mode saved. Reboot normally to apply.');
        await screen.findByText('Proxy / App saved');
        const posts = fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST');
        expect(posts.map(([url]) => url)).toEqual(['/api/obd/config', '/api/device/settings']);
        expect(JSON.parse(posts[0][1].body)).toEqual({ enabled: false });
        expect(posts[1][1].body.get('proxy_ble')).toBe('true');

        unmount();
    });

    it('saves OBD mode explicitly by disabling proxy before enabling OBD', async () => {
        let deviceSettings = { proxy_ble: true, proxy_name: 'V1-Proxy' };
        let obdSettings = { enabled: false };
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/device/settings',
                respond: () => jsonResponse(deviceSettings)
            },
            {
                method: 'GET',
                match: '/api/obd/config',
                respond: () => jsonResponse(obdSettings)
            },
            {
                method: 'POST',
                match: '/api/device/settings',
                respond: ({ init }) => {
                    deviceSettings = {
                        ...deviceSettings,
                        proxy_ble: init.body.get('proxy_ble') === 'true',
                        proxy_name: init.body.get('proxy_name') || deviceSettings.proxy_name
                    };
                    return jsonResponse({ success: true });
                }
            },
            {
                method: 'POST',
                match: '/api/obd/config',
                respond: ({ init }) => {
                    obdSettings = { ...obdSettings, ...JSON.parse(init.body) };
                    return jsonResponse({ success: true });
                }
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Proxy / App saved');
        await fireEvent.click(screen.getByRole('button', { name: /^OBD/ }));

        await screen.findByText('OBD pending save');
        await screen.findByText('OBD is selected but not saved yet.');
        expect(fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST')).toHaveLength(0);
        await fireEvent.click(screen.getByRole('button', { name: /save operating mode/i }));

        await screen.findByText('OBD mode saved. Reboot normally to apply.');
        await screen.findByText('OBD saved');
        const posts = fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST');
        expect(posts.map(([url]) => url)).toEqual(['/api/device/settings', '/api/obd/config']);
        expect(posts[0][1].body.get('proxy_ble')).toBe('false');
        expect(JSON.parse(posts[1][1].body)).toEqual({ enabled: true });

        unmount();
    });
});
