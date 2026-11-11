<script>
    import { onMount, tick } from 'svelte';
    import { fetchJsonWithTimeout, fetchWithTimeout } from '#lib/utils/poll.js';
    import PageHeader from '#lib/components/PageHeader.svelte';
    import StatusAlert from '#lib/components/StatusAlert.svelte';
    import ProfileSaveDialog from '#lib/features/profiles/ProfileSaveDialog.svelte';
    import ProfileSavedListCard from '#lib/features/profiles/ProfileSavedListCard.svelte';
    import ProfileSettingsPanel from '#lib/features/profiles/ProfileSettingsPanel.svelte';
    import ProfileOperationStatus from '#lib/features/profiles/ProfileOperationStatus.svelte';
    import { createProfileAuthoring, customFrequencyError } from '#lib/features/profiles/profileAuthoring.svelte.js';
    import { fetchProfileCatalog, requestDetectorOperation } from '#lib/features/profiles/profileApi.js';
    import { createDetectorOperationTracker } from '#lib/features/profiles/detectorOperationTracker.js';

    let profiles = $state([]);
    let catalogGeneration = 0;
    let loading = $state(true);
    let message = $state(null);
    let deletingProfile = $state(false);
    const authoring = createProfileAuthoring({ onSaved: recordSavedProfile, onMessage: (next) => { message = next; } });
    // Publish catalog writes in the same order that they reach storage.
    let mutationBusy = $derived(!!authoring.savingProfile || deletingProfile);
    let profileLoadGeneration = 0;
    let capturedSnapshot = $state(null);
    let snapshotLoading = $state(true);
    let snapshotGeneration = 0;
    let disposed = false;
    let profileSchemaReady = $state(true);
    let operationStatus = $state(null);
    let operationBusy = $state(false);
    let factoryConfirmation = $state('');
    const PROFILE_LOAD_ERROR_TEXT = 'Failed to load profiles';
    let operationTracker;

    function clearMessageText(text) {
        if (message?.text === text) {
            message = null;
        }
    }

    function recordSavedProfile(name, description, displayOn) {
        catalogGeneration += 1;
        const saved = { name, description, displayOn: displayOn ?? true };
        const found = profiles.some((profile) => profile.name === name);
        profiles = found
            ? profiles.map((profile) => (profile.name === name ? saved : profile))
            : [...profiles, saved];
    }

    function ensureCustomFrequencyOwnership(detector = authoring.draft?.detector) {
        if (!detector) return;
        detector.customFrequencyPolicy = 'value';
        if (!Array.isArray(detector.customFrequencyDefinitions)) detector.customFrequencyDefinitions = [];
    }

    function addCustomFrequencyRange(band) {
        if (!authoring.draft.detector) return;
        ensureCustomFrequencyOwnership();
        if (authoring.draft.detector.customFrequencyDefinitions.length >= 64) return;
        const range = band === 'ka'
            ? { lowerMHz: 33400, upperMHz: 36002 }
            : { lowerMHz: 23910, upperMHz: 24250 };
        authoring.draft.detector.customFrequencyDefinitions = [
            ...authoring.draft.detector.customFrequencyDefinitions,
            { index: authoring.draft.detector.customFrequencyDefinitions.length, ...range }
        ];
    }

    function removeCustomFrequencyRange(index) {
        if (!authoring.draft.detector) return;
        const remaining = authoring.draft.detector.customFrequencyDefinitions
            .filter((_, definitionIndex) => definitionIndex !== index)
            .map((definition, definitionIndex) => ({ ...definition, index: definitionIndex }));
        authoring.draft.detector.customFrequencyDefinitions = remaining;
        if (remaining.length === 0) authoring.draft.detector.customFrequencyPolicy = 'unchanged';
    }

    onMount(() => {
        void fetchProfiles();
        void fetchCapturedSnapshot();
        operationTracker = createDetectorOperationTracker({
            storage: window.localStorage,
            onStatus: (status) => {
                operationStatus = status;
                if (status && !status.terminal) {
                    // An accepted operation can change every captured value.
                    // Keep open editors independent, but stop offering this source.
                    snapshotGeneration += 1;
                    capturedSnapshot = null;
                    snapshotLoading = true;
                } else {
                    void fetchCapturedSnapshot();
                }
            },
            onMessage: (nextMessage) => { message = nextMessage; }
        });
        void operationTracker.resume();
        return () => {
            disposed = true;
            profileLoadGeneration += 1;
            authoring.dispose();
            snapshotGeneration += 1;
            operationTracker.dispose();
        };
    });

    async function startDetectorOperation(path, body, label) {
        if (operationBusy) return;
        operationBusy = true;
        message = { type: 'info', text: `${label} is being queued...` };
        try {
            const result = await requestDetectorOperation(path, body);
            if (result.kind === 'accepted') operationTracker.accepted(result.response);
            if (disposed) return;
            if (result.kind !== 'accepted') {
                message = { type: 'error', text: result.kind === 'rejected'
                    ? result.response?.error || `${label} could not be queued (HTTP ${result.status})`
                    : result.reason === 'invalid_identity' ? `${label} returned an invalid operation identity.`
                        : `${label} connection was interrupted before acceptance was confirmed.` };
                return;
            }
            const response = result.response;
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
        const savedName = authoring.acceptedProfile?.name;
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
        const generation = ++snapshotGeneration;
        capturedSnapshot = null;
        snapshotLoading = true;
        try {
            const loaded = await fetchJsonWithTimeout('/api/v1/snapshot');
            if (disposed || generation !== snapshotGeneration) return;
            const snapshot = loaded.ok ? loaded.data : null;
            capturedSnapshot = snapshot?.available === true &&
                /^[0-9A-F]{2}(?::[0-9A-F]{2}){5}$/.test(snapshot.address || '')
                ? snapshot
                : { available: false };
        } catch (e) {
            if (!disposed && generation === snapshotGeneration) {
                capturedSnapshot = { available: false };
            }
        } finally {
            if (!disposed && generation === snapshotGeneration) snapshotLoading = false;
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
        if (snapshotLoading || !capturedSnapshot?.settings || !capturedSnapshot?.observations?.userBytes?.available) return;
        profileLoadGeneration += 1;
        authoring.beginCaptured(capturedSnapshot);
        message = {
            type: 'info',
            text: 'Draft started from the last observed V1 user bytes. In-the-Box actions start off because the detector cannot report these app settings. No detector changes were made.'
        };
        void focusEditor();
    }

    async function fetchProfiles() {
        const generation = catalogGeneration;
        try {
            const catalog = await fetchProfileCatalog();
            if (disposed) return false;
            if (generation !== catalogGeneration) return await fetchProfiles();
            profiles = catalog.profiles;
            profileSchemaReady = catalog.schemaReady;
            clearMessageText(PROFILE_LOAD_ERROR_TEXT);
            return true;
        } catch (e) {
            if (disposed) return false;
            if (generation !== catalogGeneration) return await fetchProfiles();
            message = { type: 'error', text: PROFILE_LOAD_ERROR_TEXT };
            return false;
        } finally {
            loading = false;
        }
    }

    async function focusEditor() {
        await tick();
        const editor = document.getElementById('profile-editor');
        editor?.focus({ preventScroll: true });
        editor?.scrollIntoView?.({ block: 'start', behavior: 'smooth' });
    }

    function startEditing() {
        const name = authoring.acceptedProfile?.name;
        if (name) {
            // A lost save response may have committed. Reopen saved profiles
            // through the existing fresh reader; pending edits keep their snapshot.
            return editProfile(name);
        }
        profileLoadGeneration += 1;
        authoring.reedit();
    }

    function showSaveDialog() {
        profileLoadGeneration += 1;
        authoring.openNamingDialog();
    }

    function cancelProfileEditing() {
        profileLoadGeneration += 1;
        authoring.cancel();
    }

    function saveEditedProfile() {
        // This Save supersedes earlier selections; later selections still own
        // their independent load generation and authoring source.
        profileLoadGeneration += 1;
        return authoring.saveEdit(deletingProfile);
    }

    async function copyProfile(name) {
        const loadGeneration = ++profileLoadGeneration;
        try {
            const res = await fetchWithTimeout(
                `/api/v1/profile?name=${encodeURIComponent(name)}`,
                {}, undefined, async (response) => ({
                    ok: response.ok,
                    body: response.ok ? await response.json() : await response.text()
                })
            );
            if (disposed || loadGeneration !== profileLoadGeneration) return;
            if (!res.ok) {
                message = { type: 'error', text: `Failed to load: ${res.body}` };
                return;
            }
            authoring.beginCopy(res.body, profiles, name);
            message = { type: 'info', text: `Copying ${name}. Save under a new name.` };
        } catch {
            if (disposed || loadGeneration !== profileLoadGeneration) return;
            message = { type: 'error', text: 'Connection error' };
        }
    }

    async function editProfile(name) {
        const loadGeneration = ++profileLoadGeneration;
        if (authoring.reeditPending(name)) {
            message = null;
            void focusEditor();
            return;
        }
        message = { type: 'info', text: `Loading ${name}...` };
        try {
            const res = await fetchWithTimeout(
                `/api/v1/profile?name=${encodeURIComponent(name)}`,
                {},
                undefined,
                async (response) => ({
                    ok: response.ok,
                    body: response.ok ? await response.json() : await response.text()
                })
            );
            if (disposed || loadGeneration !== profileLoadGeneration) return;
            if (res.ok) {
                authoring.beginEdit(res.body);
                message = null;
                void focusEditor();
            } else {
                message = { type: 'error', text: `Failed to load: ${res.body}` };
            }
        } catch (e) {
            if (disposed || loadGeneration !== profileLoadGeneration) return;
            message = { type: 'error', text: 'Connection error' };
        }
    }

    function createNewProfile() {
        profileLoadGeneration += 1;
        authoring.beginNew();
        message = {
            type: 'info',
            text: 'Choose your settings, then save this profile. Apply it to your V1 or assign it to Auto-Push.'
        };
        void focusEditor();
    }

    function resetDraftToLocalDefaults() {
        if (!authoring.draft) return;
        profileLoadGeneration += 1;
        authoring.resetEditableDomains();
        message = {
            type: 'info',
            text: 'Draft reset to V1Simple profile defaults. No command was sent to the detector.'
        };
    }

    async function deleteProfile(name) {
        if (mutationBusy) return;
        if (!confirm(`Delete profile "${name}"?`)) return;

        deletingProfile = true;
        try {
            const res = await fetchWithTimeout(
                '/api/v1/profile/delete',
                {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({ name })
                },
                undefined,
                async (response) => ({
                    ok: response.ok,
                    errorData: response.ok ? null : await response.json().catch(() => ({}))
                })
            );
            if (res.ok) {
                catalogGeneration += 1;
                profiles = profiles.filter((profile) => profile.name !== name);
                message = { type: 'success', text: 'Profile deleted' };
            } else {
                const error = res.errorData?.error || res.errorData?.message;
                message = {
                    type: 'error',
                    text: error ? `Failed to delete: ${error}` : 'Failed to delete'
                };
            }
        } catch (e) {
            message = { type: 'error', text: 'Failed to delete' };
        } finally {
            deletingProfile = false;
        }
    }
</script>

<div class="page-stack">
    <PageHeader
        title="V1 Profiles"
        subtitle="Choose how your Valentine One behaves. Save a profile, then apply it or use it automatically."
    >
        <div class="flex flex-wrap items-center gap-2">
            <a class="btn btn-outline btn-sm" href="/autopush">Assign profiles to Auto-Push</a>
            {#if profileSchemaReady && authoring.acceptedProfile?.settings}
                <button class="btn btn-primary btn-sm" onclick={createNewProfile}>New Profile</button>
            {/if}
        </div>
    </PageHeader>

    <StatusAlert {message} />

    {#if operationStatus}
        <ProfileOperationStatus {operationStatus} />
    {/if}

    {#if !loading && !profileSchemaReady}
        <StatusAlert
            message="Profile migration is pending. Creation and editing stay read-only, but existing profiles can be deleted if the older catalog is too large to migrate safely."
            fallbackType="warning"
        />
    {/if}

    <ProfileSaveDialog
        dialog={authoring.namingDialog}
        savingProfile={authoring.savingProfile}
        {mutationBusy}
        oncancel={authoring.closeNamingDialog}
        onsave={() => authoring.saveNewName(profiles, deletingProfile)}
    />

    <ProfileSavedListCard
        {loading}
        {profiles}
        {mutationBusy}
        allowEdit={profileSchemaReady}
        currentName={authoring.acceptedProfile?.name || ''}
        compact={!!authoring.acceptedProfile?.settings}
        oneditProfile={editProfile}
        oncopyProfile={copyProfile}
        ondeleteProfile={deleteProfile}
    />

    {#if profileSchemaReady}
        <ProfileSettingsPanel
            {authoring}
            {capturedSnapshot}
            canApply={!!authoring.acceptedProfile?.name && profiles.some((profile) => profile.name === authoring.acceptedProfile.name)}
            applyDisabled={operationBusy || !capturedSnapshot?.address}
            onapply={applySavedProfile}
            {mutationBusy}
            frequencyError={authoring.draft
                ? customFrequencyError(authoring.draft.settings, authoring.draft.detector)
                : null}
            oncancelEditing={cancelProfileEditing}
            onsaveEditedProfile={saveEditedProfile}
            oncreateNewProfile={createNewProfile}
            onstartEditing={startEditing}
            onaddCustomFrequencyRange={addCustomFrequencyRange}
            onremoveCustomFrequencyRange={removeCustomFrequencyRange}
            onresetDraft={resetDraftToLocalDefaults}
            onshowSaveDialog={showSaveDialog}
        />
    {/if}

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
                {#if operationStatus && !operationStatus.terminal}
                    <p class="copy-muted">Captured settings will refresh when the detector operation finishes.</p>
                {:else}
                    <div class="state-loading compact"><span class="loading loading-spinner"></span></div>
                {/if}
            {:else if !capturedSnapshot?.available}
                <p class="state-empty">
                    No detector settings have been captured yet. Connect a V1 during normal operation, then return to maintenance.
                </p>
            {:else}
                <div class="surface-panel grid gap-2 sm:grid-cols-2">
                    <div><span class="copy-caption">Detector</span><div>{capturedSnapshot.name || capturedSnapshot.address}</div></div>
                    <div><span class="copy-caption">Firmware</span><div>{formatFirmware(capturedSnapshot.firmware?.value)}</div></div>
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
                <div>
                    <button
                        class="btn btn-primary btn-sm"
                        disabled={!profileSchemaReady || !capturedSnapshot.observations?.userBytes?.available || !capturedSnapshot.settings}
                        onclick={startDraftFromCapturedSettings}
                    >
                        Start draft from captured settings
                    </button>
                </div>
                <details class="surface-collapse">
                    <summary class="collapse-title min-h-0 py-3 text-sm font-semibold">Detector details & factory reset</summary>
                    <div class="collapse-content space-y-3">
                        <div><span class="copy-caption">Captured settings bytes</span><div class="font-mono">{formatBytes(capturedSnapshot.observations?.userBytes?.value)}</div></div>
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
                    </div>
                </details>
            {/if}
        </div>
    </div>

    <div class="surface-note copy-muted space-y-1">
        <p><strong>Save:</strong> Stores a profile on V1Simple. Your detector changes when the profile is applied.</p>
        <p><strong>Apply:</strong> Uses the saved profile on the captured V1 and checks the result.</p>
        <p><strong>Auto-Push:</strong> Applies the assigned profile when your detector connects during normal use.</p>
    </div>
</div>
