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
        detectorConfigurationFromSnapshot,
        fromApiDetectorConfiguration,
        fromApiSettings,
        toApiDetectorConfiguration,
        toApiSettings
    } from '$lib/features/profiles/profileSettingsAdapter';

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
    let editDescription = $state('');
    let capturedSnapshot = $state(null);
    let snapshotLoading = $state(true);
    let profileSchemaReady = $state(true);
    const PROFILE_LOAD_ERROR_TEXT = 'Failed to load profiles';

    function validateProfileName(raw) {
        const canonical = raw.trim();
        if (!canonical) return { error: 'Profile name required' };
        if (canonical.length > 64) return { error: 'Profile name exceeds 64 characters' };
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
        const collision = profiles.find(
            (profile) =>
                profile.name.toLocaleLowerCase() === canonical.toLocaleLowerCase() &&
                profile.name !== canonical
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

    onMount(() => {
        void fetchProfiles();
        void fetchCapturedSnapshot();
    });

    async function fetchCapturedSnapshot() {
        try {
            const res = await fetchWithTimeout('/api/v1/snapshot');
            capturedSnapshot = res.ok ? await res.json() : { available: false };
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
        editedDetector = { ...currentProfile.detector };
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
            const res = await fetchWithTimeout('/api/v1/profiles');
            if (res.ok) {
                const data = await res.json();
                profiles = data.profiles || [];
                profileSchemaReady = data.schemaVersion !== 1;
                clearMessageText(PROFILE_LOAD_ERROR_TEXT);
                return true;
            } else {
                message = { type: 'error', text: PROFILE_LOAD_ERROR_TEXT };
                return false;
            }
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
                description: saveDescription.trim(),
                schemaVersion: 2,
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
            editedDetector = { ...currentProfile.detector };
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
                editedDetector = { ...currentProfile.detector };
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

    async function saveEditedProfile() {
        if (savingProfile) return;
        if (!editedSettings || !currentProfile || !currentProfile.name) {
            message = { type: 'error', text: 'No profile loaded to save' };
            return;
        }

        const profile = currentProfile;
        const editSession = editedSettings;
        const savedSettings = { ...editedSettings };
        savingProfile = profile.name;
        message = { type: 'info', text: `Saving ${profile.name}...` };
        try {
            const payload = {
                name: profile.name,
                description: editDescription.trim(),
                schemaVersion: 2,
                detector: toApiDetectorConfiguration(editedDetector || profile.detector),
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
                if (editedSettings === editSession) {
                    const draftUnchanged = editDescription.trim() === payload.description &&
                        Object.entries(savedSettings).every(([key, value]) => editedSettings[key] === value);
                    currentProfile = {
                        ...profile,
                        description: payload.description,
                        detector: { ...editedDetector },
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

    {#if !loading && !profileSchemaReady}
        <StatusAlert
            message="The profile settings migration is still pending. Profiles are temporarily read-only so detector choices cannot be accepted and then ignored."
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
                </div>

                {#if capturedSnapshot.provenance?.captureTimedOut}
                    <div class="surface-alert alert-warning" role="status">
                        The connection capture timed out. Available values are preserved; missing values remain unknown.
                    </div>
                {/if}
                {#if capturedSnapshot.capabilities?.versionKnown}
                    <p class="copy-caption">
                        Firmware-qualified: {capturedSnapshot.capabilities.supportedUserByteCount} user bytes,
                        saved volume {capturedSnapshot.capabilities.savedVolume ? 'supported' : 'not supported'},
                        Gatso RT4 {capturedSnapshot.capabilities.settings?.gatsoRT4 ? 'supported' : 'not supported'}.
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
                        <select class="select select-sm" bind:value={editedDetector.display}>
                            <option value="unchanged">Leave unchanged</option>
                            <option value="on">On</option>
                            <option value="off">Off (completely dark)</option>
                        </select>
                    </label>
                    <label class="field-control">
                        <span class="field-label copy-caption">Volume policy</span>
                        <select class="select select-sm" bind:value={editedDetector.volumePolicy}>
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
                </div>
                {#if editedDetector.volumePolicy === 'saved'}
                    <div class="surface-alert alert-warning" role="status">
                        Saved-volume intent is preserved in this profile. Until saved-volume transport lands, Auto-Push rejects this profile before sending any detector changes.
                    </div>
                {/if}
                {#if editedSettings?.customFreqs}
                    <div class="surface-alert alert-warning" role="status">
                        Custom Frequencies only enables definitions already stored in the V1. Sweep definitions are not captured or authored in this version.
                    </div>
                {/if}
                <p class="copy-caption">
                    Independent keep-Bluetooth-LED-on control is not supported yet. Display Off is completely dark,
                    including the Bluetooth LED. Custom-frequency definitions remain detector-resident until their
                    complete vendor transactions are implemented.
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

        <ProfileSavedListCard
            {loading}
            {profiles}
            oneditProfile={editProfile}
            ondeleteProfile={deleteProfile}
        />
    {/if}

    <div class="surface-note copy-muted space-y-1">
        <p><strong>Create:</strong> Build a detector configuration without a V1 connection.</p>
        <p><strong>Edit:</strong> Update or delete saved profiles during maintenance.</p>
        <p>
            <strong>Apply:</strong> Assign a saved profile on the Auto-Push page. It is sent automatically
            after the next matching V1 connection in normal operation.
        </p>
    </div>
</div>
