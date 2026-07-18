import { fireEvent, render, screen } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

describe('diagnostics and logs route page', () => {
    afterEach(() => {
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
    });

    it('groups maintenance logs, formats sizes, and exposes power-off evidence', async () => {
        const fetchMock = installFetchMock([
            {
                method: 'GET',
                match: '/api/diagnostics/logs',
                respond: jsonResponse({
                    files: [
                        { path: '/poweroff.log', category: 'power', sizeBytes: 900 },
                        {
                            path: '/perf/perf_boot_24.csv',
                            category: 'performance',
                            sizeBytes: 58120
                        },
                        { path: '/alp/alp_24.csv', category: 'alp', sizeBytes: 2048 }
                    ],
                    truncated: false,
                    lastPoweroffEvidence: 'result=DEEPSLEEP reason=external_power',
                    maxDownloadBytes: 16 * 1024 * 1024
                })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('perf_boot_24.csv');
        expect(screen.getByRole('heading', { name: 'Power' })).toBeInTheDocument();
        expect(screen.getByRole('heading', { name: 'Performance' })).toBeInTheDocument();
        expect(screen.getByRole('heading', { name: 'ALP' })).toBeInTheDocument();
        expect(screen.getByText('900 B')).toBeInTheDocument();
        expect(screen.getByText('57 KiB')).toBeInTheDocument();
        expect(screen.getByText('2.0 KiB')).toBeInTheDocument();
        expect(screen.getByText(/result=DEEPSLEEP reason=external_power/)).toBeInTheDocument();
        expect(screen.getByText(/limited to 16 MiB/i)).toBeInTheDocument();

        await fireEvent.click(screen.getByRole('button', { name: 'Refresh' }));
        expect(
            fetchMock.mock.calls.filter(([url]) => url === '/api/diagnostics/logs')
        ).toHaveLength(2);

        unmount();
    });

    it('shows a useful empty state', async () => {
        installFetchMock([
            {
                method: 'GET',
                match: '/api/diagnostics/logs',
                respond: jsonResponse({
                    files: [],
                    truncated: false,
                    maxDownloadBytes: 16 * 1024 * 1024
                })
            }
        ]);
        const { unmount } = render(Page);

        expect(
            await screen.findByText('No diagnostic log files were found on the SD card.')
        ).toBeInTheDocument();
        expect(screen.getByText(/return to maintenance mode and refresh/i)).toBeInTheDocument();

        unmount();
    });

    it('shows the backend error without pretending the list is empty', async () => {
        installFetchMock([
            {
                method: 'GET',
                match: '/api/diagnostics/logs',
                respond: jsonResponse({ message: 'Log index failed' }, 500)
            }
        ]);
        const { unmount } = render(Page);

        expect(await screen.findByText('Log index failed')).toBeInTheDocument();
        expect(screen.queryByText(/no diagnostic log files/i)).not.toBeInTheDocument();

        unmount();
    });

    it('shows the maintenance-only state returned by the device', async () => {
        installFetchMock([
            {
                method: 'GET',
                match: '/api/diagnostics/logs',
                respond: jsonResponse({ error: 'maintenance_mode_required' }, 409)
            }
        ]);
        const { unmount } = render(Page);

        expect(
            await screen.findByText(
                'Diagnostics and log downloads are available only in maintenance mode.'
            )
        ).toBeInTheDocument();

        unmount();
    });

    it('distinguishes temporary storage contention from a missing SD card', async () => {
        installFetchMock([
            {
                method: 'GET',
                match: '/api/diagnostics/logs',
                respond: jsonResponse({ error: 'storage_busy' }, 503)
            }
        ]);
        const { unmount } = render(Page);

        expect(
            await screen.findByText('Diagnostic storage is busy. Wait a moment, then refresh.')
        ).toBeInTheDocument();
        expect(screen.queryByText(/SD card is unavailable/i)).not.toBeInTheDocument();

        unmount();
    });

    it('percent-encodes the complete diagnostic path in download links', async () => {
        installFetchMock([
            {
                method: 'GET',
                match: '/api/diagnostics/logs',
                respond: jsonResponse({
                    files: [
                        {
                            path: '/perf/perf_boot_24-88cdc436.csv',
                            category: 'performance',
                            sizeBytes: 100
                        }
                    ],
                    truncated: false,
                    maxDownloadBytes: 1000
                })
            }
        ]);
        const { unmount } = render(Page);

        const link = await screen.findByRole('link', { name: 'Download' });
        expect(link).toHaveAttribute(
            'href',
            '/api/diagnostics/log?path=%2Fperf%2Fperf_boot_24-88cdc436.csv'
        );
        expect(link).toHaveAttribute('download', 'perf_boot_24-88cdc436.csv');

        unmount();
    });

    it('disables downloads that exceed the device size limit', async () => {
        installFetchMock([
            {
                method: 'GET',
                match: '/api/diagnostics/logs',
                respond: jsonResponse({
                    files: [
                        { path: '/perf/allowed.csv', category: 'performance', sizeBytes: 1000 },
                        { path: '/perf/oversized.csv', category: 'performance', sizeBytes: 1001 }
                    ],
                    truncated: false,
                    maxDownloadBytes: 1000
                })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('oversized.csv');
        expect(screen.getByRole('link', { name: 'Download' })).toHaveAttribute(
            'download',
            'allowed.csv'
        );
        const oversizedAction = screen.getByRole('button', { name: 'Too large' });
        expect(oversizedAction).toBeDisabled();
        expect(oversizedAction).toHaveAttribute(
            'title',
            'Exceeds the 1000 B device download limit'
        );

        unmount();
    });
});
