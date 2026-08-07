import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { installFixtureFetchMock } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function countCalls(fetchMock, url) {
    return fetchMock.mock.calls.filter(([requestUrl]) => requestUrl === url).length;
}

function installDefaultFetch(overrides = []) {
    return installFixtureFetchMock('gps_settings_and_status', overrides);
}

describe('gps route page', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
    });

    it('loads GPS settings without touching OBD APIs', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('GPS module and UTC logging settings for normal operation.');
        await screen.findByText('Enable GPS');

        expect(countCalls(fetchMock, '/api/gps/config')).toBe(1);
        expect(countCalls(fetchMock, '/api/gps/status')).toBe(0);
        expect(fetchMock.mock.calls.some(([url]) => String(url).startsWith('/api/obd'))).toBe(
            false
        );

        unmount();
    });

    it('documents next-normal-boot behavior without promising live GPS status', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText(/GPS starts after exiting maintenance/);
        expect(screen.queryByText('Live status')).not.toBeInTheDocument();
        expect(screen.queryByText('Stable')).not.toBeInTheDocument();
        expect(screen.getByRole('checkbox', { name: /enable gps/i })).toBeEnabled();
        expect(screen.getByLabelText(/Baud rate/i)).toBeEnabled();
        expect(countCalls(fetchMock, '/api/gps/status')).toBe(0);

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
