import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../test/fetch-mock.js';
import { refreshDeviceSettings } from '#lib/stores/deviceSettings.svelte.js';
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

function installModeFetch(initial = {}, overrides = []) {
    const state = { proxy_ble: true, proxy_name: 'V1-Proxy', enabled: false, ...initial };
    const fetchMock = installDefaultFetch([
        ...overrides,
        {
            method: 'GET', match: '/api/device/settings',
            respond: () => jsonResponse({ proxy_ble: state.proxy_ble, proxy_name: state.proxy_name })
        },
        {
            method: 'GET', match: '/api/obd/config',
            respond: () => jsonResponse({ enabled: state.enabled })
        },
        {
            method: 'POST', match: '/api/device/settings',
            respond: ({ init }) => {
                if (init.body.has('proxy_ble')) {
                    state.proxy_ble = init.body.get('proxy_ble') === 'true';
                    // Enabling either mode disables the other in the firmware's settings mutation.
                    if (state.proxy_ble) state.enabled = false;
                }
                if (init.body.has('proxy_name')) state.proxy_name = init.body.get('proxy_name');
                return jsonResponse({ success: true });
            }
        },
        {
            method: 'POST', match: '/api/obd/config',
            respond: ({ init }) => {
                state.enabled = JSON.parse(init.body).enabled;
                if (state.enabled) state.proxy_ble = false;
                return jsonResponse({ success: true });
            }
        }
    ]);
    return { state, fetchMock };
}

