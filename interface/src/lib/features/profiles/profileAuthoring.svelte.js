import { fetchWithTimeout } from '#lib/utils/poll.js';
import {
    createDefaultProfileSettings,
    createProfileDetectorConfiguration,
    cloneDetectorConfiguration,
    customFrequencyBand,
    detectorConfigurationFromSnapshot,
    fromApiDetectorConfiguration,
    fromApiSettings,
    toApiDetectorConfiguration,
    toApiSettings
} from '#lib/features/profiles/profileSettingsAdapter.js';
import {
    createDefaultInTheBoxSettings,
    cloneInTheBoxSettings,
    fromApiInTheBoxSettings,
    toApiInTheBoxSettings,
    inTheBoxSettingsError
} from '#lib/features/profiles/inTheBoxSettings.js';
const PROFILE_DESCRIPTION_MAX_BYTES = 4096;
function descriptionForSave(value) {
    const description = (value || '').trim();
    return new TextEncoder().encode(description).length <= PROFILE_DESCRIPTION_MAX_BYTES
        ? { description }
        : { error: 'Description must be 4096 UTF-8 bytes or fewer' };
}
function validateProfileName(raw, profiles) {
    const canonical = raw.trim();
    if (!canonical) return { error: 'Profile name required' };
    if (new TextEncoder().encode(canonical).length > 64) {
        return { error: 'Profile name exceeds 64 UTF-8 bytes' };
    }
    if (canonical.startsWith('.') || canonical.startsWith('_')) {
        return { error: 'Profile name cannot begin with dot or underscore' };
    }
    if (canonical.includes('/') || canonical.includes('\\') || canonical.includes('..')) {
        return { error: 'Profile name cannot contain path characters' };
    }
    const hasControlCharacter = [...canonical].some((character) => {
        const code = character.charCodeAt(0);
        return code < 32 || code === 127;
    });
    if (hasControlCharacter || /[:*?"<>|]/.test(canonical)) {
        return { error: 'Profile name contains an invalid character' };
    }
    const asciiFold = (value) => value.replace(/[A-Z]/g, (character) => character.toLowerCase());
    const collision = profiles.find(
        (profile) => asciiFold(profile.name) === asciiFold(canonical)
    );
    if (collision) return { error: 'Profile name collides with an existing profile' };
    return { canonical };
}

function detectorConfigurationsEqual(left, right) {
    return JSON.stringify(toApiDetectorConfiguration(left)) ===
        JSON.stringify(toApiDetectorConfiguration(right));
}

function inTheBoxConfigurationsEqual(left, right) {
    return JSON.stringify(toApiInTheBoxSettings(left)) === JSON.stringify(toApiInTheBoxSettings(right));
}

export function customFrequencyError(settings, detector) {
    if (detector?.customFrequencyPolicy !== 'value') {
        return detector?.userSettings !== 'unchanged' && settings?.customFreqs
            ? 'To enable custom-frequency detection, choose profile ranges and add at least one K or Ka range.'
            : null;
    }
    const definitions = detector.customFrequencyDefinitions;
    if (!Array.isArray(definitions) || definitions.length === 0) {
        return 'Add at least one K or Ka range, or choose to keep the detector’s current ranges.';
    }
    if (definitions.length > 64) {
        return 'A profile can author at most 64 custom-frequency ranges.';
    }
    const validEdges = definitions.every((definition) =>
        Number.isInteger(Number(definition.lowerMHz)) &&
        Number.isInteger(Number(definition.upperMHz)) &&
        Number(definition.lowerMHz) > 0 &&
        Number(definition.upperMHz) > Number(definition.lowerMHz) &&
        Number(definition.upperMHz) <= 65535
    );
    if (!validEdges) return 'Every custom-frequency range needs valid lower and upper MHz edges.';
    if (definitions.some((definition) => customFrequencyBand(definition) === null)) {
        return 'Every range must fit inside the published Gen2 K or Ka sweep section.';
    }
    return null;
}

async function postProfileSave(name, prepared, { createOnly = false, parseErrorJson = false } = {}) {
    const payload = {
        name,
        ...(createOnly ? { createOnly: true } : {}),
        description: prepared.description,
        schemaVersion: 4,
        detector: toApiDetectorConfiguration(prepared.savedDetector),
        inTheBox: toApiInTheBoxSettings(prepared.savedInTheBox),
        settings: toApiSettings(prepared.savedSettings)
    };
    const result = await fetchWithTimeout('/api/v1/profile', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
    }, undefined, async (response) => {
        if (response.ok) return { ok: true, error: null };
        const body = await response.text();
        // Preserve each save path's existing error presentation.
        if (parseErrorJson) {
            try {
                return { ok: false, error: JSON.parse(body).error || body };
            } catch {
                // Non-JSON errors are displayed as received.
            }
        }
        return { ok: false, error: body };
    });
    return { ...result, payload };
}

