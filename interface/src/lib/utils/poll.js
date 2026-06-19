/**
 * Shared fetch / polling helpers for the V1 web interface.
 *
 * - fetchWithTimeout: wraps fetch() with an AbortController deadline.
 * - createPoll: setInterval wrapper with in-flight dedup and stop().
 */

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
	const controller = new AbortController();
	const id = setTimeout(() => controller.abort(), timeoutMs);
	return fetch(url, { ...opts, signal: controller.signal }).finally(() =>
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
