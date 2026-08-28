import { afterEach, describe, expect, it, vi } from 'vitest';
import {
    MAINTENANCE_API_WRITE_HEADER,
    MAINTENANCE_API_WRITE_HEADER_VALUE,
    URLENCODED_FORM_CONTENT_TYPE,
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

    it('converts string-only FormData to ordered URL-encoded fields', async () => {
        const fetchSpy = installFetchSpy();
        const formData = new FormData();
        formData.append('slot', '1');
        formData.append('mode', 'first');
        formData.append('mode', 'second value');

        await fetchWithTimeout('/api/device/settings', {
            method: 'POST',
            body: formData
        });

        const init = fetchSpy.mock.calls[0][1];
        expect(init.body).toBeInstanceOf(URLSearchParams);
        expect(Array.from(init.body.entries())).toEqual([
            ['slot', '1'],
            ['mode', 'first'],
            ['mode', 'second value']
        ]);
        expect(init.headers['Content-Type']).toBe(URLENCODED_FORM_CONTENT_TYPE);
        expect(JSON.stringify(init.headers)).not.toContain('multipart/form-data');
    });

    it('rejects File or Blob FormData values before fetch', async () => {
        const fetchSpy = installFetchSpy();
        const formData = new FormData();
        formData.append('backup', new Blob(['fixture']), 'backup.json');

        expect(() =>
            fetchWithTimeout('/api/settings/restore', {
                method: 'POST',
                body: formData
            })
        ).toThrow(/File and Blob form values/);
        expect(fetchSpy).not.toHaveBeenCalled();
    });

    it('gives bodyless maintenance POSTs an explicit empty URL-encoded body', async () => {
        const fetchSpy = installFetchSpy();

        await fetchWithTimeout('/api/wifi/scan', { method: 'POST' });

        const init = fetchSpy.mock.calls[0][1];
        expect(init.body).toBeInstanceOf(URLSearchParams);
        expect(init.body.toString()).toBe('');
        expect(init.headers['Content-Type']).toBe(URLENCODED_FORM_CONTENT_TYPE);
        expect(init).toHaveProperty('body');
    });
});
