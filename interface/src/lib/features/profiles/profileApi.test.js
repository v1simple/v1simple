import { afterEach, describe, expect, it, vi } from 'vitest';
import { fetchProfileCatalog, requestDetectorOperation } from './profileApi.js';
const json = (body, status = 200) => new Response(JSON.stringify(body), { status });
const accepted = { success: true, queued: true, operationId: 77, state: 'pending_normal_boot', rebooting: true, target: 'normal' };
afterEach(() => { vi.useRealTimers(); vi.unstubAllGlobals(); });

describe('profile API reads and operation admission', () => {
    it('accumulates pages with exact cursor encoding and conservative schema readiness', async () => {
        vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(json({ schemaVersion: 4, profiles: [{ name: 'A & B' }], hasMore: true, nextCursor: 'A & B' }))
            .mockResolvedValueOnce(json({ schemaVersion: 3, profiles: [{ name: 'Z' }], hasMore: false })));
        expect(await fetchProfileCatalog()).toEqual({ profiles: [{ name: 'A & B' }, { name: 'Z' }], schemaReady: false });
        expect(fetch.mock.calls.map(([url]) => url)).toEqual(['/api/v1/profiles', '/api/v1/profiles?after=A%20%26%20B&limit=10']);
    });
    it.each([null, { profiles: {} }, { profiles: [], hasMore: true }, { profiles: [], hasMore: true, nextCursor: 2 }])('rejects an invalid catalog page %j', async (body) => {
        vi.stubGlobal('fetch', vi.fn(async () => json(body)));
        await expect(fetchProfileCatalog()).rejects.toThrow('Invalid profile');
        expect(fetch).toHaveBeenCalledOnce();
    });
    it('stops a cursor cycle without publishing partial data or repeating the first page', async () => {
        vi.stubGlobal('fetch', vi.fn().mockResolvedValueOnce(json({ profiles: [], hasMore: true, nextCursor: 'A' }))
            .mockResolvedValueOnce(json({ profiles: [], hasMore: true, nextCursor: 'B' }))
            .mockResolvedValueOnce(json({ profiles: [], hasMore: true, nextCursor: 'A' })));
        await expect(fetchProfileCatalog()).rejects.toThrow('Invalid profile cursor');
        expect(fetch).toHaveBeenCalledTimes(3);
    });
    it.each([
        [202, accepted, 'accepted'],
        [409, { error: 'operation_in_progress', message: 'Already queued' }, 'rejected'],
        [202, { success: true }, 'uncertain']
    ])('keeps status %s admission distinct as %s', async (status, response, kind) => {
        vi.stubGlobal('fetch', vi.fn(async () => json(response, status)));
        const body = new URLSearchParams({ profile: 'Daily', address: 'AA:BB:CC:DD:EE:FF' });
        expect(await requestDetectorOperation('/api/v1/apply', body)).toMatchObject({ kind, status, response });
        expect(fetch).toHaveBeenCalledOnce();
        expect(fetch.mock.calls[0]).toEqual(['/api/v1/apply', expect.objectContaining({
            method: 'POST', body,
            headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8', 'X-V1Simple-Request': 'maintenance-ui' }
        })]);
    });
    it('keeps the deadline active through a stalled response body and never retries', async () => {
        vi.useFakeTimers();
        vi.stubGlobal('fetch', vi.fn(async (_, init) => ({ status: 202, ok: true, json: () => new Promise((_, reject) => {
            init.signal.addEventListener('abort', () => reject(new Error('aborted')), { once: true });
        }) })));
        const result = requestDetectorOperation('/api/autopush/push', new URLSearchParams({ slot: '1' }));
        await vi.advanceTimersByTimeAsync(5000);
        expect(await result).toEqual({ kind: 'uncertain', reason: 'connection' });
        await vi.advanceTimersByTimeAsync(5000);
        expect(fetch).toHaveBeenCalledOnce();
    });
});
