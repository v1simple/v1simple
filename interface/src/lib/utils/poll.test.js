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
        vi.useRealTimers();
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
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

    it('returns the original unconsumed Response for binary backup callers', async () => {
        vi.useFakeTimers();
        const bytes = new Uint8Array([0, 255, 13, 10, 128]);
        const response = new Response(bytes, { status: 201, headers: { 'Content-Type': 'application/octet-stream' } });
        vi.stubGlobal('fetch', vi.fn(async () => response));
        const result = await fetchWithTimeout('/api/settings/backup');
        expect(result).toBe(response);
        expect(result.bodyUsed).toBe(false);
        expect(result.status).toBe(201);
        expect(result.headers.get('Content-Type')).toBe('application/octet-stream');
        expect(new Uint8Array(await (await result.blob()).arrayBuffer())).toEqual(bytes);
        expect(vi.getTimerCount()).toBe(0);
    });

    it('returns an optional response consumer result and releases its timer', async () => {
        vi.useFakeTimers();
        installFetchSpy();
        const result = await fetchWithTimeout('/api/status', {}, undefined, (response) => response.json());
        expect(result).toEqual({});
        expect(vi.getTimerCount()).toBe(0);
    });

    it('propagates consumer exceptions and releases the deadline', async () => {
        vi.useFakeTimers();
        installFetchSpy();
        await expect(fetchWithTimeout('/api/status', {}, 25, () => {
            throw new Error('Cannot consume');
        })).rejects.toThrow('Cannot consume');
        expect(vi.getTimerCount()).toBe(0);
    });

    it('keeps a custom deadline active until response consumption settles', async () => {
        vi.useFakeTimers();
        vi.stubGlobal('fetch', vi.fn(async (_url, { signal }) => new Response(new ReadableStream({
            start(controller) {
                controller.enqueue(new TextEncoder().encode('{'));
                signal.addEventListener('abort', () => controller.error(new DOMException('Aborted', 'AbortError')), { once: true });
            }
        }))));
        const result = fetchWithTimeout('/api/status', {}, 25, (response) => response.json())
            .then((value) => ({ value }), (error) => ({ error }));
        await vi.advanceTimersByTimeAsync(25);
        expect(await result).toMatchObject({ error: { name: 'AbortError' } });
        expect(vi.getTimerCount()).toBe(0);
    });
});
