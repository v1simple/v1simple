// Each page owns its values and validation. Each instance owns one immediate
// write followed by authoritative readback, including ambiguous write results.
export function createImmediateConfig({ read, write, validateAndApply, name }) {
    let phase = $state('unknown');
    let message = $state(null);
    let publicationEpoch = 0;

    async function readAndApply(fresh, epoch) {
        try {
            const data = await read({ fresh });
            if (epoch !== publicationEpoch) return false;
            validateAndApply(data);
            return true;
        } catch (error) {
            if (epoch !== publicationEpoch) return false;
            console.error(`Failed to load ${name} settings`, error);
            message = { type: 'error', text: `Failed to load ${name} settings.` };
            return false;
        }
    }

    async function load({ fresh = true } = {}) {
        if (phase === 'loading' || phase === 'saving' || phase === 'retired') return false;
        const epoch = publicationEpoch;
        phase = 'loading';
        message = null;
        const loaded = await readAndApply(fresh, epoch);
        if (epoch !== publicationEpoch) return false;
        phase = loaded ? 'ready' : 'unknown';
        return loaded;
    }

    async function save(fields, successText) {
        if (phase !== 'ready') return false;
        const submitted = { ...fields };
        const epoch = publicationEpoch;
        phase = 'saving';
        message = null;
        let saved = false;
        try {
            saved = await write(submitted);
            if (!saved && epoch === publicationEpoch) {
                message = { type: 'error', text: `Failed to save ${name} setting.` };
            }
        } catch (_) {
            if (epoch === publicationEpoch) message = { type: 'error', text: 'Connection error.' };
        }
        // Complete every admitted readback even after navigation. A shared
        // reader retains its own consumer lifetime; retired pages cannot publish.
        const refreshed = await readAndApply(true, epoch);
        if (epoch !== publicationEpoch) return false;
        phase = refreshed ? 'ready' : 'unknown';
        if (saved && refreshed && successText) message = { type: 'success', text: successText };
        return saved && refreshed;
    }

    function dispose() {
        publicationEpoch += 1;
        phase = 'retired';
        message = null;
    }

    return {
        get loaded() { return phase === 'ready' || phase === 'saving'; },
        get loading() { return phase === 'loading'; },
        get saving() { return phase === 'saving'; },
        get canEdit() { return phase === 'ready'; },
        get message() { return message; },
        load,
        save,
        dispose
    };
}
