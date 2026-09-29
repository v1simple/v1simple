<script>
    import { onMount } from 'svelte';
    import { fetchJsonWithTimeout, fetchWithTimeout } from '$lib/utils/poll';
    import PageHeader from '$lib/components/PageHeader.svelte';
    import StatusAlert from '$lib/components/StatusAlert.svelte';
    import ColorControl from '$lib/components/ColorControl.svelte';
    import ColorPickerModal from '$lib/components/ColorPickerModal.svelte';
    import { parseColorInput, rgb565ToHex, rgb565ToRgb888, rgb888ToRgb565 } from '$lib/utils/colors';
    import {
        DETECTOR_OPERATION_COMPONENTS,
        DETECTOR_OPERATION_POLL_WINDOW_MS,
        DETECTOR_OPERATION_STATES,
        forgetDetectorOperationId,
        readDetectorOperationId,
        rememberDetectorOperationId,
        validDetectorOperationStart,
        validDetectorOperationStatus
    } from '$lib/features/profiles/detectorOperation';
    import {
        isMaintenance,
        retainRuntimeStatus,
        runtimeStatus,
        runtimeStatusError,
        runtimeStatusLoading
    } from '$lib/stores/runtimeStatus.svelte.js';

    let data = $state({
        enabled: false,
        activeSlot: 0,
        slots: []
    });

    let profiles = $state([]);
    let loading = $state(true);
    let message = $state(null);
    let editingSlot = $state(null);
    let editingDraft = $state(null);
    let selectedProfileVolumePolicy = $state(null);
    let busy = $state(false);
    let pickerOpen = $state(false);
    let pickerR = $state(0);
    let pickerG = $state(0);
    let pickerB = $state(0);
    let capturedSnapshot = $state(null);
    let operationStatus = $state(null);
    let operationPollTimer = null;
    let operationPollDeadline = 0;
    let operationPollFailures = 0;
    let operationPollEpoch = 0;
    const runtimeModeKnown = $derived(
        !$runtimeStatusLoading &&
            !$runtimeStatusError &&
            typeof $runtimeStatus?.maintenanceBoot === 'boolean'
    );
    const profileSchemaReady = $derived(data.schemaVersion === 4);

    const defaultSlotNames = ['Default', 'Highway', 'Comfort'];
    const defaultSlotColors = [0x400a, 0x07e0, 0x8410];
    const slotIcons = ['🏠', '🏎️', '👥'];
    const MAINTENANCE_PUSH_NOTE =
        'Apply to V1 uses the captured detector. V1Simple restarts to apply and check its settings, then returns here with the result.';

    onMount(() => {
        const releaseRuntimeStatus = retainRuntimeStatus({ needsStatus: true });
        void (async () => {
            await Promise.all([fetchSlots(), fetchProfiles(), fetchCapturedSnapshot()]);
            loading = false;
        })();
        operationPollDeadline = Date.now() + DETECTOR_OPERATION_POLL_WINDOW_MS;
        void resumeDetectorOperation();
        return () => {
            operationPollEpoch += 1;
            releaseRuntimeStatus();
            if (operationPollTimer) clearTimeout(operationPollTimer);
        };
    });

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
                : null;
        } catch {
            capturedSnapshot = null;
        }
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

    async function resumeDetectorOperation() {
        const operationId = readDetectorOperationId(window.localStorage);
        if (!operationId) return;
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
            if (pollEpoch !== operationPollEpoch ||
                readDetectorOperationId(window.localStorage) !== operationId) return;
            if (res.status === 409) {
                operationStatus = null;
                forgetDetectorOperationId(window.localStorage);
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
            if (!validDetectorOperationStatus(status, operationId)) {
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
            if (status.terminal) forgetDetectorOperationId(window.localStorage);
            else scheduleOperationPoll();
        } catch {
            if (pollEpoch !== operationPollEpoch ||
                readDetectorOperationId(window.localStorage) !== operationId) return;
            operationPollFailures += 1;
            scheduleOperationPoll();
        }
    }

    async function fetchSlots() {
        try {
            const res = await fetchJsonWithTimeout('/api/autopush/slots');
            if (!res.ok) {
                message = { type: 'error', text: 'Failed to load slots' };
                return;
            }
            const loaded = res.data;
            loaded.slots = (loaded.slots || []).map((s) => {
                return {
                    ...s,
                    volumeConfigured: s.volumeConfigured ?? false,
                    volume: s.volumeConfigured ? s.volume : 5,
                    muteVolume: s.volumeConfigured ? s.muteVolume : 0,
                    darkModeConfigured: s.darkModeConfigured ?? false,
                    darkMode: s.darkMode ?? false,
                    alertPersist: s.alertPersist ?? 0,
                    priorityArrowOnly: s.priorityArrowOnly ?? false
                };
            });
            data = loaded;
        } catch (e) {
            message = { type: 'error', text: 'Failed to load slots' };
        }
    }

    async function fetchProfiles() {
        try {
            const loadedProfiles = [];
            let cursor = '';
            while (true) {
                const url = cursor
                    ? `/api/v1/profiles?after=${encodeURIComponent(cursor)}&limit=10`
                    : '/api/v1/profiles';
                const res = await fetchJsonWithTimeout(url);
                if (!res.ok) {
                    message = { type: 'error', text: 'Failed to load profiles' };
                    return;
                }
                const d = res.data;
                if (!Array.isArray(d.profiles)) throw new Error('Invalid profile page');
                loadedProfiles.push(...d.profiles);
                if (!d.hasMore) break;
                if (typeof d.nextCursor !== 'string' || !d.nextCursor || d.nextCursor === cursor) {
                    throw new Error('Invalid profile cursor');
                }
                cursor = d.nextCursor;
            }
            profiles = loadedProfiles;
        } catch (e) {
            message = { type: 'error', text: 'Failed to load profiles' };
        }
    }

    async function activateSlot(slot) {
        if (busy) return;
        busy = true;
        message = { type: 'info', text: `Setting slot ${slot + 1} as default...` };
        try {
            const formData = new FormData();
            formData.append('slot', slot);
            formData.append('enable', 'true');

            const res = await fetchWithTimeout('/api/autopush/activate', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
                body: new URLSearchParams(formData)
            });

            if (res.ok) {
                data.activeSlot = slot;
                data.enabled = true;
                message = { type: 'success', text: `Slot ${slot + 1} set as default` };
            } else {
                message = { type: 'error', text: 'Failed to set default slot' };
            }
        } catch (e) {
            message = { type: 'error', text: 'Connection error' };
        } finally {
            busy = false;
        }
    }

    async function pushNow(slot) {
        if (busy) return;
        if (!runtimeModeKnown) {
            message = {
                type: 'warning',
                text: 'Apply to V1 is unavailable until device runtime mode can be verified.'
            };
            return;
        }
        if (!$isMaintenance) {
            message = {
                type: 'info',
                text: 'Apply to V1 is available in maintenance mode, where V1Simple can restart and return with the result.'
            };
            return;
        }
        const address = capturedSnapshot?.address;
        if (!address) {
            message = { type: 'error', text: 'Capture a V1 in maintenance mode before applying this slot.' };
            return;
        }
        busy = true;
        message = { type: 'info', text: `Queueing slot ${slot + 1} for ${address}...` };
        try {
            const body = new URLSearchParams();
            body.set('slot', String(slot));
            body.set('address', address);

            const res = await fetchWithTimeout('/api/autopush/push', {
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
                    text: response.message || response.error || `Apply could not be queued (HTTP ${res.status})`
                };
                return;
            }
            if (!validDetectorOperationStart(response)) {
                message = { type: 'error', text: 'Apply returned an invalid operation identity.' };
                return;
            }
            operationPollEpoch += 1;
            if (operationPollTimer) clearTimeout(operationPollTimer);
            rememberDetectorOperationId(window.localStorage, response.operationId);
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
                text: `Slot ${slot + 1} queued as operation ${response.operationId} for ${address}. V1Simple will return here with verification results.`
            };
        } catch (e) {
            message = { type: 'error', text: 'Connection error' };
        } finally {
            busy = false;
        }
    }

    async function saveSlot(slot) {
        if (busy) return;
        if (editingSlot !== slot || !editingDraft) return;
        if (editingDraft.profile && !hasProfileOption(editingDraft.profile)) {
            message = {
                type: 'error',
                text: 'Choose an available profile or None before saving this stale reference.'
            };
            return;
        }
        busy = true;
        const s = editingDraft;
        if (s.volumeConfigured && (!Number.isInteger(Number(s.volume)) ||
            !Number.isInteger(Number(s.muteVolume)) || Number(s.volume) < 0 ||
            Number(s.volume) > 9 || Number(s.muteVolume) < 0 || Number(s.muteVolume) > 9)) {
            message = { type: 'error', text: 'Main and muted volume must each be between 0 and 9' };
            busy = false;
            return;
        }
        if (s.volumeConfigured && !['temporary', 'saved'].includes(selectedProfileVolumePolicy)) {
            message = { type: 'error', text: 'Choose a profile with Temporary or Save on V1 volume policy before overriding volume' };
            busy = false;
            return;
        }
        if (!Number.isInteger(s.color) || s.color < 1 || s.color > 0xffff) {
            message = { type: 'error', text: 'Choose a visible slot color before saving' };
            busy = false;
            return;
        }
        message = { type: 'info', text: 'Saving slot...' };
        const persist = Math.max(0, Math.min(5, Number(s.alertPersist ?? 0)));
        s.alertPersist = persist;

        try {
            const formData = new FormData();
            formData.append('slot', slot);
            // Match firmware's ASCII-only uppercase display-name representation.
            formData.append('name', s.name.replace(/[a-z]/g, (letter) => letter.toUpperCase()));
            formData.append('color', String(s.color));
            formData.append('volumeConfigured', s.volumeConfigured ? 'true' : 'false');
            if (s.volumeConfigured) {
                formData.append('volume', String(s.volume));
                formData.append('muteVol', String(s.muteVolume));
            }
            formData.append('darkModeConfigured', s.darkModeConfigured ? 'true' : 'false');
            if (s.darkModeConfigured) formData.append('darkMode', s.darkMode ? 'true' : 'false');
            formData.append('profile', s.profile);
            formData.append('clearProfile', s.profile ? 'false' : 'true');
            formData.append('alertPersist', persist);
            formData.append('priorityArrowOnly', s.priorityArrowOnly ? 'true' : 'false');

            const res = await fetchWithTimeout('/api/autopush/slot', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
                body: new URLSearchParams(formData)
            });

            if (res.ok) {
                message = { type: 'success', text: 'Slot saved!' };
                editingSlot = null;
                editingDraft = null;
                await fetchSlots();
            } else {
                const error = await res.json().catch(() => null);
                message = { type: 'error', text: res.status === 409 && error?.error ? error.error : 'Failed to save' };
            }
        } catch (e) {
            message = { type: 'error', text: 'Connection error' };
        } finally {
            busy = false;
        }
    }

    function hasProfileOption(profileName) {
        if (!profileName) return true;
        return profiles.some((p) => p.name === profileName);
    }

    function beginEdit(slot) {
        editingSlot = slot;
        editingDraft = {
            ...data.slots[slot],
            color: data.slots[slot].color || defaultSlotColors[slot]
        };
        void loadSelectedProfilePolicy(editingDraft.profile);
    }

    async function loadSelectedProfilePolicy(name) {
        selectedProfileVolumePolicy = null;
        if (!name) return;
        try {
            const result = await fetchWithTimeout(
                `/api/v1/profile?name=${encodeURIComponent(name)}`,
                {}, undefined, async (response) => ({
                    ok: response.ok,
                    body: response.ok ? await response.json() : null
                })
            );
            if (editingDraft?.profile === name && result.ok) {
                selectedProfileVolumePolicy = result.body?.detector?.volume?.policy || 'unchanged';
            }
        } catch {
            // Saving a volume modifier remains disabled until the assigned profile can be checked.
        }
    }

    function setSlotColor(value) {
        const color = parseColorInput(value);
        if (color === null || color === 0) {
            message = { type: 'error', text: 'Enter a visible RGB565 or six-digit RGB color' };
            return;
        }
        editingDraft.color = color;
    }

    function openSlotColorPicker() {
        const rgb = rgb565ToRgb888(editingDraft.color);
        pickerR = rgb.red;
        pickerG = rgb.green;
        pickerB = rgb.blue;
        pickerOpen = true;
    }

    function applySlotColorPicker() {
        const color = rgb888ToRgb565(pickerR, pickerG, pickerB);
        if (color === 0) {
            message = { type: 'error', text: 'Choose a visible slot color' };
            return;
        }
        editingDraft.color = color;
        pickerOpen = false;
    }

    function cancelEdit() {
        pickerOpen = false;
        selectedProfileVolumePolicy = null;
        editingSlot = null;
        editingDraft = null;
    }
