<script>
    import { onMount } from 'svelte';
    import { fetchWithTimeout } from '$lib/utils/poll';
    import PageHeader from '$lib/components/PageHeader.svelte';
    import StatusAlert from '$lib/components/StatusAlert.svelte';
    import ProfileSaveDialog from '$lib/features/profiles/ProfileSaveDialog.svelte';
    import ProfileSavedListCard from '$lib/features/profiles/ProfileSavedListCard.svelte';
    import ProfileSettingsPanel from '$lib/features/profiles/ProfileSettingsPanel.svelte';
    import {
        createDefaultProfileSettings,
        createDefaultDetectorConfiguration,
        cloneDetectorConfiguration,
        detectorConfigurationFromSnapshot,
        fromApiDetectorConfiguration,
        fromApiSettings,
        toApiDetectorConfiguration,
        toApiSettings
    } from '$lib/features/profiles/profileSettingsAdapter';
    import {
        DETECTOR_OPERATION_POLL_WINDOW_MS,
        DETECTOR_OPERATION_STATES,
        forgetDetectorOperationId,
        readDetectorOperationId,
        rememberDetectorOperationId,
        validDetectorOperationStart,
        validDetectorOperationStatus
    } from '$lib/features/profiles/detectorOperation';

    let profiles = $state([]);
    let currentProfile = $state(null);
    let loading = $state(true);
    let message = $state(null);
    let showSaveDialog = $state(false);
    let saveName = $state('');
    let saveDescription = $state('');
    let savingProfile = $state(null);
    let editingSettings = $state(false);
    let editedSettings = $state(null);
    let editedDetector = $state(null);
    const PROFILE_DESCRIPTION_MAX_BYTES = 4096;

    function descriptionForSave(value) {
        const description = (value || '').trim();
        return new TextEncoder().encode(description).length <= PROFILE_DESCRIPTION_MAX_BYTES
            ? { description }
            : { error: 'Description must be 4096 UTF-8 bytes or fewer' };
    }
    let editDescription = $state('');
    let capturedSnapshot = $state(null);
    let snapshotLoading = $state(true);
    let profileSchemaReady = $state(true);
    let operationStatus = $state(null);
    let operationBusy = $state(false);
    let factoryConfirmation = $state('');
    const PROFILE_LOAD_ERROR_TEXT = 'Failed to load profiles';
    let operationPollTimer = null;
    let operationPollDeadline = 0;
    let operationPollFailures = 0;
    let operationPollEpoch = 0;

    function validateProfileName(raw) {
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
            (profile) => asciiFold(profile.name) === asciiFold(canonical) && profile.name !== canonical
        );
        if (collision) return { error: 'Profile name collides with an existing profile' };
        return { canonical };
    }

    function clearMessageText(text) {
        if (message?.text === text) {
            message = null;
        }
    }

    function recordSavedProfile(name, description, displayOn) {
        const saved = { name, description, displayOn: displayOn ?? true };
        const found = profiles.some((profile) => profile.name === name);
        profiles = found
            ? profiles.map((profile) => (profile.name === name ? saved : profile))
            : [...profiles, saved];
    }

    function detectorConfigurationsEqual(left, right) {
        return JSON.stringify(toApiDetectorConfiguration(left)) ===
            JSON.stringify(toApiDetectorConfiguration(right));
    }

    onMount(() => {
        void fetchProfiles();
        void fetchCapturedSnapshot();
        operationPollDeadline = Date.now() + DETECTOR_OPERATION_POLL_WINDOW_MS;
        void resumeDetectorOperation();
        return () => {
            operationPollEpoch += 1;
            if (operationPollTimer) clearTimeout(operationPollTimer);
        };
    });

    function storedOperationId() {
        return readDetectorOperationId(window.localStorage);
    }

    function forgetOperationId() {
        forgetDetectorOperationId(window.localStorage);
    }

    function scheduleOperationPoll() {
        if (Date.now() >= operationPollDeadline) {
            message = {
                type: 'warning',
                text: 'Detector operation status polling paused after five minutes. Reload to resume the exact operation.'
            };
            return;
        }
        if (operationPollTimer) clearTimeout(operationPollTimer);
        const delay = Math.min(5000, 750 * (2 ** Math.min(operationPollFailures, 3)));
        operationPollTimer = setTimeout(() => void resumeDetectorOperation(), delay);
    }

    function validOperationStatus(status, operationId) {
        return validDetectorOperationStatus(status, operationId);
    }

    function rememberOperationId(operationId) {
        rememberDetectorOperationId(window.localStorage, operationId);
    }

    async function resumeDetectorOperation() {
        const operationId = storedOperationId();
        if (!Number.isSafeInteger(operationId) || operationId <= 0) return;
        const pollEpoch = operationPollEpoch;
        try {
            const res = await fetchWithTimeout(
                `/api/autopush/status?operationId=${encodeURIComponent(operationId)}`,
                {}, undefined, async (response) => ({
                    status: response.status,
                    ok: response.ok,
                    body: await response.json()
                })
            );
            if (pollEpoch !== operationPollEpoch || storedOperationId() !== operationId) return;
            if (res.status === 409) {
                operationStatus = null;
                forgetOperationId();
                message = {
                    type: 'error',
                    text: `Saved operation ${operationId} does not match the detector's current operation. No status was reused.`
                };
                return;
            }
            if (!res.ok) {
                operationPollFailures += 1;
                scheduleOperationPoll();
                return;
            }
            const status = res.body;
            if (!validOperationStatus(status, operationId)) {
                operationStatus = null;
                message = { type: 'error', text: 'Detector operation returned malformed or mismatched status.' };
                return;
            }
            const priorState = operationStatus?.operationId === operationId ? operationStatus.state : null;
            const priorRank = DETECTOR_OPERATION_STATES.indexOf(priorState);
            const nextRank = DETECTOR_OPERATION_STATES.indexOf(status.state);
            if (priorRank >= 0 && !status.terminal && nextRank < priorRank) {
                message = { type: 'error', text: 'Detector operation status regressed; stale status was rejected.' };
                return;
            }
            operationStatus = status;
            operationPollFailures = 0;
            if (status.terminal) forgetOperationId();
            else scheduleOperationPoll();
        } catch {
            if (pollEpoch !== operationPollEpoch || storedOperationId() !== operationId) return;
            // Normal mode intentionally takes Wi-Fi down while the operation runs.
            operationPollFailures += 1;
            scheduleOperationPoll();
        }
    }

    async function startDetectorOperation(path, body, label) {
        if (operationBusy) return;
        operationBusy = true;
        message = { type: 'info', text: `${label} is being queued...` };
        try {
            const res = await fetchWithTimeout(path, {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
                body
            }, undefined, async (response) => ({
                status: response.status,
                ok: response.ok,
                body: await response.json()
            }));
            const response = res.body;
            if (!res.ok) {
                message = {
                    type: 'error',
                    text: response.error || `${label} could not be queued (HTTP ${res.status})`
                };
                return;
            }
            if (!validDetectorOperationStart(response)) {
                message = { type: 'error', text: `${label} returned an invalid operation identity.` };
                return;
            }
            operationPollEpoch += 1;
            if (operationPollTimer) clearTimeout(operationPollTimer);
            rememberOperationId(response.operationId);
            operationStatus = {
                operationId: response.operationId,
                state: response.state,
                result: 'in_progress',
                terminal: false,
                components: {}
            };
            operationPollDeadline = Date.now() + DETECTOR_OPERATION_POLL_WINDOW_MS;
            operationPollFailures = 0;
            scheduleOperationPoll();
            message = {
                type: 'info',
                text: `${label} queued as operation ${response.operationId}. V1Simple will restart, verify the exact captured detector in normal mode, and return here.`
            };
        } catch {
            message = { type: 'error', text: `${label} connection was interrupted before acceptance was confirmed.` };
        } finally {
            operationBusy = false;
        }
    }

    function applySavedProfile() {
        const savedName = currentProfile?.name;
        const stillSaved = savedName && profiles.some((profile) => profile.name === savedName);
        const address = capturedSnapshot?.address;
        if (!stillSaved || !address) {
            message = { type: 'error', text: 'Load a saved profile and a captured detector first.' };
            return;
        }
        const body = new URLSearchParams();
        body.set('profile', savedName);
        body.set('address', address);
        void startDetectorOperation('/api/v1/apply', body, `Apply saved profile "${savedName}"`);
    }

    function factoryResetDetector() {
        const address = capturedSnapshot?.address;
        if (!address || factoryConfirmation !== 'RESET V1') {
            message = { type: 'error', text: 'Type RESET V1 to confirm the detector factory reset.' };
            return;
        }
        const body = new URLSearchParams();
        body.set('address', address);
        body.set('confirm', factoryConfirmation);
        void startDetectorOperation('/api/v1/factory-reset', body, 'Detector factory reset');
    }

    async function fetchCapturedSnapshot() {
        try {
            const loaded = await fetchWithTimeout(
                '/api/v1/snapshot', {}, undefined, async (response) => ({
                    ok: response.ok,
                    body: response.ok ? await response.json() : null
                })
            );
            const snapshot = loaded.ok ? loaded.body : null;
            capturedSnapshot = snapshot?.available === true &&
                /^[0-9A-F]{2}(?::[0-9A-F]{2}){5}$/.test(snapshot.address || '')
                ? snapshot
                : { available: false };
        } catch (e) {
            capturedSnapshot = { available: false };
        } finally {
            snapshotLoading = false;
        }
    }

    function formatFirmware(value) {
        if (!Number.isInteger(value) || value <= 0) return 'Unavailable';
        const digits = String(value).padStart(5, '0');
        return `${digits[0]}.${digits.slice(1)}`;
    }

    function formatBytes(values) {
        if (!Array.isArray(values) || values.length !== 6) return 'Unavailable';
        return values.map((value) => Number(value).toString(16).padStart(2, '0').toUpperCase()).join(' ');
    }

    function startDraftFromCapturedSettings() {
        if (!capturedSnapshot?.settings || !capturedSnapshot?.observations?.userBytes?.available) return;
        const settings = fromApiSettings(capturedSnapshot.settings);
        currentProfile = {
            available: true,
            draft: true,
            captured: true,
            name: '',
            description: '',
            detector: detectorConfigurationFromSnapshot(capturedSnapshot),
            settings
        };
        editedSettings = { ...settings, baseBytes: settings.baseBytes ? [...settings.baseBytes] : undefined };
        editedDetector = cloneDetectorConfiguration(currentProfile.detector);
        editDescription = '';
        saveName = '';
        saveDescription = '';
        editingSettings = true;
        message = {
            type: 'info',
            text: 'Draft started from the last observed V1 user bytes. No detector changes were made.'
        };
    }

    async function fetchProfiles() {
        try {
            const loadedProfiles = [];
            let cursor = '';
            let schemaReady = true;
            while (true) {
                const url = cursor
                    ? `/api/v1/profiles?after=${encodeURIComponent(cursor)}&limit=10`
                    : '/api/v1/profiles';
                const res = await fetchWithTimeout(url);
                if (!res.ok) {
                    message = { type: 'error', text: PROFILE_LOAD_ERROR_TEXT };
                    return false;
                }
                const data = await res.json();
                if (!Array.isArray(data.profiles)) throw new Error('Invalid profile page');
                loadedProfiles.push(...data.profiles);
                schemaReady = schemaReady && data.schemaVersion === 3;
                if (!data.hasMore) break;
                if (typeof data.nextCursor !== 'string' || !data.nextCursor || data.nextCursor === cursor) {
                    throw new Error('Invalid profile cursor');
                }
                cursor = data.nextCursor;
            }
            profiles = loadedProfiles;
            profileSchemaReady = schemaReady;
            clearMessageText(PROFILE_LOAD_ERROR_TEXT);
            return true;
        } catch (e) {
            message = { type: 'error', text: PROFILE_LOAD_ERROR_TEXT };
            return false;
        } finally {
            loading = false;
        }
    }

    async function saveCurrentProfile() {
        if (savingProfile) return;
        const validatedName = validateProfileName(saveName);
        if (validatedName.error) {
            message = { type: 'error', text: validatedName.error };
            return;
        }
        const validatedDescription = descriptionForSave(saveDescription);
        if (validatedDescription.error) {
            message = { type: 'error', text: validatedDescription.error };
            return;
        }

        const settingsToSave =
            editingSettings && editedSettings ? editedSettings : currentProfile?.settings;

        if (!settingsToSave) {
            message = { type: 'error', text: 'No settings to save' };
            return;
        }

        savingProfile = validatedName.canonical;
        try {
            const payload = {
                name: validatedName.canonical,
                description: validatedDescription.description,
                schemaVersion: 3,
                detector: toApiDetectorConfiguration(editedDetector || currentProfile?.detector),
                settings: toApiSettings(settingsToSave)
            };

            const res = await fetchWithTimeout('/api/v1/profile', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(payload)
            }, undefined, async (response) => ({
                ok: response.ok,
                error: response.ok ? null : await response.text()
            }));

            if (res.ok) {
                const canonicalName = validatedName.canonical;
                recordSavedProfile(
                    canonicalName,
                    payload.description,
                    true
                );
                saveName = canonicalName;
                message = { type: 'success', text: `Profile "${canonicalName}" saved` };
                showSaveDialog = false;
            } else {
                message = { type: 'error', text: `Failed to save: ${res.error}` };
            }
        } catch (e) {
            message = { type: 'error', text: 'Connection error' };
        } finally {
            savingProfile = null;
        }
    }

    function startEditing() {
        if (currentProfile && currentProfile.settings) {
            editedSettings = { ...currentProfile.settings };
            editedDetector = cloneDetectorConfiguration(currentProfile.detector);
            editDescription = currentProfile.description || '';
            editingSettings = true;
        }
    }

    function cancelEditing() {
        editedSettings = null;
        editedDetector = null;
        editDescription = '';
        editingSettings = false;
    }

    function openSaveDialog() {
        if (editingSettings && editDescription && !saveDescription) {
            saveDescription = editDescription;
        }
        showSaveDialog = true;
    }

    function closeSaveDialog() {
        showSaveDialog = false;
    }

    async function editProfile(name) {
        message = { type: 'info', text: `Loading ${name}...` };
        try {
            const res = await fetchWithTimeout(`/api/v1/profile?name=${encodeURIComponent(name)}`);
            if (res.ok) {
                const data = await res.json();
                currentProfile = {
                    ...data,
                    detector: fromApiDetectorConfiguration(data.detector || {}),
                    settings: fromApiSettings(data.settings || {})
                };
                editedSettings = { ...currentProfile.settings };
                editedDetector = cloneDetectorConfiguration(currentProfile.detector);
                editDescription = data.description || '';
                editingSettings = true;
                message = { type: 'info', text: `Editing ${name}` };
            } else {
                const error = await res.text();
                message = { type: 'error', text: `Failed to load: ${error}` };
            }
        } catch (e) {
            message = { type: 'error', text: 'Connection error' };
        }
    }

    function createNewProfile() {
        currentProfile = {
            available: true,
            draft: true,
            name: '',
            description: '',
            detector: createDefaultDetectorConfiguration(),
            settings: createDefaultProfileSettings()
        };
        editedSettings = createDefaultProfileSettings();
        editedDetector = createDefaultDetectorConfiguration();
        editDescription = '';
        saveName = '';
        saveDescription = '';
        editingSettings = true;
        message = {
            type: 'info',
            text: 'Creating an offline V1 profile. Save it, then assign it on the Auto-Push page.'
        };
    }

    function resetDraftToLocalDefaults() {
        if (!editingSettings) return;
        editedSettings = createDefaultProfileSettings();
        editedDetector = createDefaultDetectorConfiguration();
        message = {
            type: 'info',
            text: 'Draft reset to V1Simple profile defaults. No command was sent to the detector.'
        };
    }

    async function saveEditedProfile() {
        if (savingProfile) return;
        if (!editedSettings || !currentProfile || !currentProfile.name) {
            message = { type: 'error', text: 'No profile loaded to save' };
            return;
        }
        const validatedDescription = descriptionForSave(editDescription);
        if (validatedDescription.error) {
            message = { type: 'error', text: validatedDescription.error };
            return;
        }

        const profile = currentProfile;
        const editSession = editedSettings;
        const detectorEditSession = editedDetector;
        const savedSettings = { ...editedSettings };
        const savedDetector = cloneDetectorConfiguration(editedDetector || profile.detector);
        savingProfile = profile.name;
        message = { type: 'info', text: `Saving ${profile.name}...` };
        try {
            const payload = {
                name: profile.name,
                description: validatedDescription.description,
                schemaVersion: 3,
                detector: toApiDetectorConfiguration(savedDetector),
                settings: toApiSettings(savedSettings)
            };

            const res = await fetchWithTimeout('/api/v1/profile', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(payload)
            }, undefined, async (response) => ({
                ok: response.ok,
                error: response.ok ? null : await response.text()
            }));

            if (res.ok) {
                recordSavedProfile(payload.name, payload.description, true);
                message = { type: 'success', text: `Profile "${payload.name}" saved` };
                if (editedSettings === editSession && editedDetector === detectorEditSession) {
                    const draftUnchanged = editDescription.trim() === payload.description &&
                        Object.entries(savedSettings).every(([key, value]) => editedSettings[key] === value) &&
                        detectorConfigurationsEqual(editedDetector, savedDetector);
                    currentProfile = {
                        ...profile,
                        description: payload.description,
                        detector: savedDetector,
                        settings: savedSettings
                    };
                    if (draftUnchanged) cancelEditing();
                }
            } else {
                message = { type: 'error', text: `Failed to save: ${res.error}` };
            }
        } catch (e) {
            message = { type: 'error', text: 'Connection error' };
        } finally {
            savingProfile = null;
        }
    }

    async function deleteProfile(name) {
        if (!confirm(`Delete profile "${name}"?`)) return;

        try {
            const res = await fetchWithTimeout('/api/v1/profile/delete', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify({ name })
            });
            if (res.ok) {
                profiles = profiles.filter((profile) => profile.name !== name);
                message = { type: 'success', text: 'Profile deleted' };
            } else {
                const errorData = await res.json().catch(() => ({}));
                const error = errorData?.error || errorData?.message;
                message = {
                    type: 'error',
                    text: error ? `Failed to delete: ${error}` : 'Failed to delete'
                };
            }
        } catch (e) {
            message = { type: 'error', text: 'Failed to delete' };
        }
    }
