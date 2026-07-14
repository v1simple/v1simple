import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function countCalls(fetchMock, url) {
    return fetchMock.mock.calls.filter(([requestUrl]) => requestUrl === url).length;
}

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
                match: '/api/gps/config',
                respond: jsonResponse({
                    gpsEnabled: true,
                    gpsBaud: 9600,
                    gpsEnablePinActiveHigh: true,
                    gpsLogUtcToPerf: true,
                    gpsLogUtcToAlp: true
                })
            },
            {
                method: 'GET',
                match: '/api/gps/status',
                respond: jsonResponse({
                    moduleDetected: true,
                    hasFix: true,
                    stableHasFix: true,
                    satellites: 7,
                    hdop: 1.2,
                    fixAgeMs: 800,
                    lastSentenceAgeMs: 300,
                    parserActive: true,
                    detectionTimedOut: false,
                    counters: {
                        sentencesParsed: 42,
                        parseFailures: 1,
                        checksumFailures: 0,
                        bytesRead: 4096
                    }
                })
            },
            { method: 'POST', match: '/api/gps/config', respond: jsonResponse({ success: true }) }
        ],
        jsonResponse({})
    );
}

describe('gps route page', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
    });

    it('loads GPS settings without touching OBD APIs', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('GPS module settings, UTC logging, and fix status.');
        await screen.findByText('Enable GPS');

        expect(countCalls(fetchMock, '/api/gps/config')).toBe(1);
        expect(fetchMock.mock.calls.some(([url]) => String(url).startsWith('/api/obd'))).toBe(
            false
        );

        unmount();
    });

    it('hides live GPS fix status in maintenance mode', async () => {
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/status',
                respond: jsonResponse({ maintenanceBoot: true, maintenanceBootUptimeMs: 18000 })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText(/Live GPS fix data is not running in maintenance mode/);
        expect(screen.queryByText('Live status')).not.toBeInTheDocument();
        expect(screen.queryByText('Stable')).not.toBeInTheDocument();
        expect(screen.getByRole('checkbox', { name: /enable gps/i })).toBeEnabled();
        expect(screen.getByLabelText(/Baud rate/i)).toBeEnabled();

        unmount();
    });

    it('saves GPS settings through the GPS config endpoint', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        const toggle = await screen.findByRole('checkbox', { name: /enable gps/i });
        expect(toggle).toBeChecked();

        await fireEvent.click(toggle);

        await waitFor(() => {
            expect(
                fetchMock.mock.calls.some(([url, init]) => {
                    if (url !== '/api/gps/config' || init?.method !== 'POST') return false;
                    return JSON.parse(init.body).gpsEnabled === false;
                })
            ).toBe(true);
        });
        expect(fetchMock.mock.calls.some(([url]) => String(url).startsWith('/api/obd'))).toBe(
            false
        );

        unmount();
    });
});