</script>

<div class="page-stack">
    <PageHeader
        title="Auto-Push Profiles"
        subtitle="Choose the profiles V1Simple applies when your V1 connects."
    >
        <div class="badge {data.enabled ? 'badge-success' : 'badge-ghost'}">
            {data.enabled ? 'Enabled' : 'Disabled'}
        </div>
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
                {#if operationStatus.reason === 'in_the_box_persist_failed'}
                    <p class="copy-caption">V1Simple could not confirm saving the In-the-Box choices, so it did not activate them. Detector results are shown below.</p>
                {/if}
                {#if operationStatus.kind && operationStatus.targetAddress}
                    <p class="copy-caption">
                        {operationStatus.kind} · target {operationStatus.targetAddress} · source {operationStatus.source}
                    </p>
                {/if}
                {#if operationStatus.components}
                    <div class="grid gap-2 sm:grid-cols-2">
                        {#each DETECTOR_OPERATION_COMPONENTS as name (name)}
                            {@const component = operationStatus.components[name]}
                            {#if component?.requested}
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
            message="The profile settings migration is still pending. Slot editing and default selection are temporarily read-only; existing saved slots can still be applied."
            fallbackType="warning"
        />
    {/if}

    <div class="surface-note space-y-3">
        <p>
            Each slot uses a saved V1 profile. Auto-Push applies the global default when your V1
            connects, unless that detector has its own slot selected in <a class="link" href="/devices">Devices</a>.
        </p>
        <details>
            <summary class="cursor-pointer text-sm font-semibold">Saving and applying slots</summary>
            <dl class="mt-3 grid gap-3 sm:grid-cols-3">
                <div>
                    <dt class="font-semibold">Save slot</dt>
                    <dd class="copy-caption">Stores the profile assignment, name, color and options.</dd>
                </div>
                <div>
                    <dt class="font-semibold">Set as default</dt>
                    <dd class="copy-caption">Enables Auto-Push and chooses the global default for future connections.</dd>
                </div>
                <div>
                    <dt class="font-semibold">Apply to V1</dt>
                    <dd class="copy-caption">Applies a saved slot to the captured V1 and checks the result.</dd>
                </div>
            </dl>
        </details>
    </div>

    {#if $runtimeStatusLoading}
        <StatusAlert
            message="Checking device runtime mode before enabling Apply to V1…"
            fallbackType="info"
        />
    {:else if !runtimeModeKnown}
        <StatusAlert
            message="Apply to V1 is unavailable because device runtime mode could not be verified."
            fallbackType="warning"
        />
    {:else if $isMaintenance}
        <StatusAlert message={MAINTENANCE_PUSH_NOTE} fallbackType="info" />
        {#if !capturedSnapshot}
            <StatusAlert
                message="No captured V1 is available. Capture the detector in V1 Profiles before using Apply to V1."
                fallbackType="warning"
            />
        {/if}
    {:else}
        <StatusAlert
            message="Apply to V1 is available in maintenance mode. Auto-Push still applies the selected slot when your V1 connects during normal use."
            fallbackType="info"
        />
    {/if}

    {#if loading}
        <div class="state-loading">
            <span class="loading loading-lg loading-spinner"></span>
        </div>
    {:else}
        <div class="grid gap-4">
            {#each data.slots as slot, i}
                <div class="surface-card {data.activeSlot === i ? 'ring-2 ring-primary' : ''}">
                    <div class="card-body">
                        <div class="flex flex-wrap items-start justify-between gap-3">
                            <div class="flex min-w-0 items-center gap-3">
                                <div class="text-3xl" aria-hidden="true">{slotIcons[i]}</div>
                                <span
                                    class="color-swatch-btn sm"
                                    style={`background-color: ${rgb565ToHex(slot.color)}`}
                                    role="img"
                                    aria-label={`${slot.name || defaultSlotNames[i]} color`}
                                ></span>
                                <div class="min-w-0">
                                    <p class="copy-caption">Slot {i + 1}</p>
                                    <h3 class="break-words text-lg font-bold">
                                        {slot.name || defaultSlotNames[i]}
                                    </h3>
                                    {#if data.activeSlot === i}
                                        <span class="badge badge-sm badge-primary">Global default</span>
                                    {/if}
                                </div>
                            </div>
                            {#if editingSlot !== i}
                                <button
                                    class="btn btn-ghost btn-sm"
                                    disabled={!profileSchemaReady}
                                    onclick={() => beginEdit(i)}>Edit</button
                                >
                            {/if}
                        </div>

                        {#if editingSlot === i}
                            <div class="mt-3 space-y-5">
                                <div class="field-control">
                                    <label class="label py-1" for={`slot-${i}-profile`}>
                                        <span class="field-label">Profile</span>
                                    </label>
                                    <select
                                        id={`slot-${i}-profile`}
                                        class="select w-full select-sm"
                                        bind:value={editingDraft.profile}
                                        onchange={() => void loadSelectedProfilePolicy(editingDraft.profile)}
                                    >
                                        <option value="">-- None --</option>
                                        {#if editingDraft.profile && !hasProfileOption(editingDraft.profile)}
                                            <option value={editingDraft.profile}
                                                >{editingDraft.profile} (missing)</option
                                            >
                                        {/if}
                                        {#each profiles as p}
                                            <option value={p.name}>{p.name}</option>
                                        {/each}
                                    </select>
                                    <p class="copy-caption">The saved detector settings this slot will apply.</p>
                                </div>

                                <div class="grid grid-cols-1 gap-3 sm:grid-cols-2">
                                    <label class="field-control">
                                        <span class="field-label copy-caption">Slot name</span>
                                        <input
                                            type="text"
                                            class="input w-full input-sm"
                                            bind:value={editingDraft.name}
                                            placeholder={defaultSlotNames[i]}
                                        />
                                    </label>
                                    <ColorControl
                                        id={`slot-${i}-color`}
                                        label="Slot color"
                                        value={editingDraft.color}
                                        ariaLabel={`Choose ${editingDraft.name || defaultSlotNames[i]} color`}
                                        onPick={openSlotColorPicker}
                                        onHexChange={setSlotColor}
                                    />
                                </div>

                                <details class="surface-panel" open={editingDraft.volumeConfigured || editingDraft.darkModeConfigured}>
                                    <summary class="cursor-pointer font-semibold">Override profile settings</summary>
                                    <div class="mt-3 space-y-4">
                                        <p class="copy-caption">Optional changes for this slot. The saved profile stays unchanged.</p>
                                        <div class="field-control">
                                            <label class="label cursor-pointer justify-start gap-3 py-1">
                                                <input type="checkbox" class="toggle toggle-primary toggle-sm"
                                                    bind:checked={editingDraft.volumeConfigured} />
                                                <span class="field-label copy-caption">Override profile volume</span>
                                            </label>
                                            {#if editingDraft.volumeConfigured}
                                                <div class="grid grid-cols-1 gap-3 sm:grid-cols-2">
                                                    <label class="field-control">
                                                        <span class="field-label copy-caption">Main volume (0–9)</span>
                                                        <input class="input w-full input-sm" type="number" min="0" max="9" bind:value={editingDraft.volume} />
                                                    </label>
                                                    <label class="field-control">
                                                        <span class="field-label copy-caption">Muted volume (0–9)</span>
                                                        <input class="input w-full input-sm" type="number" min="0" max="9" bind:value={editingDraft.muteVolume} />
                                                    </label>
                                                </div>
                                                <p class="copy-caption">Uses the assigned profile’s {selectedProfileVolumePolicy === 'saved' ? 'Save on V1' : selectedProfileVolumePolicy === 'temporary' ? 'Temporary' : 'volume'} policy. The profile must use Temporary or Save on V1.</p>
                                            {/if}
                                        </div>
                                        <label class="field-control">
                                            <span class="field-label copy-caption">V1 detector display</span>
                                            <select class="select w-full min-w-0 select-sm" value={editingDraft.darkModeConfigured ? (editingDraft.darkMode ? 'on' : 'off') : 'profile'}
                                                onchange={(event) => {
                                                    editingDraft.darkModeConfigured = event.currentTarget.value !== 'profile';
                                                    editingDraft.darkMode = event.currentTarget.value === 'on';
                                                }}>
                                                <option value="profile">Use profile</option>
                                                <option value="on">Display off — Bluetooth light follows profile</option>
                                                <option value="off">Display on</option>
                                            </select>
                                        </label>
                                        <p class="copy-caption">
                                            With the detector display off, the Bluetooth light follows the assigned profile.
                                            Choose its light setting in <a class="link" href="/profiles">V1 Profiles</a>.
                                        </p>
                                    </div>
                                </details>

                                <section class="space-y-3" aria-labelledby={`slot-${i}-screen`}>
                                    <div>
                                        <h4 id={`slot-${i}-screen`} class="font-semibold">V1Simple display</h4>
                                        <p class="copy-caption">Choose how alerts appear on the V1Simple screen.</p>
                                    </div>
                                    <div class="field-control">
                                        <label class="label cursor-pointer justify-start gap-3 py-1">
                                            <input
                                                type="checkbox"
                                                class="toggle toggle-primary toggle-sm"
                                                bind:checked={editingDraft.priorityArrowOnly}
                                            />
                                            <span class="field-label copy-caption">Priority arrow only</span>
                                        </label>
                                        <p class="copy-caption">Show only the priority alert’s direction. Leave off to show all V1 alert directions.</p>
                                    </div>
                                    <div class="field-control">
                                        <label class="label flex-wrap py-1" for={`slot-${i}-persist`}>
                                            <span class="field-label copy-caption">Alert persistence (seconds)</span>
                                            <span class="field-hint copy-micro">0 = off, max 5s</span>
                                        </label>
                                        <div class="flex items-center gap-2">
                                            <input
                                                id={`slot-${i}-persist`}
                                                type="range"
                                                min="0"
                                                max="5"
                                                step="1"
                                                class="range min-w-0 flex-1 range-primary range-xs"
                                                bind:value={editingDraft.alertPersist}
                                            />
                                            <input
                                                aria-label="Alert persistence value (seconds)"
                                                type="number"
                                                min="0"
                                                max="5"
                                                class="input w-16 input-xs"
                                                bind:value={editingDraft.alertPersist}
                                            />
                                            <span class="copy-caption">s</span>
                                        </div>
                                        <p class="copy-caption">Keep the last alert visible briefly after it clears.</p>
                                    </div>
                                </section>
                                <div class="card-actions justify-end">
                                    <button class="btn btn-ghost btn-sm" onclick={cancelEdit}>Cancel</button>
                                    <button
                                        class="btn btn-sm btn-success"
                                        onclick={() => saveSlot(i)}
                                        disabled={busy ||
                                            (editingDraft.profile &&
                                                !hasProfileOption(editingDraft.profile))}
                                    >
                                        Save slot
                                    </button>
                                </div>
                            </div>
                        {:else}
                            <div class="mt-3 space-y-3">
                                <div>
                                    <p class="copy-caption">Profile</p>
                                    <p class="break-words text-lg font-semibold">
                                        {slot.profile || 'None selected'}{slot.profile && !hasProfileOption(slot.profile)
                                            ? ' (missing)'
                                            : ''}
                                    </p>
                                </div>
                                <dl class="grid grid-cols-1 gap-3 text-sm sm:grid-cols-2">
                                    <div>
                                        <dt class="copy-muted">Volume</dt>
                                        <dd>{slot.volumeConfigured ? `Main ${slot.volume} · Muted ${slot.muteVolume}` : 'Use profile'}</dd>
                                    </div>
                                    <div>
                                        <dt class="copy-muted">V1 detector display</dt>
                                        <dd>{slot.darkModeConfigured ? (slot.darkMode ? 'Display off — Bluetooth light follows profile' : 'Display on') : 'Use profile'}</dd>
                                    </div>
                                    <div>
                                        <dt class="copy-muted">V1Simple arrows</dt>
                                        <dd>{slot.priorityArrowOnly ? 'Priority arrow only' : 'All alert directions'}</dd>
                                    </div>
                                    <div>
                                        <dt class="copy-muted">V1Simple alert persistence</dt>
                                        <dd>{slot.alertPersist ? `${slot.alertPersist}s after an alert clears` : 'Off'}</dd>
                                    </div>
                                </dl>
                            </div>
                            <div class="mt-3 card-actions justify-end">
                                {#if data.activeSlot !== i}
                                    <button
                                        class="btn btn-outline btn-sm"
                                        onclick={() => activateSlot(i)}
                                        disabled={busy || !profileSchemaReady}
                                    >
                                        Set as default
                                    </button>
                                {/if}
                                <button
                                    class="btn btn-primary btn-sm"
                                    onclick={() => pushNow(i)}
                                    disabled={!slot.profile ||
                                        busy ||
                                        !runtimeModeKnown ||
                                        !$isMaintenance ||
                                        !capturedSnapshot}
                                >
                                    Apply to V1
                                </button>
                            </div>
                        {/if}
                    </div>
                </div>
            {/each}
        </div>

        {#if profiles.length === 0}
            <StatusAlert
                message="No saved profiles. Go to V1 Profiles to pull settings from your V1 first."
                fallbackType="warning"
            />
        {/if}
    {/if}

    <ColorPickerModal
        open={pickerOpen}
        label="Slot color"
        bind:red={pickerR}
        bind:green={pickerG}
        bind:blue={pickerB}
        oncancel={() => { pickerOpen = false; }}
        onapply={applySlotColorPicker}
    />
</div>
