import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, installFixtureFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function countCalls(fetchMock, url) {
    return fetchMock.mock.calls.filter(([requestUrl]) => requestUrl === url).length;
}

function installDefaultFetch() {
    return installFetchMock(
        [
            {
                method: 'GET',
                match: '/api/status',
                respond: jsonResponse({ maintenanceBoot: false, maintenanceBootUptimeMs: 0 })
            },
            {
                method: 'GET',
                match: '/api/obd/config',
                respond: jsonResponse({ enabled: false, minRssi: -80 })
            },
            {
                method: 'GET',
                match: '/api/obd/devices',
                respond: jsonResponse({
                    devices: [
                        {
                            address: 'A4:C1:38:00:11:22',
                            name: 'Truck Adapter',
                            connected: false,
                            active: true
                        }
                    ]
                })
            },
            {
                method: 'GET',
                match: '/api/obd/status',
                respond: jsonResponse({
                    enabled: false,
                    connected: false,
                    pollCount: 0,
                    pollErrors: 0,
                    savedAddress: 'A4:C1:38:00:11:22'
                })
            },
            { method: 'POST', match: '/api/obd/config', respond: jsonResponse({ success: true }) },
            {
                method: 'POST',
                match: '/api/obd/devices/name',
                respond: jsonResponse({ success: true })
            },
            { method: 'POST', match: '/api/obd/scan', respond: jsonResponse({ success: true }) },
            { method: 'POST', match: '/api/obd/forget', respond: jsonResponse({ success: true }) }
        ],
        jsonResponse({})
    );
}

