import { afterEach, describe, expect, it, vi } from 'vitest';
import {
    MAINTENANCE_API_WRITE_HEADER,
    MAINTENANCE_API_WRITE_HEADER_VALUE,
    fetchWithTimeout
} from './poll.js';

function installFetchSpy() {
    const fetchSpy = vi.fn(async () => new Response('{}', { status: 200 }));
    global.fetch = fetchSpy;
    return fetchSpy;
}

describe('fetchWithTimeout', () => {
    afterEach(() => {
        vi.restoreAllMocks();
    });

    it('adds the maintenance write header to mutating API requests', async () => {
        const fetchSpy = installFetchSpy();

        await fetchWithTimeout('/api/wifi/networks', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: '{}'
        });

        const init = fetchSpy.mock.calls[0][1];
        expect(init.headers).toMatchObject({
            'Content-Type': 'application/json',
            [MAINTENANCE_API_WRITE_HEADER]: MAINTENANCE_API_WRITE_HEADER_VALUE
        });
    });

    it('leaves read API requests and non-API writes unmodified', async () => {
        const fetchSpy = installFetchSpy();

        await fetchWithTimeout('/api/status');
        await fetchWithTimeout('/submit', { method: 'POST' });

        expect(fetchSpy.mock.calls[0][1].headers).toBeUndefined();
        expect(fetchSpy.mock.calls[1][1].headers).toBeUndefined();
    });
});
