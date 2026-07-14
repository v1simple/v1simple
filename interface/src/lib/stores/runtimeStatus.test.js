import { waitFor } from '@testing-library/svelte';
import { get } from 'svelte/store';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import { isMaintenance, retainRuntimeStatus, runtimeStatus } from './runtimeStatus.svelte.js';

describe('runtime status store', () => {
    afterEach(() => {
        vi.restoreAllMocks();
    });

    it('defaults to non-maintenance runtime status', () => {
        expect(get(runtimeStatus).maintenanceBoot).toBe(false);
        expect(get(runtimeStatus).maintenanceBootUptimeMs).toBe(0);
        expect(get(isMaintenance)).toBe(false);
    });

    it('surfaces maintenance boot fields from /api/status', async () => {
        installFetchMock(
            [
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
                            uptime: 12,
                            heap_free: 32768,
                            hostname: 'v1simple',
                            firmware_version: 'test'
                        },
                        maintenanceBoot: true,
                        maintenanceBootUptimeMs: 4321,
                        v1_connected: false,
                        alert: null
                    })
                }
            ],
            jsonResponse({})
        );

        const release = retainRuntimeStatus({ needsStatus: true });

        await waitFor(() => {
            expect(get(runtimeStatus).maintenanceBoot).toBe(true);
            expect(get(runtimeStatus).maintenanceBootUptimeMs).toBe(4321);
            expect(get(isMaintenance)).toBe(true);
        });

        release();
    });
});
