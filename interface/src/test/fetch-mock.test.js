import { afterEach, describe, expect, it, vi } from 'vitest';

import { apiFixtureMatchers, installFixtureFetchMock } from './fetch-mock.js';

const originalFetch = global.fetch;

describe('firmware-captured fetch fixtures', () => {
    afterEach(() => {
        global.fetch = originalFetch;
        vi.restoreAllMocks();
    });

    it('matches exact paths and returns the captured status payload', async () => {
        installFixtureFetchMock('wifi_status_default');

        const response = await fetch('/api/wifi/status?cacheBust=1');

        expect(response.status).toBe(200);
        expect(response.headers.get('content-type')).toBe('application/json');
        expect(await response.json()).toEqual({
            enabled: true,
            savedSSID: '',
            scanRunning: false,
            state: 'disconnected'
        });
    });

    it('advances scan responses in capture order and repeats the terminal state', async () => {
        installFixtureFetchMock('wifi_scan_success');

        const started = await fetch(
            new Request('http://v1simple.test/api/wifi/scan', { method: 'POST' })
        );
        const scanning = await fetch('/api/wifi/scan');
        const complete = await fetch('/api/wifi/scan');
        const repeated = await fetch('/api/wifi/scan');

        expect(await started.json()).toEqual({ networks: [], scanning: true });
        expect(await scanning.json()).toEqual({ networks: [], scanning: true });
        expect(await complete.json()).toEqual({
            networks: [{ rssi: -42, secure: true, ssid: 'BenchAP' }],
            scanning: false
        });
        expect(await repeated.json()).toEqual({
            networks: [{ rssi: -42, secure: true, ssid: 'BenchAP' }],
            scanning: false
        });
    });

    it('preserves the transactional enable failure and status-refetch sequence', async () => {
        installFixtureFetchMock('wifi_enable_failure');

        const before = await fetch('/api/wifi/status');
        const failed = await fetch('/api/wifi/enable', { method: 'POST' });
        const after = await fetch('/api/wifi/status');

        expect(failed.status).toBe(500);
        expect(await failed.json()).toEqual({
            message: 'Failed to start connection',
            success: false
        });
        expect(await before.json()).toMatchObject({ enabled: false });
        expect(await after.json()).toMatchObject({ enabled: false });
    });

    it('serves every captured remaining OBD route with its real maintenance response', async () => {
        installFixtureFetchMock('obd_maintenance_routes');

        const devices = await fetch('/api/obd/devices');
        const status = await fetch('/api/obd/status');
        const renamed = await fetch('/api/obd/devices/name', { method: 'POST' });
        const scan = await fetch('/api/obd/scan', { method: 'POST' });
        const forgotten = await fetch('/api/obd/forget', { method: 'POST' });

        expect((await devices.json()).devices[0]).toMatchObject({
            address: 'A4:C1:38:00:11:22',
            name: 'Truck Adapter',
            connected: false
        });
        expect(status.status).toBe(409);
        expect(await status.json()).toMatchObject({ error: 'maintenance_mode' });
        expect(await renamed.json()).toEqual({ success: true });
        expect(scan.status).toBe(409);
        expect(await scan.json()).toMatchObject({ error: 'maintenance_mode' });
        expect(await forgotten.json()).toEqual({ success: true });
    });

    it('rejects unknown scenarios before installing a mock', () => {
        expect(() => apiFixtureMatchers('not-captured')).toThrow(
            'Unknown WiFi API fixture scenario: not-captured'
        );
    });

    it('preserves captured non-JSON bodies without JSON quoting', async () => {
        installFixtureFetchMock('diagnostics_routes');

        const response = await fetch('/api/diagnostics/log?path=%2Fperf%2Fperf_boot_7.csv');

        expect(response.headers.get('content-type')).toBe('text/csv');
        expect(await response.text()).toBe('header\nrow\n');
    });

    it('fails closed when a fixture scenario does not cover the request', async () => {
        installFixtureFetchMock('wifi_status_default');

        await expect(fetch('/api/not-captured')).rejects.toThrow(
            'Unexpected fixture request: GET /api/not-captured'
        );
    });

    it('combines disjoint fixture scenarios with the same strict fallback', async () => {
        installFixtureFetchMock(['frontend_core_routes', 'v1_device_routes']);

        expect((await fetch('/api/status')).status).toBe(200);
        expect((await fetch('/api/v1/devices')).status).toBe(200);
        await expect(fetch('/api/not-captured')).rejects.toThrow('Unexpected fixture request');
    });
});
