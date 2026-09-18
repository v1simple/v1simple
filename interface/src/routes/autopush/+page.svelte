<script>
    import { onMount } from 'svelte';
    import { fetchJsonWithTimeout, fetchWithTimeout } from '$lib/utils/poll';
    import PageHeader from '$lib/components/PageHeader.svelte';
    import StatusAlert from '$lib/components/StatusAlert.svelte';
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
    let busy = $state(false);
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
    const profileSchemaReady = $derived(data.schemaVersion === 3);

    const defaultSlotNames = ['Default', 'Highway', 'Comfort'];
    const slotIcons = ['🏠', '🏎️', '👥'];
    const MAINTENANCE_PUSH_NOTE =
        'Push Now starts a verified Apply for the exact captured V1, restarts briefly into normal runtime, then returns here with the durable result.';

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
        message = { type: 'info', text: `Activating slot ${slot + 1}...` };
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
                message = { type: 'success', text: `Slot ${slot + 1} activated` };
            } else {
                message = { type: 'error', text: 'Failed to activate' };
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
                text: 'Push Now is unavailable until device runtime mode can be verified.'
            };
            return;
        }
        if (!$isMaintenance) {
            message = {
                type: 'info',
                text: 'Push Now is available from maintenance mode so the verified operation can restart and return safely.'
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
        message = { type: 'info', text: 'Saving slot...' };
        const persist = Math.max(0, Math.min(5, Number(s.alertPersist ?? 0)));
        s.alertPersist = persist;

        try {
            const formData = new FormData();
            formData.append('slot', slot);
            formData.append('name', s.name);
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
                message = { type: 'error', text: 'Failed to save' };
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
        editingDraft = { ...data.slots[slot] };
    }

    function cancelEdit() {
        editingSlot = null;
        editingDraft = null;
    }
</script>

<div class="page-stack">
    <PageHeader
        title="Auto-Push Profiles"
        subtitle="Configure saved Auto-Push slots and choose the global default."
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
            message="The profile settings migration is still pending. Slot editing and activation are temporarily read-only; existing saved slots can still be pushed."
            fallbackType="warning"
        />
    {/if}

    <div class="surface-note">
        <p>
            Auto-Push sends V1 settings when you connect during normal runtime. The global default
            slot is used unless a saved V1 device override selects another slot.
        </p>
    </div>

    {#if $runtimeStatusLoading}
        <StatusAlert
            message="Checking device runtime mode before enabling live V1 pushes…"
            fallbackType="info"
        />
    {:else if !runtimeModeKnown}
        <StatusAlert
            message="Live V1 pushes are unavailable because device runtime mode could not be verified."
            fallbackType="warning"
        />
    {:else if $isMaintenance}
        <StatusAlert message={MAINTENANCE_PUSH_NOTE} fallbackType="info" />
        {#if !capturedSnapshot}
            <StatusAlert
                message="No captured V1 target is available. Capture the detector before using Push Now."
                fallbackType="warning"
            />
        {/if}
    {:else}
        <StatusAlert
            message="Push Now is available in maintenance mode; normal runtime remains dedicated to detector execution."
            fallbackType="info"
        />
    {/if}

    {#if loading}
        <div class="state-loading">
            <span class="loading loading-lg loading-spinner"></span>
        </div>
    {:else}
        <!-- Slot Cards -->
        <div class="grid gap-4">
            {#each data.slots as slot, i}
                <div class="surface-card {data.activeSlot === i ? 'ring-2 ring-primary' : ''}">
                    <div class="card-body">
                        <div class="flex items-start justify-between">
                            <div class="flex items-center gap-3">
                                <div class="text-3xl">{slotIcons[i]}</div>
                                <div>
                                    {#if editingSlot === i}
                                        <input
                                            type="text"
                                            class="input w-40 input-sm"
                                            bind:value={editingDraft.name}
                                            placeholder={defaultSlotNames[i]}
                                        />
                                    {:else}
                                        <h3 class="text-lg font-bold">
                                            {slot.name || defaultSlotNames[i]}
                                        </h3>
                                    {/if}
                                    {#if data.activeSlot === i}
                                        <span class="badge badge-sm badge-primary"
                                            >Global default</span
                                        >
                                    {/if}
                                </div>
                            </div>
                            <div class="flex gap-1">
                                {#if editingSlot === i}
                                    <button
                                        class="btn btn-sm btn-success"
                                        onclick={() => saveSlot(i)}
                                        disabled={busy ||
                                            (editingDraft.profile &&
                                                !hasProfileOption(editingDraft.profile))}
                                    >
                                        Save
                                    </button>
                                    <button class="btn btn-ghost btn-sm" onclick={cancelEdit}>
                                        Cancel
                                    </button>
                                {:else}
                                    <button
                                        class="btn btn-ghost btn-sm"
                                        disabled={!profileSchemaReady}
                                        onclick={() => beginEdit(i)}>Edit</button
                                    >
                                {/if}
                            </div>
                        </div>

                        {#if editingSlot === i}
                            <!-- Edit Mode -->
                            <div class="mt-3 grid grid-cols-2 gap-3">
                                <div class="field-control">
                                    <!-- provide stable ids for accessibility -->
                                    <label class="label py-1" for={`slot-${i}-profile`}>
                                        <span class="field-label copy-caption">Profile</span>
                                    </label>
                                    <select
                                        id={`slot-${i}-profile`}
                                        class="select w-full select-sm"
                                        bind:value={editingDraft.profile}
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
                                </div>
                                <div class="field-control">
                                    <label class="label cursor-pointer justify-start gap-3 py-1">
                                        <input
                                            type="checkbox"
                                            class="toggle toggle-primary toggle-sm"
                                            bind:checked={editingDraft.priorityArrowOnly}
                                        />
                                        <span class="field-label copy-caption"
                                            >Priority Arrow Only</span
                                        >
                                    </label>
                                </div>
                                <div class="field-control">
                                    <label class="label py-1" for={`slot-${i}-persist`}>
                                        <span class="field-label copy-caption"
                                            >Alert persistence (seconds)</span
                                        >
                                        <span class="field-hint copy-micro">0 = off, max 5s</span>
                                    </label>
                                    <div class="flex items-center gap-2">
                                        <input
                                            id={`slot-${i}-persist`}
                                            type="range"
                                            min="0"
                                            max="5"
                                            step="1"
                                            class="range flex-1 range-primary range-xs"
                                            bind:value={editingDraft.alertPersist}
                                        />
                                        <input
                                            type="number"
                                            min="0"
                                            max="5"
                                            class="input w-16 input-xs"
                                            bind:value={editingDraft.alertPersist}
                                        />
                                        <span class="copy-caption">s</span>
                                    </div>
                                </div>
                            </div>
                        {:else}
                            <!-- View Mode -->
                            <div class="mt-2 grid grid-cols-2 gap-x-4 gap-y-1 text-sm">
                                <div class="copy-muted">Profile:</div>
                                <div class="font-medium">
                                    {slot.profile || '—'}{slot.profile && !hasProfileOption(slot.profile)
                                        ? ' (missing)'
                                        : ''}
                                </div>
                                <div class="copy-muted">Options:</div>
                                <div class="font-medium">
                                    {#if slot.priorityArrowOnly}↑ Prio Arrow{/if}
                                    {#if !slot.priorityArrowOnly}—{/if}
                                </div>
                                <div class="copy-muted">Alert persistence:</div>
                                <div class="font-medium">{slot.alertPersist || 0}s</div>
                            </div>
                        {/if}

                        {#if editingSlot !== i}
                            <div class="mt-3 card-actions justify-end">
                                {#if data.activeSlot !== i}
                                    <button
                                        class="btn btn-outline btn-sm"
                                        onclick={() => activateSlot(i)}
                                        disabled={busy || !profileSchemaReady}
                                    >
                                        Activate
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
                                    Push Now
                                </button>
                            </div>
                        {/if}
                    </div>
                </div>
            {/each}
        </div>

        <!-- Info -->
        {#if profiles.length === 0}
            <StatusAlert
                message="No saved profiles. Go to V1 Profiles to pull settings from your V1 first."
                fallbackType="warning"
            />
        {/if}
    {/if}
</div>