describe('obd route page', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
    });

    it('re-fetches the captured canonical config after an out-of-range save', async () => {
        const fetchMock = installFixtureFetchMock('obd_config_invalid_value', [
            {
                method: 'GET',
                match: '/api/status',
                respond: jsonResponse({ maintenanceBoot: true, maintenanceBootUptimeMs: 15000 })
            },
            {
                method: 'GET',
                match: '/api/obd/devices',
                respond: jsonResponse({ devices: [] })
            }
        ]);
        const { unmount } = render(Page);

        const input = await screen.findByLabelText(/Min RSSI \(dBm\)/i);
        expect(input).toHaveValue(-80);

        await fireEvent.input(input, { target: { value: '-10' } });
        await fireEvent.change(input);

        await waitFor(() => expect(input).toHaveValue(-40));
        expect(countCalls(fetchMock, '/api/obd/config')).toBe(3);

        unmount();
    });

    it('shows an OBD settings error when the OBD settings fetch fails on mount', async () => {
        const errorSpy = vi.spyOn(console, 'error').mockImplementation(() => {});
        installFetchMock(
            [
                {
                    method: 'GET',
                    match: '/api/status',
                    respond: jsonResponse({ maintenanceBoot: false, maintenanceBootUptimeMs: 0 })
                },
                {
                    method: 'GET',
                    match: '/api/obd/config',
                    respond: () => Promise.reject(new Error('settings unavailable'))
                },
                {
                    method: 'GET',
                    match: '/api/obd/status',
                    respond: jsonResponse({
                        enabled: false,
                        connected: false,
                        pollCount: 0,
                        pollErrors: 0
                    })
                },
                {
                    method: 'POST',
                    match: '/api/obd/config',
                    respond: jsonResponse({ success: true })
                },
                {
                    method: 'POST',
                    match: '/api/obd/scan',
                    respond: jsonResponse({ success: true })
                },
                {
                    method: 'POST',
                    match: '/api/obd/forget',
                    respond: jsonResponse({ success: true })
                }
            ],
            jsonResponse({})
        );
        const { unmount } = render(Page);

        await screen.findByText('Failed to load OBD settings.');
        expect(errorSpy).toHaveBeenCalled();

        unmount();
    });

    it('shows saved OBD devices and lets you rename them', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('Truck Adapter');
        await fireEvent.click(screen.getByRole('button', { name: /^rename$/i }));

        const input = await screen.findByDisplayValue('Truck Adapter');
        await fireEvent.input(input, { target: { value: 'Family Car' } });
        await fireEvent.click(screen.getByRole('button', { name: /^save$/i }));

        await screen.findByText('OBD device name saved.');
        await waitFor(() => {
            expect(screen.getByText('Family Car')).toBeInTheDocument();
        });
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === '/api/obd/devices/name' && init?.method === 'POST'
            )
        ).toBe(true);
        expect(fetchMock.mock.calls.some(([url]) => String(url).startsWith('/api/gps'))).toBe(
            false
        );

        unmount();
    });

    it('maps OBD runtime state codes to the correct labels', async () => {
        installFetchMock(
            [
                {
                    method: 'GET',
                    match: '/api/status',
                    respond: jsonResponse({ maintenanceBoot: false, maintenanceBootUptimeMs: 0 })
                },
                {
                    method: 'GET',
                    match: '/api/obd/config',
                    respond: jsonResponse({ enabled: true, minRssi: -80 })
                },
                {
                    method: 'GET',
                    match: '/api/obd/status',
                    respond: jsonResponse({
                        enabled: true,
                        connected: true,
                        securityReady: true,
                        encrypted: true,
                        bonded: true,
                        speedValid: true,
                        speedMph: 0,
                        speedAgeMs: 12,
                        rssi: -65,
                        scanInProgress: false,
                        savedAddressValid: true,
                        pollCount: 166,
                        pollErrors: 0,
                        state: 8
                    })
                },
                {
                    method: 'POST',
                    match: '/api/obd/config',
                    respond: jsonResponse({ success: true })
                },
                {
                    method: 'POST',
                    match: '/api/obd/scan',
                    respond: jsonResponse({ success: true })
                },
                {
                    method: 'POST',
                    match: '/api/obd/forget',
                    respond: jsonResponse({ success: true })
                }
            ],
            jsonResponse({})
        );
        const { unmount } = render(Page);

        await screen.findByText('Polling');
        expect(screen.queryByText('ErrorBackoff')).not.toBeInTheDocument();

        unmount();
    });

    it('hides live OBD status and scan actions in maintenance mode', async () => {
        installFixtureFetchMock(
            ['obd_config_invalid_value', 'obd_maintenance_routes'],
            [
                {
                    method: 'GET',
                    match: '/api/status',
                    respond: jsonResponse({ maintenanceBoot: true, maintenanceBootUptimeMs: 15000 })
                }
            ]
        );
        const { unmount } = render(Page);

        await screen.findByText(/OBD live state is not running in maintenance mode/);
        expect(screen.queryByText('Polling')).not.toBeInTheDocument();
        expect(screen.queryByText(/Polls:/)).not.toBeInTheDocument();
        expect(screen.getByRole('button', { name: /scan now/i })).toBeDisabled();
        expect(screen.queryByText('Connected')).not.toBeInTheDocument();
        expect(screen.getByText('Saved')).toBeInTheDocument();
        expect(screen.getByLabelText(/Min RSSI/i)).toBeInTheDocument();

        unmount();
    });

    it('reconciles the OBD toggle from backend truth after a failed save', async () => {
        const fetchMock = installFetchMock(
            [
                {
                    method: 'GET',
                    match: '/api/status',
                    respond: jsonResponse({ maintenanceBoot: false, maintenanceBootUptimeMs: 0 })
                },
                {
                    method: 'GET',
                    match: '/api/obd/config',
                    respond: jsonResponse({ enabled: false, minRssi: -80 })
                },
                {
                    method: 'GET',
                    match: '/api/obd/status',
                    respond: jsonResponse({
                        enabled: false,
                        connected: false,
                        pollCount: 0,
                        pollErrors: 0
                    })
                },
                {
                    method: 'POST',
                    match: '/api/obd/config',
                    respond: jsonResponse({ success: false }, 500)
                },
                {
                    method: 'POST',
                    match: '/api/obd/scan',
                    respond: jsonResponse({ success: true })
                },
                {
                    method: 'POST',
                    match: '/api/obd/forget',
                    respond: jsonResponse({ success: true })
                }
            ],
            jsonResponse({})
        );
        const { unmount } = render(Page);

        const toggle = await screen.findByRole('checkbox', { name: /enable obd/i });
        expect(toggle).not.toBeChecked();

        await fireEvent.click(toggle);

        await screen.findByText('Failed to save OBD setting.');
        await waitFor(() => expect(toggle).not.toBeChecked());
        expect(countCalls(fetchMock, '/api/obd/config')).toBeGreaterThanOrEqual(3);

        unmount();
    });

    it('reconciles the OBD min RSSI input after a failed save', async () => {
        installFetchMock(
            [
                {
                    method: 'GET',
                    match: '/api/status',
                    respond: jsonResponse({ maintenanceBoot: false, maintenanceBootUptimeMs: 0 })
                },
                {
                    method: 'GET',
                    match: '/api/obd/config',
                    respond: jsonResponse({ enabled: true, minRssi: -80 })
                },
                {
                    method: 'GET',
                    match: '/api/obd/status',
                    respond: jsonResponse({
                        enabled: true,
                        connected: true,
                        speedValid: false,
                        rssi: -64,
                        state: 8,
                        savedAddressValid: true,
                        pollCount: 12,
                        pollErrors: 0
                    })
                },
                {
                    method: 'POST',
                    match: '/api/obd/config',
                    respond: jsonResponse({ success: false }, 500)
                },
                {
                    method: 'POST',
                    match: '/api/obd/scan',
                    respond: jsonResponse({ success: true })
                },
                {
                    method: 'POST',
                    match: '/api/obd/forget',
                    respond: jsonResponse({ success: true })
                }
            ],
            jsonResponse({})
        );
        const { unmount } = render(Page);

        const input = await screen.findByLabelText(/Min RSSI \(dBm\)/i);
        expect(input).toHaveValue(-80);

        await fireEvent.input(input, { target: { value: '-55' } });
        await fireEvent.change(input);

        await screen.findByText('Failed to save OBD setting.');
        await waitFor(() => expect(input).toHaveValue(-80));

        unmount();
    });

    it('re-fetches OBD config and status after a rejected save request', async () => {
        let obdConfig = { enabled: false, minRssi: -80 };
        let obdStatus = { enabled: false, connected: false, pollCount: 0, pollErrors: 0, state: 0 };
        const fetchMock = installFetchMock(
            [
                {
                    method: 'GET',
                    match: '/api/status',
                    respond: jsonResponse({ maintenanceBoot: false, maintenanceBootUptimeMs: 0 })
                },
                {
                    method: 'GET',
                    match: '/api/obd/config',
                    respond: () => jsonResponse(obdConfig)
                },
                {
                    method: 'GET',
                    match: '/api/obd/status',
                    respond: () => jsonResponse(obdStatus)
                },
                {
                    method: 'POST',
                    match: '/api/obd/config',
                    respond: async ({ init }) => {
                        const body = JSON.parse(init.body);
                        obdConfig = { ...obdConfig, ...body };
                        obdStatus = {
                            enabled: !!obdConfig.enabled,
                            connected: !!obdConfig.enabled,
                            speedValid: false,
                            rssi: -61,
                            state: obdConfig.enabled ? 8 : 0,
                            savedAddressValid: !!obdConfig.enabled,
                            pollCount: obdConfig.enabled ? 18 : 0,
                            pollErrors: 0
                        };
                        return Promise.reject(new Error('connection lost after write'));
                    }
                },
                {
                    method: 'POST',
                    match: '/api/obd/scan',
                    respond: jsonResponse({ success: true })
                },
                {
                    method: 'POST',
                    match: '/api/obd/forget',
                    respond: jsonResponse({ success: true })
                }
            ],
            jsonResponse({})
        );
        const { unmount } = render(Page);

        const toggle = await screen.findByRole('checkbox', { name: /enable obd/i });
        expect(toggle).not.toBeChecked();

        await fireEvent.click(toggle);

        await screen.findByText('Connection error.');
        await screen.findByText('Polling');
        await waitFor(() => expect(toggle).toBeChecked());
        expect(countCalls(fetchMock, '/api/obd/config')).toBeGreaterThanOrEqual(3);
        expect(countCalls(fetchMock, '/api/obd/status')).toBeGreaterThanOrEqual(2);

        unmount();
    });
});
