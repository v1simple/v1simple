/**
 * Shared fetch / polling helpers for the V1 web interface.
 *
 * - fetchWithTimeout: wraps fetch() with an AbortController deadline.
 * - createPoll: setInterval wrapper with in-flight dedup and stop().
 */

export const MAINTENANCE_API_WRITE_HEADER = 'X-V1Simple-Request';
export const MAINTENANCE_API_WRITE_HEADER_VALUE = 'maintenance-ui';
export const URLENCODED_FORM_CONTENT_TYPE = 'application/x-www-form-urlencoded;charset=UTF-8';

function isMaintenanceApiWrite(url, opts) {
    const method = String(opts?.method || 'GET').toUpperCase();
    return (
        method !== 'GET' && method !== 'HEAD' && typeof url === 'string' && url.startsWith('/api/')
    );
}

function withRequestHeader(opts, name, value) {
    if (typeof Headers !== 'undefined' && opts?.headers instanceof Headers) {
        const headers = new Headers(opts.headers);
        headers.set(name, value);
        return { ...opts, headers };
    }
    if (Array.isArray(opts?.headers)) {
        const headers = new Headers(opts.headers);
        headers.set(name, value);
        return { ...opts, headers };
    }

    const headers = { ...(opts?.headers || {}) };
    const existing = Object.keys(headers).find(
        (candidate) => candidate.toLowerCase() === name.toLowerCase()
    );
    if (existing) delete headers[existing];
    headers[name] = value;
    return {
        ...opts,
        headers
    };
}

function encodeStringFormData(formData) {
    const encoded = new URLSearchParams();
    for (const [name, value] of formData.entries()) {
        if (typeof value !== 'string') {
            throw new TypeError('File and Blob form values are not supported by maintenance APIs');
        }
        encoded.append(name, value);
    }
    return encoded;
}

function normalizeRequestOptions(url, opts) {
    let normalized = opts;
    if (typeof FormData !== 'undefined' && opts?.body instanceof FormData) {
        normalized = withRequestHeader(
            { ...opts, body: encodeStringFormData(opts.body) },
            'Content-Type',
            URLENCODED_FORM_CONTENT_TYPE
        );
    }
    if (isMaintenanceApiWrite(url, normalized) && normalized?.body == null) {
        normalized = withRequestHeader(
            { ...normalized, body: new URLSearchParams() },
            'Content-Type',
            URLENCODED_FORM_CONTENT_TYPE
        );
    }
    if (isMaintenanceApiWrite(url, normalized)) {
        normalized = withRequestHeader(
            normalized,
            MAINTENANCE_API_WRITE_HEADER,
            MAINTENANCE_API_WRITE_HEADER_VALUE
        );
    }
    return normalized;
}

/**
 * Wrap fetch() with an AbortController timeout so hung requests
 * on a flaky ESP32 AP link don't pile up forever.
 *
 * @param {string} url
 * @param {RequestInit} [opts]
 * @param {number} [timeoutMs=5000]
 * @returns {Promise<Response>}
 */
export function fetchWithTimeout(url, opts = {}, timeoutMs = 5000) {
    const requestOpts = normalizeRequestOptions(url, opts);
    const controller = new AbortController();
    const id = setTimeout(() => controller.abort(), timeoutMs);
    return fetch(url, { ...requestOpts, signal: controller.signal }).finally(() =>
        clearTimeout(id)
    );
}

/**
 * Create a poll handle with automatic in-flight dedup.
 * If the previous tick's async callback is still running, the next
 * tick is silently skipped — no overlapping requests.
 *
 * @param {() => Promise<void>} fn  Async function called each tick
 * @param {number} intervalMs       Poll interval in milliseconds
 * @returns {{ start(): void, stop(): void }}
 */
export function createPoll(fn, intervalMs) {
    let id = null;
    let inFlight = false;

    async function tick() {
        if (inFlight) return;
        inFlight = true;
        try {
            await fn();
        } finally {
            inFlight = false;
        }
    }

    return {
        start() {
            if (id === null) id = setInterval(tick, intervalMs);
        },
        stop() {
            if (id !== null) {
                clearInterval(id);
                id = null;
            }
        }
    };
}
