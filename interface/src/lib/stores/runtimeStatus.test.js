import { waitFor } from '@testing-library/svelte';
import { get } from 'svelte/store';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import {
    isMaintenance, retainRuntimeStatus, runtimeStatus, runtimeStatusError, runtimeStatusLoading
} from './runtimeStatus.svelte.js';

// Headers resolve immediately; the body stays pending and obeys fetch's signal.
function pendingJson(signal, status = 200) {
    let controller;
    const abort = () => controller.error(new DOMException('Aborted', 'AbortError'));
    const response = new Response(new ReadableStream({
        start(streamController) {
            controller = streamController;
            controller.enqueue(new TextEncoder().encode('{"maintenanceBoot":'));
            signal.addEventListener('abort', abort, { once: true });
        }
    }), { status });
    return {
        response,
        finish(value) {
            signal.removeEventListener('abort', abort);
            controller.enqueue(new TextEncoder().encode(`${value}}`));
            controller.close();
        },
        fail() {
            signal.removeEventListener('abort', abort);
            controller.error(new Error('Body interrupted'));
        }
    };
}

describe('runtime status store', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
    });

    it('defaults to non-maintenance runtime status', () => {
        expect(get(runtimeStatus).maintenanceBoot).toBe(false);
        expect(get(runtimeStatus).maintenanceBootUptimeMs).toBe(0);
        expect(get(runtimeStatus).maintenanceBootTimeoutMs).toBe(600000);
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

    it('times out a stalled JSON body and recovers on the next poll without overlap', async () => {
        vi.useFakeTimers();
        let signal;
        let body;
        const fetchMock = vi.fn(async (_url, options) => {
            if (fetchMock.mock.calls.length === 1) {
                signal = options.signal;
                body = pendingJson(signal);
                return body.response;
            }
            return new Response('{"maintenanceBoot":true}');
        });
        vi.stubGlobal('fetch', fetchMock);
        const release = retainRuntimeStatus({ needsStatus: true });
        try {
            await vi.advanceTimersByTimeAsync(4999);
            expect(get(runtimeStatusLoading)).toBe(true);
            expect(get(runtimeStatusError)).toBeNull();
            expect(fetchMock).toHaveBeenCalledTimes(1);
            await vi.advanceTimersByTimeAsync(1);
            expect(signal.aborted).toBe(true);
            expect(get(runtimeStatusLoading)).toBe(false);
            expect(get(runtimeStatusError)).toBe('Connection lost');
            await vi.advanceTimersByTimeAsync(1000);
            expect(fetchMock).toHaveBeenCalledTimes(2);
            expect(get(runtimeStatus).maintenanceBoot).toBe(true);
            expect(get(runtimeStatusError)).toBeNull();
        } finally {
            release();
            body.fail();
        }
    });

    it('accepts a healthy delayed body before the deadline and clears its timer', async () => {
        vi.useFakeTimers();
        let body;
        let signal;
        vi.stubGlobal('fetch', vi.fn(async (_url, options) => {
            signal = options.signal;
            body = pendingJson(signal);
            return body.response;
        }));
        const release = retainRuntimeStatus({ needsStatus: true });
        try {
            await vi.advanceTimersByTimeAsync(400);
            expect(get(runtimeStatusLoading)).toBe(true);
            body.finish(true);
            await vi.advanceTimersByTimeAsync(0);
            expect(get(runtimeStatusLoading)).toBe(false);
            expect(get(runtimeStatusError)).toBeNull();
            expect(get(isMaintenance)).toBe(true);
        } finally {
            release();
        }
        await vi.advanceTimersByTimeAsync(5000);
        expect(signal.aborted).toBe(false);
        expect(vi.getTimerCount()).toBe(0);
    });

    it('still times out before headers arrive', async () => {
        vi.useFakeTimers();
        vi.stubGlobal('fetch', vi.fn((_url, { signal }) => new Promise((_resolve, reject) => {
            signal.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')), { once: true });
        })));
        const release = retainRuntimeStatus({ needsStatus: true });
        try {
            await vi.advanceTimersByTimeAsync(5000);
            expect(get(runtimeStatusError)).toBe('Connection lost');
            expect(get(runtimeStatusLoading)).toBe(false);
        } finally {
            release();
        }
    });

    it('reports non-OK headers without waiting for their body', async () => {
        vi.useFakeTimers();
        let body;
        vi.stubGlobal('fetch', vi.fn(async (_url, { signal }) => {
            body = pendingJson(signal, 503);
            return body.response;
        }));
        const release = retainRuntimeStatus({ needsStatus: true });
        try {
            await vi.advanceTimersByTimeAsync(0);
            expect(get(runtimeStatusError)).toBe('API error');
            expect(get(runtimeStatusLoading)).toBe(false);
            expect(body.response.bodyUsed).toBe(false);
        } finally {
            body.finish(false);
            release();
        }
    });

    it('reports malformed successful JSON as a connection failure', async () => {
        vi.useFakeTimers();
        vi.stubGlobal('fetch', vi.fn(async () => new Response('{')));
        const release = retainRuntimeStatus({ needsStatus: true });
        try {
            await vi.advanceTimersByTimeAsync(0);
            expect(get(runtimeStatusError)).toBe('Connection lost');
            expect(get(runtimeStatusLoading)).toBe(false);
        } finally {
            release();
        }
    });

    it.each(['complete', 'reject'])('ignores an old body that %ss after release and re-retain', async (outcome) => {
        vi.useFakeTimers();
        const bodies = [];
        const fetchMock = vi.fn(async (_url, { signal }) => {
            const body = pendingJson(signal);
            bodies.push(body);
            return body.response;
        });
        vi.stubGlobal('fetch', fetchMock);
        const releaseOld = retainRuntimeStatus({ needsStatus: true });
        await vi.advanceTimersByTimeAsync(0);
        releaseOld();
        const releaseNew = retainRuntimeStatus({ needsStatus: true });
        try {
            await vi.advanceTimersByTimeAsync(0);
            if (outcome === 'complete') bodies[0].finish(true);
            else bodies[0].fail();
            await vi.advanceTimersByTimeAsync(0);
            expect(get(isMaintenance)).toBe(false);
            expect(get(runtimeStatusError)).toBeNull();
            expect(get(runtimeStatusLoading)).toBe(true);
            await vi.advanceTimersByTimeAsync(3000);
            expect(fetchMock).toHaveBeenCalledTimes(2);
            bodies[1].finish(true);
            await vi.advanceTimersByTimeAsync(0);
            expect(get(isMaintenance)).toBe(true);
            expect(get(runtimeStatusLoading)).toBe(false);
        } finally {
            releaseNew();
            for (const body of bodies) body.fail();
        }
    });
});