describe('dashboard route page', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
    });

    it('loads maintenance-compatible shared runtime status on mount', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('STA connected');
        expect(screen.getByText(/GarageNet/)).toHaveTextContent('GarageNet • -54 dBm');
        expect(screen.getByText(/Maintenance status is not confirmed/)).toBeInTheDocument();
        await waitFor(() => {
            expect(countCalls(fetchMock, '/api/status')).toBeGreaterThanOrEqual(1);
        });

        unmount();
    });

    it.each([
        ['unknown', true, 'Power source unavailable'],
        ['unknown', false, 'Power source unavailable'],
        ['battery', false, 'On battery power'],
        ['usb', true, 'External power'],
        [undefined, true, 'On battery power'],
        [undefined, false, 'External power']
    ])('uses battery source %s ahead of legacy on_battery=%s', async (source, onBattery, expected) => {
        installDefaultFetch([{
            method: 'GET', match: '/api/status', respond: jsonResponse({
                wifi: { sta_connected: false, ap_active: true },
                maintenanceBoot: true,
                battery: { has_battery: true, on_battery: onBattery, source,
                    percentage: 75, voltage_mv: 3900 }
            })
        }]);
        const { unmount } = render(Page);
        const caption = await screen.findByText(new RegExp(expected));
        expect(caption).toHaveTextContent(`${expected} • 3900 mV`);
        if (source === 'unknown') {
            expect(screen.queryByText(/On battery power|External power/)).not.toBeInTheDocument();
        }
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

        await screen.findByText('Device status unavailable');
        expect(screen.getByText(/maintenance shell remains available/i)).toBeInTheDocument();
        expect(screen.getByRole('link', { name: 'First use guide' })).toHaveAttribute('href', '/first-use.html');
        expect(screen.getByRole('link', { name: 'First use guide' })).toHaveAttribute('data-sveltekit-reload');
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

        await screen.findByText('Device status unavailable');
        expect(screen.getByText(/maintenance shell remains available/i)).toBeInTheDocument();
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

        await screen.findByText('STA connected');
        expect(screen.queryByText('Ka')).not.toBeInTheDocument();
        expect(screen.queryByText('34700 MHz')).not.toBeInTheDocument();
        expect(screen.queryByText(/Strength:/)).not.toBeInTheDocument();
        expect(screen.queryByText(/Live radar|Live detector/i)).not.toBeInTheDocument();
        expect(screen.queryByText(/Storage usage|Alert history|Persisted history/i)).not.toBeInTheDocument();

        unmount();
    });

    it('shows a maintenance-compatible loading state before the first status response', async () => {
        let resolveStatus;
        const statusResponse = new Promise((resolve) => {
            resolveStatus = resolve;
        });
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/status',
                respond: () => statusResponse
            }
        ]);
        const { unmount } = render(Page);

        expect(await screen.findByText('Checking maintenance device status…')).toBeInTheDocument();
        expect(screen.queryByText('Valentine One')).not.toBeInTheDocument();
        expect(screen.queryByText(/Live radar|Live detector/i)).not.toBeInTheDocument();

        resolveStatus(jsonResponse({
            wifi: { sta_connected: false, ap_active: true, sta_ip: '', ap_ip: '192.168.35.5', ssid: 'V1-Simple', rssi: 0 },
            device: { uptime: 42, heap_free: 49152, hostname: 'v1simple', firmware_version: '1.2.3' },
            battery: { has_battery: false, on_battery: false, percentage: 0, voltage_mv: 0 },
            maintenanceBoot: true,
            maintenanceBootUptimeMs: 1000
        }));
        expect(await screen.findByText('Access point active')).toBeInTheDocument();

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
        expect(screen.queryByText(/Storage usage|Alert history|Persisted history/i)).not.toBeInTheDocument();
        expect(screen.queryByText(/Live radar|Live detector/i)).not.toBeInTheDocument();
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
        expect(screen.getByText(/owns the local display\. OBD speed and phone proxy are off/)).toBeInTheDocument();
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

    it.each([
        ['OBD', 'Proxy / App', { proxy_ble: false, enabled: true }, '/api/device/settings'],
        ['Display only', 'Proxy / App', { proxy_ble: false, enabled: false }, '/api/device/settings'],
        ['Proxy / App', 'OBD', { proxy_ble: true, enabled: false }, '/api/obd/config'],
        ['Display only', 'OBD', { proxy_ble: false, enabled: false }, '/api/obd/config']
    ])('switches %s to %s with one firmware-owned mode mutation', async (from, target, initial, endpoint) => {
        const { state, fetchMock } = installModeFetch(initial);
        const { unmount } = render(Page);
        await screen.findByText(`${from} saved`);
        await fireEvent.click(screen.getByRole('button', { name: new RegExp(`^${target}`) }));
        await screen.findByText(`${target} pending save`);
        expect(fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST')).toHaveLength(0);
        await fireEvent.click(screen.getByRole('button', { name: /save operating mode/i }));
        await screen.findByText(`${target} mode saved. Reboot normally to apply.`);
        await screen.findByText(`${target} saved`);
        const posts = fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST');
        expect(posts.map(([url]) => url)).toEqual([endpoint]);
        expect(state.proxy_ble).toBe(target === 'Proxy / App');
        expect(state.enabled).toBe(target === 'OBD');
        const lastPost = fetchMock.mock.calls.findLastIndex(([, init]) => init?.method === 'POST');
        expect(fetchMock.mock.calls.slice(lastPost + 1).some(([url]) => url === '/api/device/settings')).toBe(true);
        unmount();
    });

    it('saves a dirty name before OBD without first disabling the current mode', async () => {
        const { state, fetchMock } = installModeFetch();
        const { unmount } = render(Page);
        await screen.findByText('Proxy / App saved');
        await fireEvent.input(screen.getByLabelText('Proxy Name'), { target: { value: 'Road Proxy' } });
        await fireEvent.click(screen.getByRole('button', { name: /^OBD/ }));
        await fireEvent.click(screen.getByRole('button', { name: /save operating mode/i }));
        await screen.findByText('OBD mode saved. Reboot normally to apply.');
        const posts = fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST');
        expect(posts.map(([url]) => url)).toEqual(['/api/device/settings', '/api/obd/config']);
        expect([...posts[0][1].body.entries()]).toEqual([['proxy_name', 'Road Proxy']]);
        expect(state).toEqual({ proxy_ble: false, enabled: true, proxy_name: 'Road Proxy' });
        expect(screen.getByLabelText('Proxy Name')).toHaveValue('Road Proxy');
        unmount();
    });

    it.each([
        ['Proxy / App', { proxy_ble: false, enabled: true }, '/api/device/settings', 'OBD'],
        ['OBD', { proxy_ble: true, enabled: false }, '/api/obd/config', 'Proxy / App']
    ])('retains the saved mode when enabling %s fails', async (target, initial, endpoint, previous) => {
        vi.spyOn(console, 'error').mockImplementation(() => {});
        const { state, fetchMock } = installModeFetch(initial, [
            { method: 'POST', match: endpoint, respond: jsonResponse({ error: 'save failed' }, 500) }
        ]);
        const { unmount } = render(Page);
        await screen.findByText(`${previous} saved`);
        await fireEvent.click(screen.getByRole('button', { name: new RegExp(`^${target}`) }));
        await fireEvent.click(screen.getByRole('button', { name: /save operating mode/i }));
        await screen.findByText('Failed to save operating mode.');
        await screen.findByText(`${previous} saved`);
        expect(state).toMatchObject(initial);
        expect(fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST')).toHaveLength(1);
        expect(screen.queryByText(`${target} mode saved. Reboot normally to apply.`)).not.toBeInTheDocument();
        unmount();
    });

    it('keeps a rejected name draft and does not start the OBD change', async () => {
        vi.spyOn(console, 'error').mockImplementation(() => {});
        const { state, fetchMock } = installModeFetch({}, [
            { method: 'POST', match: '/api/device/settings', respond: jsonResponse({ error: 'save failed' }, 500) }
        ]);
        const { unmount } = render(Page);
        await screen.findByText('Proxy / App saved');
        await fireEvent.input(screen.getByLabelText('Proxy Name'), { target: { value: 'Unsaved Proxy' } });
        await fireEvent.click(screen.getByRole('button', { name: /^OBD/ }));
        await fireEvent.click(screen.getByRole('button', { name: /save operating mode/i }));
        await screen.findByText('Failed to save operating mode.');
        await waitFor(() => expect(screen.getByRole('button', { name: /save proxy name/i })).toBeEnabled());
        expect(screen.getByLabelText('Proxy Name')).toHaveValue('Unsaved Proxy');
        expect(state).toEqual({ proxy_ble: true, enabled: false, proxy_name: 'V1-Proxy' });
        const posts = fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST');
        expect(posts).toHaveLength(1);
        expect(posts[0][1].body.has('proxy_ble')).toBe(false);
        unmount();
    });

    it('reports a saved name separately when the following OBD change fails', async () => {
        vi.spyOn(console, 'error').mockImplementation(() => {});
        const { state } = installModeFetch({}, [
            { method: 'POST', match: '/api/obd/config', respond: jsonResponse({ error: 'save failed' }, 500) }
        ]);
        const { unmount } = render(Page);
        await screen.findByText('Proxy / App saved');
        await fireEvent.input(screen.getByLabelText('Proxy Name'), { target: { value: 'Road Proxy' } });
        await fireEvent.click(screen.getByRole('button', { name: /^OBD/ }));
        await fireEvent.click(screen.getByRole('button', { name: /save operating mode/i }));
        await screen.findByText('Proxy name saved, but failed to save operating mode.');
        await screen.findByText('Proxy / App saved');
        expect(state).toEqual({ proxy_ble: true, enabled: false, proxy_name: 'Road Proxy' });
        expect(screen.getByLabelText('Proxy Name')).toHaveValue('Road Proxy');
        unmount();
    });

    it('reloads the actual saved mode after a lost OBD response without claiming success', async () => {
        vi.spyOn(console, 'error').mockImplementation(() => {});
        const { state, fetchMock } = installModeFetch({}, [{
            method: 'POST', match: '/api/obd/config', respond: () => {
                state.enabled = true;
                state.proxy_ble = false;
                throw new Error('response lost');
            }
        }]);
        const { unmount } = render(Page);
        await screen.findByText('Proxy / App saved');
        await fireEvent.click(screen.getByRole('button', { name: /^OBD/ }));
        await fireEvent.click(screen.getByRole('button', { name: /save operating mode/i }));
        await screen.findByText('Failed to save operating mode.');
        await screen.findByText('OBD saved');
        expect(fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST')).toHaveLength(1);
        expect(screen.queryByText('OBD mode saved. Reboot normally to apply.')).not.toBeInTheDocument();
        unmount();
    });

    it('invalidates a pre-write settings poll after a lost OBD response', async () => {
        vi.spyOn(console, 'error').mockImplementation(() => {});
        let holdSettings = false;
        let releaseSettings;
        const { state, fetchMock } = installModeFetch({}, [
            {
                method: 'GET', match: '/api/device/settings', respond: () => {
                    const response = jsonResponse({ proxy_ble: state.proxy_ble, proxy_name: state.proxy_name });
                    if (!holdSettings) return response;
                    holdSettings = false;
                    return new Promise((resolve) => { releaseSettings = () => resolve(response); });
                }
            },
            {
                method: 'POST', match: '/api/obd/config', respond: () => {
                    state.enabled = true;
                    state.proxy_ble = false;
                    throw new Error('response lost after commit');
                }
            }
        ]);
        const { unmount } = render(Page);
        await screen.findByText('Proxy / App saved');
        // Start the real shared poll owner, holding its pre-write observation.
        holdSettings = true;
        const pendingPoll = refreshDeviceSettings();
        const readsBeforeWrite = countCalls(fetchMock, '/api/device/settings');
        await fireEvent.click(screen.getByRole('button', { name: /^OBD/ }));
        await fireEvent.click(screen.getByRole('button', { name: /save operating mode/i }));
        await screen.findByText('Failed to save operating mode.');
        releaseSettings();
        await pendingPoll;
        await screen.findByText('OBD saved');
        expect(countCalls(fetchMock, '/api/device/settings')).toBeGreaterThan(readsBeforeWrite);
        expect(screen.queryByText('Proxy / App saved')).not.toBeInTheDocument();
        expect(screen.queryByText('OBD mode saved. Reboot normally to apply.')).not.toBeInTheDocument();
        unmount();
    });

    it('reconciles a lost name-save response before retrying the combined OBD change', async () => {
        vi.spyOn(console, 'error').mockImplementation(() => {});
        let holdSettings = false;
        let releaseSettings;
        let loseNameResponse = true;
        const { state, fetchMock } = installModeFetch({}, [
            {
                method: 'GET', match: '/api/device/settings', respond: () => {
                    const response = jsonResponse({ proxy_ble: state.proxy_ble, proxy_name: state.proxy_name });
                    if (!holdSettings) return response;
                    holdSettings = false;
                    return new Promise((resolve) => { releaseSettings = () => resolve(response); });
                }
            },
            {
                method: 'POST', match: '/api/device/settings', respond: ({ init }) => {
                    state.proxy_name = init.body.get('proxy_name');
                    if (loseNameResponse) {
                        loseNameResponse = false;
                        throw new Error('name response lost after commit');
                    }
                    return jsonResponse({ success: true });
                }
            }
        ]);
        const { unmount } = render(Page);
        await screen.findByText('Proxy / App saved');
        holdSettings = true;
        const pendingPoll = refreshDeviceSettings();
        await fireEvent.input(screen.getByLabelText('Proxy Name'), { target: { value: 'Road Proxy' } });
        await fireEvent.click(screen.getByRole('button', { name: /^OBD/ }));
        await fireEvent.click(screen.getByRole('button', { name: /save operating mode/i }));
        await screen.findByText('Failed to save operating mode.');
        releaseSettings();
        await pendingPoll;
        await screen.findByText('Proxy / App saved');
        expect(screen.getByLabelText('Proxy Name')).toHaveValue('Road Proxy');
        expect(state).toEqual({ proxy_ble: true, enabled: false, proxy_name: 'Road Proxy' });
        expect(fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST')).toHaveLength(1);

        await fireEvent.click(screen.getByRole('button', { name: /^OBD/ }));
        await fireEvent.click(screen.getByRole('button', { name: /save operating mode/i }));
        await screen.findByText('OBD mode saved. Reboot normally to apply.');
        const posts = fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST');
        expect(posts.map(([url]) => url)).toEqual(['/api/device/settings', '/api/obd/config']);
        unmount();
    });

    it('reports a failed final reload instead of a successful mode save', async () => {
        vi.spyOn(console, 'error').mockImplementation(() => {});
        const { state } = installModeFetch({}, [{
            method: 'GET', match: '/api/obd/config',
            respond: () => state.enabled
                ? jsonResponse({ error: 'read failed' }, 500)
                : jsonResponse({ enabled: false })
        }]);
        const { unmount } = render(Page);
        await screen.findByText('Proxy / App saved');
        await fireEvent.input(screen.getByLabelText('Proxy Name'), { target: { value: 'Road Proxy' } });
        await fireEvent.click(screen.getByRole('button', { name: /^OBD/ }));
        await fireEvent.click(screen.getByRole('button', { name: /save operating mode/i }));
        await screen.findByText('Failed to load operating mode.');
        expect(screen.queryByText('OBD mode saved. Reboot normally to apply.')).not.toBeInTheDocument();
        expect(screen.getByLabelText('Proxy Name')).toHaveValue('Road Proxy');
        expect(state).toEqual({ proxy_ble: false, enabled: true, proxy_name: 'Road Proxy' });
        unmount();
    });

    it('blocks further mode and name changes while the mode write is pending', async () => {
        let finishWrite;
        const response = new Promise((resolve) => { finishWrite = resolve; });
        const { state, fetchMock } = installModeFetch({}, [{
            method: 'POST', match: '/api/obd/config', respond: () => response
        }]);
        const { unmount } = render(Page);
        await screen.findByText('Proxy / App saved');
        await fireEvent.click(screen.getByRole('button', { name: /^OBD/ }));
        const save = screen.getByRole('button', { name: /save operating mode/i });
        await fireEvent.click(save);
        expect(save).toBeDisabled();
        expect(screen.getByRole('button', { name: /^Proxy \/ App/ })).toBeDisabled();
        expect(screen.getByRole('button', { name: /save proxy name/i })).toBeDisabled();
        expect(screen.getByLabelText('Proxy Name')).toBeDisabled();
        await fireEvent.click(save);
        expect(fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST')).toHaveLength(1);
        state.enabled = true;
        state.proxy_ble = false;
        finishWrite(jsonResponse({ success: true }));
        await screen.findByText('OBD mode saved. Reboot normally to apply.');
        unmount();
    });

    it('keeps the draft when an explicit proxy name save fails', async () => {
        vi.spyOn(console, 'error').mockImplementation(() => {});
        installModeFetch({}, [{
            method: 'POST', match: '/api/device/settings', respond: jsonResponse({ error: 'save failed' }, 500)
        }]);
        const { unmount } = render(Page);
        await screen.findByText('Proxy / App saved');
        await fireEvent.input(screen.getByLabelText('Proxy Name'), { target: { value: 'Unsaved Proxy' } });
        await fireEvent.click(screen.getByRole('button', { name: /save proxy name/i }));
        await screen.findByText('Failed to save proxy name.');
        await waitFor(() => expect(screen.getByRole('button', { name: /save proxy name/i })).toBeEnabled());
        expect(screen.getByLabelText('Proxy Name')).toHaveValue('Unsaved Proxy');
        unmount();
    });
});