</script>

<div class="page-stack">
    <PageHeader
        title="V1 Profiles"
        subtitle="Create, edit, and save profiles for automatic use during normal operation."
    >
        <div class="badge badge-info">Offline authoring</div>
    </PageHeader>

    <StatusAlert {message} />

    {#if operationStatus}
        <div class="surface-card" aria-label="Detector operation status">
            <div class="card-body space-y-3">
                <div class="flex flex-wrap items-center justify-between gap-2">
                    <h2 class="card-title">Detector operation {operationStatus.operationId}</h2>
                    <span class:badge-success={operationStatus.state === 'succeeded'}
                          class:badge-warning={operationStatus.state === 'partial'}
                          class:badge-error={operationStatus.state === 'failed'}
                          class="badge">
                        {operationStatus.state || 'unknown'}
                    </span>
                </div>
                <p class="copy-muted">
                    Result: {operationStatus.result || 'in_progress'} · Reason: {operationStatus.reason || 'none'}
                </p>
                {#if operationStatus.kind && operationStatus.targetAddress}
                    <p class="copy-caption">
                        {operationStatus.kind} · target {operationStatus.targetAddress} · source {operationStatus.source}
                    </p>
                {/if}
                {#if operationStatus.components}
                    <div class="grid gap-2 sm:grid-cols-2">
                        {#each Object.entries(operationStatus.components) as [name, component] (name)}
                            {#if component.requested}
                                <div class="surface-panel">
                                    <div class="copy-caption">{name}</div>
                                    <div>{component.outcome} · {component.reason}</div>
                                    <div class="copy-caption">
                                        sent {component.sent ? 'yes' : 'no'} · verified {component.verified ? 'yes' : 'no'}
                                    </div>
                                </div>
                            {/if}
                        {/each}
                    </div>
                {/if}
            </div>
        </div>
    {/if}

    {#if !loading && !profileSchemaReady}
        <StatusAlert
            message="Profile migration is pending. Creation and editing stay read-only, but existing profiles can be deleted if the older catalog is too large to migrate safely."
            fallbackType="warning"
        />
    {/if}

    <ProfileSaveDialog
        open={showSaveDialog}
        {savingProfile}
        bind:saveName
        bind:saveDescription
        oncancel={closeSaveDialog}
        onsave={saveCurrentProfile}
    />

    <div class="surface-card">
        <div class="card-body space-y-3">
            <div class="flex flex-wrap items-start justify-between gap-3">
                <div>
                    <h2 class="card-title">Last observed V1</h2>
                    <p class="copy-muted">
                        Captured during normal operation and stored for this maintenance session. This is not live state.
                    </p>
                </div>
                {#if capturedSnapshot?.available}
                    <div class="badge badge-warning">Previous boot · not live</div>
                {/if}
            </div>

            {#if snapshotLoading}
                <div class="state-loading compact"><span class="loading loading-spinner"></span></div>
            {:else if !capturedSnapshot?.available}
                <p class="state-empty">
                    No detector settings have been captured yet. Connect a V1 during normal operation, then return to maintenance.
                </p>
            {:else}
                <div class="surface-panel grid gap-2 sm:grid-cols-2">
                    <div><span class="copy-caption">Detector</span><div>{capturedSnapshot.name || capturedSnapshot.address}</div></div>
                    <div><span class="copy-caption">Firmware</span><div>{formatFirmware(capturedSnapshot.firmware?.value)}</div></div>
                    <div><span class="copy-caption">User bytes</span><div class="font-mono">{formatBytes(capturedSnapshot.observations?.userBytes?.value)}</div></div>
                    <div><span class="copy-caption">Mode</span><div>{capturedSnapshot.observations?.mode?.available ? capturedSnapshot.observations.mode.value : 'Unavailable'}</div></div>
                    <div><span class="copy-caption">Display</span><div>{capturedSnapshot.observations?.displayOn?.available ? (capturedSnapshot.observations.displayOn.value ? 'On' : 'Off') : 'Unavailable'}</div></div>
                    <div><span class="copy-caption">Bluetooth indicator</span><div>{capturedSnapshot.observations?.bluetoothIndicator?.available ? capturedSnapshot.observations.bluetoothIndicator.value : 'Unavailable'}</div></div>
                    <div>
                        <span class="copy-caption">Volume</span>
                        <div>
                            {capturedSnapshot.observations?.currentVolume?.available
                                ? `${capturedSnapshot.observations.currentVolume.main} / ${capturedSnapshot.observations.currentVolume.muted} current`
                                : 'Unavailable'}
                            {#if capturedSnapshot.observations?.savedVolume?.available}
                                · {capturedSnapshot.observations.savedVolume.main} / {capturedSnapshot.observations.savedVolume.muted} saved
                            {/if}
                        </div>
                    </div>
                    <div>
                        <span class="copy-caption">Custom sweep definitions</span>
                        <div>
                            {capturedSnapshot.observations?.customFrequencies?.definitionsAvailable
                                ? `${capturedSnapshot.observations.customFrequencies.effectiveDefinitions?.length || 0} calibrated entries`
                                : 'Unavailable'}
                        </div>
                    </div>
                </div>

                {#if capturedSnapshot.provenance?.captureTimedOut}
                    <div class="surface-alert alert-warning" role="status">
                        The connection capture timed out. Available values are preserved; missing values remain unknown.
                    </div>
                {/if}
                {#if capturedSnapshot.observations?.currentVolume?.available && capturedSnapshot.observations?.savedVolume?.available &&
                    (Number(capturedSnapshot.observations.currentVolume.main) !== Number(capturedSnapshot.observations.savedVolume.main) ||
                     Number(capturedSnapshot.observations.currentVolume.muted) !== Number(capturedSnapshot.observations.savedVolume.muted))}
                    <div class="surface-alert alert-info" role="status">
                        Current and saved volume differ. A captured draft prefills the observed current numbers, but leaves volume unchanged until you explicitly choose Temporary or Save on V1.
                    </div>
                {/if}
                {#if capturedSnapshot.capabilities?.versionKnown && capturedSnapshot.capabilities?.gen2}
                    <p class="copy-caption">
                        Firmware-qualified: {capturedSnapshot.capabilities.supportedUserByteCount} user bytes,
                        saved volume {capturedSnapshot.capabilities.savedVolume ? 'supported' : 'not supported'},
                        keep-Bluetooth-indicator-on {capturedSnapshot.capabilities.keepBluetoothLedOn ? 'supported' : 'not supported'},
                        custom sweeps {capturedSnapshot.capabilities.customSweeps ? 'supported' : 'not supported'},
                        Gatso RT4 {capturedSnapshot.capabilities.settings?.gatsoRT4 ? 'supported' : 'not supported'}.
                    </p>
                {:else if capturedSnapshot.capabilities?.versionKnown}
                    <p class="copy-caption">
                        This firmware version is outside the qualified Gen2 range; captured bytes are shown without feature claims.
                    </p>
                {:else}
                    <p class="copy-caption">Firmware capabilities are unknown; the captured bytes are shown without feature claims.</p>
                {/if}
                <div>
                    <button
                        class="btn btn-primary btn-sm"
                        disabled={!profileSchemaReady || !capturedSnapshot.observations?.userBytes?.available || !capturedSnapshot.settings}
                        onclick={startDraftFromCapturedSettings}
                    >
                        Start draft from captured settings
                    </button>
                </div>
                {#if capturedSnapshot.capabilities?.detectorFactoryResetWorkflowAvailable}
                    <div class="surface-panel space-y-2">
                        <h3 class="font-semibold">Factory reset this detector</h3>
                        <p class="copy-caption">
                            This sends the vendor factory-default command to {capturedSnapshot.address} in normal mode.
                            V1Simple will freshly recapture the detector afterward. User-byte defaults can be verified;
                            numeric factory volume and other undocumented defaults remain explicitly unverified.
                        </p>
                        <label class="field-control">
                            <span class="field-label copy-caption">Type RESET V1 to confirm</span>
                            <input class="input input-sm" bind:value={factoryConfirmation} autocomplete="off" />
                        </label>
                        <button class="btn btn-error btn-sm" type="button"
                            disabled={operationBusy || factoryConfirmation !== 'RESET V1'}
                            onclick={factoryResetDetector}>
                            Factory reset captured V1
                        </button>
                    </div>
                {/if}
            {/if}
        </div>
    </div>

    {#if profileSchemaReady && currentProfile && editedDetector}
        <div class="surface-card">
            <div class="card-body space-y-4">
                <div>
                    <h2 class="card-title">Detector apply policy</h2>
                    <p class="copy-muted">
                        These choices belong to this profile. Unchanged means no value is invented or sent.
                    </p>
                </div>
                <div class="grid gap-3 sm:grid-cols-2">
                    <label class="field-control">
                        <span class="field-label copy-caption">User settings bytes</span>
                        <select class="select select-sm" bind:value={editedDetector.userSettings}>
                            <option value="value">Apply profile settings</option>
                            <option value="unchanged">Leave unchanged</option>
                        </select>
                    </label>
                    <div class="grid grid-cols-2 gap-2">
                        <label class="field-control">
                            <span class="field-label copy-caption">Logic mode</span>
                            <select class="select select-sm" bind:value={editedDetector.modePolicy}>
                                <option value="unchanged">Leave unchanged</option>
                                <option value="value">Set mode</option>
                            </select>
                        </label>
                        <label class="field-control">
                            <span class="field-label copy-caption">Mode value</span>
                            <select class="select select-sm" bind:value={editedDetector.mode} disabled={editedDetector.modePolicy !== 'value'}>
                                <option value={1}>All Bogeys</option>
                                <option value={2}>Logic</option>
                                <option value={3}>Advanced Logic</option>
                            </select>
                        </label>
                    </div>
                    <label class="field-control">
                        <span class="field-label copy-caption">V1 display</span>
                        <select
                            class="select select-sm"
                            bind:value={editedDetector.display}
                            onchange={() => {
                                if (editedDetector.display !== 'off') editedDetector.bluetoothLed = 'unchanged';
                            }}
                        >
                            <option value="unchanged">Leave unchanged</option>
                            <option value="on">On</option>
                            <option value="off">Main display off</option>
                        </select>
                    </label>
                    <label class="field-control">
                        <span class="field-label copy-caption">Bluetooth indicator while display is off</span>
                        <select class="select select-sm" bind:value={editedDetector.bluetoothLed} disabled={editedDetector.display !== 'off'}>
                            <option value="unchanged">Leave unchanged</option>
                            <option value="off">Off</option>
                            <option value="on">Keep indicator active (on or blinking)</option>
                        </select>
                    </label>
                    <label class="field-control">
                        <span class="field-label copy-caption">Volume policy</span>
                        <select
                            class="select select-sm"
                            bind:value={editedDetector.volumePolicy}
                            onchange={() => {
                                if (editedDetector.volumePolicy !== 'temporary') editedDetector.volumeDisconnect = 'restore_saved';
                            }}
                        >
                            <option value="unchanged">Leave unchanged</option>
                            <option value="temporary">Temporary</option>
                            <option value="saved">Save on V1</option>
                        </select>
                    </label>
                    <label class="field-control">
                        <span class="field-label copy-caption">Main volume (0–9)</span>
                        <input class="input input-sm" type="number" min="0" max="9" bind:value={editedDetector.mainVolume} disabled={editedDetector.volumePolicy === 'unchanged'} />
                    </label>
                    <label class="field-control">
                        <span class="field-label copy-caption">Muted volume (0–9)</span>
                        <input class="input input-sm" type="number" min="0" max="9" bind:value={editedDetector.mutedVolume} disabled={editedDetector.volumePolicy === 'unchanged'} />
                    </label>
                    <label class="field-control">
                        <span class="field-label copy-caption">Volume feedback</span>
                        <select class="select select-sm" bind:value={editedDetector.volumeFeedback} disabled={editedDetector.volumePolicy === 'unchanged'}>
                            <option value="none">None</option>
                            <option value="changed_only">Only when changed</option>
                            <option value="always">Always</option>
                        </select>
                    </label>
                    <label class="field-control">
                        <span class="field-label copy-caption">After Bluetooth disconnect</span>
                        <select
                            class="select select-sm"
                            bind:value={editedDetector.volumeDisconnect}
                            disabled={editedDetector.volumePolicy !== 'temporary'}
                        >
                            <option value="restore_saved">Restore saved volume</option>
                            <option value="keep_current">Keep temporary volume</option>
                        </select>
                    </label>
                </div>
                <div class="surface-panel space-y-3">
                    <label class="field-control">
                        <span class="field-label copy-caption">Custom sweep definitions</span>
                        <select class="select select-sm" bind:value={editedDetector.customFrequencyPolicy}>
                            <option value="unchanged">Leave detector definitions unchanged</option>
                            <option value="value">Apply this complete definition set</option>
                        </select>
                    </label>
                    {#if editedDetector.customFrequencyPolicy === 'value'}
                        <p class="copy-caption">
                            Requested edges are saved in the profile. The V1 calibrates them to its closest supported frequencies; Apply verifies and reports that calibrated readback.
                        </p>
                        <div class="max-h-72 overflow-auto">
                            <table class="table table-xs">
                                <thead><tr><th>Index</th><th>Lower MHz</th><th>Upper MHz</th></tr></thead>
                                <tbody>
                                    {#each editedDetector.customFrequencyDefinitions as definition (definition.index)}
                                        <tr>
                                            <td>{definition.index}</td>
                                            <td><input aria-label={`Custom ${definition.index} lower MHz`} class="input input-xs w-28" type="number" min="0" max="65535" bind:value={definition.lowerMHz} /></td>
                                            <td><input aria-label={`Custom ${definition.index} upper MHz`} class="input input-xs w-28" type="number" min="0" max="65535" bind:value={definition.upperMHz} /></td>
                                        </tr>
                                    {/each}
                                </tbody>
                            </table>
                        </div>
                        {#if editedDetector.customFrequencyDefinitions.length === 0}
                            <div class="surface-alert alert-warning" role="status">
                                A complete captured definition set is required. Start from the last observed V1 or load a profile that already owns one.
                            </div>
                        {/if}
                    {/if}
                </div>
                {#if editedSettings?.customFreqs && editedDetector.customFrequencyPolicy === 'unchanged'}
                    <p class="copy-caption">
                        Custom Frequencies will use the definitions already stored on the detector. Changing between
                        USA and Euro mode resets them to the detector's factory definitions for the selected region.
                    </p>
                {/if}
                <p class="copy-caption">
                    Bluetooth indicator control is independent only while the main display is off and requires supported firmware to keep it on. Volume feedback and disconnect behavior are sent as command policy, but the detector protocol provides no readback for those two policy bits.
                </p>
                <div class="flex flex-wrap gap-2">
                    <button class="btn btn-outline btn-sm" type="button" onclick={resetDraftToLocalDefaults}>
                        Reset this draft to local defaults
                    </button>
                </div>
                <p class="copy-caption">
                    Local defaults only edit this draft. Detector factory reset is the separately confirmed workflow attached to the captured V1 above.
                </p>
            </div>
        </div>
    {/if}

    {#if profileSchemaReady}
        <ProfileSettingsPanel
            {editingSettings}
            {currentProfile}
            {savingProfile}
            bind:editedSettings
            bind:editDescription
            oncancelEditing={cancelEditing}
            onsaveEditedProfile={saveEditedProfile}
            oncreateNewProfile={createNewProfile}
            onstartEditing={startEditing}
            onshowSaveDialog={openSaveDialog}
        />

        {#if currentProfile?.name && profiles.some((profile) => profile.name === currentProfile.name)}
            <div class="surface-card">
                <div class="card-body space-y-2">
                    <h2 class="card-title">Apply saved profile</h2>
                    <p class="copy-muted">
                        Apply uses the saved version of “{currentProfile.name}”, bound to the exact captured detector.
                        Unsaved edits on this page are never serialized into the operation.
                    </p>
                    <button class="btn btn-primary btn-sm" type="button"
                        disabled={operationBusy || !capturedSnapshot?.address}
                        onclick={applySavedProfile}>
                        Apply saved profile to captured V1
                    </button>
                </div>
            </div>
        {/if}

    {/if}

    <ProfileSavedListCard
        {loading}
        {profiles}
        allowEdit={profileSchemaReady}
        oneditProfile={editProfile}
        ondeleteProfile={deleteProfile}
    />

    <div class="surface-note copy-muted space-y-1">
        <p><strong>Create:</strong> Build a detector configuration without a V1 connection.</p>
        <p><strong>Edit:</strong> Update or delete saved profiles during maintenance.</p>
        <p>
            <strong>Apply:</strong> Load a saved profile here, then start a target-bound operation. V1Simple
            restarts into normal mode, verifies the exact captured V1, recaptures its settings, and returns.
        </p>
    </div>
</div>
