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
                match: '/api/device/settings',
                respond: jsonResponse({
                    alpEnabled: true,
                    alpSdLogEnabled: false,
                    alpAlertPersistSec: 2,
                    alpDisableV1LaserOnPush: true
                })
            },
            {
                method: 'POST',
                match: '/api/device/settings',
                respond: jsonResponse({ success: true })
            }
        ],
        jsonResponse({})
    );
}

describe('alp route page', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
    });

    it('loads ALP settings without polling live alert status', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('ALP');
        expect(await screen.findByRole('checkbox', { name: /enable alp listener/i })).toBeChecked();
        expect(
            screen.getByRole('checkbox', { name: /disable v1 laser alerts on profile push/i })
        ).toBeChecked();
        expect(
            screen.getByText(/Listener enable and SD logging apply on next normal boot/)
        ).toBeInTheDocument();
        expect(screen.queryByText('Live ALP Status')).not.toBeInTheDocument();
        expect(countCalls(fetchMock, '/api/alp/status')).toBe(0);

        unmount();
    });

    it('saves the V1 laser profile-push policy toggle', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        const toggle = await screen.findByRole('checkbox', {
            name: /disable v1 laser alerts on profile push/i
        });
        await fireEvent.click(toggle);

        await screen.findByText('ALP setting saved.');
        await waitFor(() => {
            expect(
                fetchMock.mock.calls.some(
                    ([url, init]) =>
                        url === '/api/device/settings' &&
                        init?.method === 'POST' &&
                        init?.body instanceof FormData &&
                        init.body.get('alpDisableV1LaserOnPush') === 'false'
                )
            ).toBe(true);
        });

        unmount();
    });

    it('reconciles ALP settings after a failed save', async () => {
        let settings = {
            alpEnabled: false,
            alpSdLogEnabled: false,
            alpAlertPersistSec: 0,
            alpDisableV1LaserOnPush: true
        };
        installFetchMock(
            [
                {
                    method: 'GET',
                    match: '/api/device/settings',
                    respond: () => jsonResponse(settings)
                },
                {
                    method: 'POST',
                    match: '/api/device/settings',
                    respond: () => {
                        settings = { ...settings, alpDisableV1LaserOnPush: true };
                        return jsonResponse({ error: 'nope' }, 500);
                    }
                }
            ],
            jsonResponse({})
        );
        const { unmount } = render(Page);

        const toggle = await screen.findByRole('checkbox', {
            name: /disable v1 laser alerts on profile push/i
        });
        expect(toggle).toBeChecked();

        await fireEvent.click(toggle);

        await screen.findByText('Failed to save ALP setting.');
        await waitFor(() => expect(toggle).toBeChecked());

        unmount();
    });
});
