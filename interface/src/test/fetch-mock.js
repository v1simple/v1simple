import { vi } from 'vitest';

function normalizeRequest(input, init = {}) {
	const url = typeof input === 'string' ? input : input?.url || '';
	const method = (init?.method || 'GET').toUpperCase();
	return { url, method, input, init };
}

export function jsonResponse(body, status = 200, headers = {}) {
	return new Response(JSON.stringify(body), {
		status,
		headers: {
			'Content-Type': 'application/json',
			...headers
		}
	});
}

export function textResponse(body, status = 200, headers = {}) {
	return new Response(String(body), {
		status,
		headers
	});
}

/**
 * Install a deterministic fetch mock.
 *
 * Each matcher is:
 * { method?: 'GET'|'POST'|..., match: string|RegExp|((url)=>boolean), respond: Response|(({url,method,input,init})=>Response|Promise<Response>) }
 */
export function installFetchMock(matchers = [], fallback = jsonResponse({})) {
	const toResponse = (value) => (value instanceof Response ? value.clone() : value);

	const fetchMock = vi.fn(async (input, init = {}) => {
		const req = normalizeRequest(input, init);
		const hit = matchers.find((entry) => {
			if (entry.method && entry.method.toUpperCase() !== req.method) {
				return false;
			}
			if (typeof entry.match === 'string') {
				return req.url.startsWith(entry.match);
			}
			if (entry.match instanceof RegExp) {
				return entry.match.test(req.url);
			}
			if (typeof entry.match === 'function') {
				return !!entry.match(req.url);
			}
			return false;
		});

		if (!hit) {
			const fallbackResponse = typeof fallback === 'function' ? await fallback(req) : fallback;
			return toResponse(fallbackResponse);
		}

		const hitResponse = typeof hit.respond === 'function' ? await hit.respond(req) : hit.respond;
		return toResponse(hitResponse);
	});

	global.fetch = fetchMock;
	return fetchMock;
}
