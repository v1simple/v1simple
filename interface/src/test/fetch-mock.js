import { vi } from 'vitest';

import wifiApiFixtures from './fixtures/wifi-api.json';

function normalizeRequest(input, init = {}) {
    const url = typeof input === 'string' ? input : input?.url || '';
    const method = (init?.method || input?.method || 'GET').toUpperCase();
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

function requestPath(url) {
    return new URL(String(url), 'http://v1simple.test').pathname;
}

function capturedResponse(capture) {
    const body = /(?:application\/json|\+json)(?:\s*;|$)/i.test(capture.contentType)
        ? JSON.stringify(capture.body)
        : String(capture.body);
    return new Response(body, {
        status: capture.status,
        headers: { 'Content-Type': capture.contentType }
    });
}

/**
 * Build exact method/path matchers backed by a real firmware response capture.
 * Ordered responses advance once per matching request and then repeat the final
 * response so terminal polling states remain stable.
 */
export function apiFixtureMatchers(scenarioName) {
    const scenario = wifiApiFixtures.scenarios?.[scenarioName];
    if (!scenario) {
        throw new Error(`Unknown WiFi API fixture scenario: ${scenarioName}`);
    }

    return Object.entries(scenario).map(([routeKey, responses]) => {
        const separator = routeKey.indexOf(' ');
        const method = routeKey.slice(0, separator);
        const path = routeKey.slice(separator + 1);
        let responseIndex = 0;

        return {
            method,
            path,
            match: (url) => requestPath(url) === path,
            respond: () => {
                const capture = responses[Math.min(responseIndex, responses.length - 1)];
                responseIndex += 1;
                return capturedResponse(capture);
            }
        };
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
            const fallbackResponse =
                typeof fallback === 'function' ? await fallback(req) : fallback;
            return toResponse(fallbackResponse);
        }

        const hitResponse =
            typeof hit.respond === 'function' ? await hit.respond(req) : hit.respond;
        return toResponse(hitResponse);
    });

    global.fetch = fetchMock;
    return fetchMock;
}

function unexpectedFixtureRequest({ method, url }) {
    throw new Error(`Unexpected fixture request: ${method} ${requestPath(url)}`);
}

export function installFixtureFetchMock(
    scenarioNameOrNames,
    matchers = [],
    fallback = unexpectedFixtureRequest
) {
    const scenarioNames = Array.isArray(scenarioNameOrNames)
        ? scenarioNameOrNames
        : [scenarioNameOrNames];
    if (scenarioNames.length === 0) {
        throw new Error('At least one WiFi API fixture scenario is required');
    }
    const fixtureMatchers = scenarioNames.flatMap((name) => apiFixtureMatchers(name));
    const routeKeys = fixtureMatchers.map(({ method, path }) => `${method} ${path}`);
    if (new Set(routeKeys).size !== routeKeys.length) {
        throw new Error('Combined WiFi API fixture scenarios contain overlapping routes');
    }
    return installFetchMock([...matchers, ...fixtureMatchers], fallback);
}
