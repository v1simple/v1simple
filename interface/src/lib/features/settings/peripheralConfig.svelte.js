import { fetchJsonWithTimeout, fetchWithTimeout } from '#lib/utils/poll.js';

// Each card owns its values and validation; each instance owns their read/write lifecycle.
export function createPeripheralConfig({ url, name, apply }) {
    let phase = $state('unknown');
    let message = $state(null);

    async function read() {
        try {
            const res = await fetchJsonWithTimeout(url);
            if (!res.ok) throw new Error(`${name} config request failed with status ${res.status}`);
            apply(res.data);
            return true;
        } catch (error) {
            console.error(`Failed to load ${name} config`, error);
            message = { type: 'error', text: `Failed to load ${name} settings.` };
            return false;
        }
    }

    async function load() {
        if (phase === 'loading' || phase === 'saving') return false;
        phase = 'loading';
        message = null;
        const loaded = await read();
        phase = loaded ? 'ready' : 'unknown';
        return loaded;
    }

    async function save(fields) {
        if (phase !== 'ready') return false;
        phase = 'saving';
        message = null;
        let saved = false;
        try {
            const res = await fetchWithTimeout(url, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(fields)
            });
            saved = res.ok;
            if (!saved) message = { type: 'error', text: `Failed to save ${name} setting.` };
        } catch (_) {
            message = { type: 'error', text: 'Connection error.' };
        }
        // A failed response may follow a committed write. Reconcile once, without repeating it.
        const refreshed = await read();
        phase = refreshed ? 'ready' : 'unknown';
        return saved && refreshed;
    }

    return {
        get loaded() { return phase === 'ready' || phase === 'saving'; },
        get loading() { return phase === 'loading'; },
        get saving() { return phase === 'saving'; },
        get canEdit() { return phase === 'ready'; },
        get message() { return message; },
        load,
        save
    };
}
