import { afterEach, describe, expect, it, vi } from 'vitest';

import { refreshDeviceSettings, retainDeviceSettings } from './deviceSettings.svelte.js';

// Exercise Response.json() itself while headers have arrived but the body has not.
function pendingJson(signal) {
    let controller;
    const abort = () => controller.error(new DOMException('Aborted', 'AbortError'));
    const response = new Response(new ReadableStream({
        start(streamController) {
            controller = streamController;
            controller.enqueue(new TextEncoder().encode('{'));
            signal.addEventListener('abort', abort, { once: true });
        }
    }));
    return {
        response,
        finish(data) {
            signal.removeEventListener('abort', abort);
            controller.enqueue(new TextEncoder().encode(JSON.stringify(data).slice(1)));
            controller.close();
        },
        fail() {
            signal.removeEventListener('abort', abort);
            controller.error(new Error('Body interrupted'));
        }
    };
}

describe('device settings store', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
    });

    it('times out a stalled body and lets the next poll refresh shared settings', async () => {
        vi.useFakeTimers();
        const bodies = [];
        const signals = [];
        const fetchMock = vi.fn(async (_url, { signal }) => {
            const body = pendingJson(signal);
            bodies.push(body);
            signals.push(signal);
            return body.response;
        });
        vi.stubGlobal('fetch', fetchMock);
        const release = retainDeviceSettings();
        const first = refreshDeviceSettings();
        let settled = false;
        first.then(() => { settled = true; });
        try {
            await vi.advanceTimersByTimeAsync(4999);
            expect(settled).toBe(false);
            expect(signals[0].aborted).toBe(false);
            expect(refreshDeviceSettings()).toBe(first);
            expect(fetchMock).toHaveBeenCalledTimes(1);

            await vi.advanceTimersByTimeAsync(1);
            expect(signals[0].aborted).toBe(true);
            expect(settled).toBe(true);
            expect(await first).toBeUndefined();

            await vi.advanceTimersByTimeAsync(10000);
            expect(fetchMock).toHaveBeenCalledTimes(2);
            const next = refreshDeviceSettings();
            expect(next).not.toBe(first);
            bodies[1].finish({ ap_ssid: 'Recovered' });
            expect(await next).toEqual({ ap_ssid: 'Recovered' });
            expect(fetchMock).toHaveBeenCalledTimes(2);
        } finally {
            release();
            for (const body of bodies) body.fail();
            await first;
        }
    });

    it('accepts a delayed body before the deadline and releases its timer', async () => {
        vi.useFakeTimers();
        let body;
        let signal;
        vi.stubGlobal('fetch', vi.fn(async (_url, options) => {
            signal = options.signal;
            body = pendingJson(signal);
            return body.response;
        }));
        const release = retainDeviceSettings();
        const pending = refreshDeviceSettings();
        try {
            await vi.advanceTimersByTimeAsync(400);
            body.finish({ ap_ssid: 'Ready' });
            expect(await pending).toEqual({ ap_ssid: 'Ready' });
        } finally {
            release();
            body.fail();
        }
        await vi.advanceTimersByTimeAsync(5000);
        expect(signal.aborted).toBe(false);
        expect(vi.getTimerCount()).toBe(0);
    });
});
