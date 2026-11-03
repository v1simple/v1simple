import { fetchJsonWithTimeout, fetchWithTimeout, URLENCODED_FORM_CONTENT_TYPE } from '#lib/utils/poll.js';
import { validDetectorOperationStart } from './detectorOperation.js';

// Return a complete catalog; each page owns whether this read is still current.
export async function fetchProfileCatalog() {
    const profiles = [];
    const seen = new Set();
    let cursor = '';
    let schemaReady = true;
    while (true) {
        const url = cursor
            ? `/api/v1/profiles?after=${encodeURIComponent(cursor)}&limit=10`
            : '/api/v1/profiles';
        const result = await fetchJsonWithTimeout(url);
        if (!result.ok || !Array.isArray(result.data?.profiles)) throw new Error('Invalid profile page');
        const data = result.data;
        profiles.push(...data.profiles);
        schemaReady = schemaReady && data.schemaVersion === 4;
        if (!data.hasMore) return { profiles, schemaReady };
        if (typeof data.nextCursor !== 'string' || !data.nextCursor || seen.has(data.nextCursor)) {
            throw new Error('Invalid profile cursor');
        }
        cursor = data.nextCursor;
        seen.add(cursor);
    }
}

// One admission request only. A missing response never licenses another POST.
export async function requestDetectorOperation(path, body) {
    try {
        const result = await fetchWithTimeout(path, {
            method: 'POST', headers: { 'Content-Type': URLENCODED_FORM_CONTENT_TYPE }, body
        }, undefined, async (response) => ({
            status: response.status, ok: response.ok, response: await response.json()
        }));
        if (!result.ok) return { kind: 'rejected', ...result };
        if (!validDetectorOperationStart(result.response)) {
            return { kind: 'uncertain', reason: 'invalid_identity', ...result };
        }
        return { kind: 'accepted', ...result };
    } catch {
        return { kind: 'uncertain', reason: 'connection' };
    }
}