function copyNameSuggestion(name, profiles) {
    const suffix = ' copy';
    let base = name;
    while (new TextEncoder().encode(base + suffix).length > 64) {
        base = Array.from(base).slice(0, -1).join('');
    }
    let candidate = base + suffix;
    let number = 2;
    while (profiles.some((profile) => profile.name.toLowerCase() === candidate.toLowerCase())) {
        const numberedSuffix = ` copy ${number++}`;
        let trimmed = name;
        while (new TextEncoder().encode(trimmed + numberedSuffix).length > 64) {
            trimmed = Array.from(trimmed).slice(0, -1).join('');
        }
        candidate = trimmed + numberedSuffix;
    }
    return candidate;
}


function cloneSettings(settings) {
    return { ...settings, ...(settings.baseBytes ? { baseBytes: [...settings.baseBytes] } : {}) };
}

function decodedProfile(data) {
    return {
        ...data,
        settings: fromApiSettings(data.settings || {}),
        detector: fromApiDetectorConfiguration(data.detector || {}),
        inTheBox: fromApiInTheBoxSettings(data.inTheBox)
    };
}

// A single mounted page owns these sessions. Catalog/load/snapshot generations
// remain with the page because they expire independently of an open editor.
export function createProfileAuthoring({ onSaved, onMessage }) {
    let acceptedProfile = $state(null);
    let draft = $state(null);
    let namingDialog = $state(null);
    let submitted = $state.raw(null);
    let nextIdentity = 0;
    let disposed = false;

    function publish(message) {
        if (!disposed) onMessage(message);
    }

    function begin(source, kind, suggestedName = '', values = source) {
        acceptedProfile = source;
        draft = {
            id: ++nextIdentity, source: acceptedProfile, kind, suggestedName,
            settings: cloneSettings(values.settings),
            detector: cloneDetectorConfiguration(values.detector),
            inTheBox: cloneInTheBoxSettings(values.inTheBox),
            description: values.description || ''
        };
        namingDialog = null;
    }

    function defaults() {
        return {
            settings: createDefaultProfileSettings(),
            detector: createProfileDetectorConfiguration(),
            inTheBox: createDefaultInTheBoxSettings()
        };
    }

    function openNamingDialog() {
        namingDialog = {
            id: ++nextIdentity,
            name: draft?.suggestedName || acceptedProfile?.name || '',
            description: draft?.description || acceptedProfile?.description || ''
        };
    }

    function cancel() {
        draft = null;
    }

    function reeditPending(name) {
        if (submitted?.intent !== 'edit' || submitted.name !== name) return false;
        // Keep the submitted source until ACK. A concurrent load could still
        // return its older contents, even after this save has been acknowledged.
        begin(submitted.source, 'edit', '', submitted.profile);
        return true;
    }

    async function save(intent, profiles, blocked) {
        if (submitted || blocked || disposed) return;
        const creating = intent === 'create';
        const dialog = namingDialog;
        const editor = draft;
        const source = editor?.source || acceptedProfile;
        let name = source?.name;
        if (creating) {
            if (!dialog) return;
            const validated = validateProfileName(dialog.name, profiles);
            if (validated.error) return publish({ type: 'error', text: validated.error });
            name = validated.canonical;
            if (editor?.kind === 'copy' && profiles.some((profile) => profile.name.toLowerCase() === name.toLowerCase())) {
                return publish({ type: 'error', text: 'Choose a new name for the copied profile' });
            }
        } else if (!editor || !source?.name) {
            return publish({ type: 'error', text: 'No profile loaded to save' });
        }
        const description = descriptionForSave(creating ? dialog.description : editor.description);
        const settings = editor?.settings || source?.settings;
        const detector = editor?.detector || source?.detector;
        const inTheBox = editor?.inTheBox || source?.inTheBox;
        const error = description.error || (!settings ? 'No settings to save' : null) ||
            customFrequencyError(settings, detector) || inTheBoxSettingsError(inTheBox);
        if (error) return publish({ type: 'error', text: error });

        // Both requests detach all transmitted contents before awaiting. Edit's
        // historical shallow field comparison is separate from that snapshot;
        // creation compares canonical wire values (including raw base bytes).
        const shallowSettings = { ...settings };
        const prepared = {
            description: description.description,
            savedSettings: cloneSettings(settings),
            savedDetector: cloneDetectorConfiguration(detector),
            savedInTheBox: cloneInTheBoxSettings(inTheBox)
        };
        const submission = {
            name, intent, source, editorId: editor?.id, dialogId: dialog?.id,
            descriptionAtStart: editor?.description,
            profile: {
                ...source, draft: false, name, description: prepared.description,
                settings: prepared.savedSettings, detector: prepared.savedDetector,
                inTheBox: prepared.savedInTheBox
            }
        };
        submitted = submission;
        if (!creating) publish({ type: 'info', text: `Saving ${name}...` });
        try {
            const response = await postProfileSave(name, prepared, {
                createOnly: creating, parseErrorJson: creating
            });
            if (disposed) return;
            if (!response.ok) {
                publish({ type: 'error', text: `Failed to save: ${response.error}` });
                return;
            }
            const { payload } = response;
            onSaved(name, payload.description, true);
            const sameSource = acceptedProfile === source;
            const sameEditor = sameSource && draft?.id === submission.editorId;
            // Edit acknowledgement advances the saved source even if its editor
            // was canceled or reset. A newly selected source owns its own state.
            if (sameSource && (!creating || sameEditor)) {
                acceptedProfile = submission.profile;
                if (draft?.source === source) draft.source = acceptedProfile;
            }
            if (sameEditor) {
                const dialogUnchanged = !creating || (namingDialog?.id === submission.dialogId &&
                    namingDialog.name.trim() === name && namingDialog.description.trim() === payload.description);
                const settingsUnchanged = !draft || (creating
                    ? JSON.stringify(toApiSettings(draft.settings)) === JSON.stringify(payload.settings)
                    : Object.entries(shallowSettings).every(([key, value]) => draft.settings[key] === value));
                const draftUnchanged = !draft || (
                    (creating ? draft.description === submission.descriptionAtStart
                        : draft.description.trim() === payload.description) && settingsUnchanged &&
                    detectorConfigurationsEqual(draft.detector, prepared.savedDetector) &&
                    inTheBoxConfigurationsEqual(draft.inTheBox, prepared.savedInTheBox));
                if (draftUnchanged && dialogUnchanged) cancel();
                if (creating && dialogUnchanged) namingDialog = null;
            }
            publish({ type: 'success', text: `Profile "${name}" saved` });
        } catch {
            publish({ type: 'error', text: 'Connection error' });
        } finally {
            if (submitted === submission) submitted = null;
        }
    }

    return {
        get acceptedProfile() { return acceptedProfile; },
        get draft() { return draft; },
        get namingDialog() { return namingDialog; },
        get savingProfile() { return submitted?.name || null; },
        beginNew() { begin({ available: true, draft: true, name: '', description: '', ...defaults() }, 'new'); },
        beginCaptured(snapshot) {
            begin({ available: true, draft: true, captured: true, name: '', description: '',
                settings: fromApiSettings(snapshot.settings),
                detector: detectorConfigurationFromSnapshot(snapshot),
                inTheBox: createDefaultInTheBoxSettings() }, 'captured');
        },
        beginCopy(data, profiles, sourceName = data.name) {
            begin({ ...decodedProfile(data), draft: true, name: '' }, 'copy', copyNameSuggestion(sourceName, profiles));
            openNamingDialog();
        },
        beginEdit(data) { begin(decodedProfile(data), 'edit'); },
        reedit() {
            if (!acceptedProfile?.settings) return;
            // Reopening a pending Edit starts from what was sent, without
            // declaring it accepted or letting later edits mutate the request.
            if (!reeditPending(acceptedProfile.name)) begin(acceptedProfile, 'edit');
        },
        reeditPending,
        resetEditableDomains() {
            if (draft) draft = { ...draft, id: ++nextIdentity, ...defaults() };
        },
        cancel,
        openNamingDialog,
        closeNamingDialog() { namingDialog = null; },
        saveNewName(profiles, blocked = false) { return save('create', profiles, blocked); },
        saveEdit(blocked = false) { return save('edit', [], blocked); },
        dispose() { disposed = true; }
    };
}
