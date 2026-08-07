<script>
    import { onMount } from 'svelte';
    import { fetchWithTimeout } from '$lib/utils/poll';
    import PageHeader from '$lib/components/PageHeader.svelte';
    import StatusAlert from '$lib/components/StatusAlert.svelte';
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
    const runtimeModeKnown = $derived(
        !$runtimeStatusLoading &&
            !$runtimeStatusError &&
            typeof $runtimeStatus?.maintenanceBoot === 'boolean'
    );

    const modeNames = {
        0: 'Unknown',
        1: 'All Bogeys',
        2: 'Logic',
        3: 'Adv Logic'
    };

    const defaultSlotNames = ['Default', 'Highway', 'Comfort'];
    const slotIcons = ['🏠', '🏎️', '👥'];
    const MAINTENANCE_PUSH_NOTE =
        'Live V1 pushes are unavailable in maintenance mode. You can still edit, save, and select the global default slot for normal runtime.';

    onMount(() => {
        const releaseRuntimeStatus = retainRuntimeStatus({ needsStatus: true });
        void (async () => {
            await Promise.all([fetchSlots(), fetchProfiles()]);
            loading = false;
        })();
        return releaseRuntimeStatus;
    });

    async function fetchSlots() {
        try {
            const res = await fetchWithTimeout('/api/autopush/slots');
            if (!res.ok) {
                message = { type: 'error', text: 'Failed to load slots' };
                return;
            }
            const loaded = await res.json();
            // Normalize defaults for new fields
            loaded.slots = (loaded.slots || []).map((s) => {
                const volumeConfigured =
                    s.volumeConfigured ??
                    (Number(s.volume) >= 0 &&
                        Number(s.volume) <= 9 &&
                        Number(s.muteVolume) >= 0 &&
                        Number(s.muteVolume) <= 9);
                return {
                    ...s,
                    volumeConfigured,
                    volume: volumeConfigured ? Number(s.volume) : 5,
                    muteVolume: volumeConfigured ? Number(s.muteVolume) : 0,
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
            const res = await fetchWithTimeout('/api/v1/profiles');
            if (res.ok) {
                const d = await res.json();
                profiles = d.profiles || [];
            } else {
                message = { type: 'error', text: 'Failed to load profiles' };
            }
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
                body: formData
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
        if ($isMaintenance) {
            message = {
                type: 'info',
                text: 'Push Now is unavailable in maintenance mode; no settings were sent to the V1.'
            };
            return;
        }
        busy = true;
        message = { type: 'info', text: 'Pushing settings to V1...' };
        try {
            const formData = new FormData();
            formData.append('slot', slot);

            const res = await fetchWithTimeout('/api/autopush/push', {
                method: 'POST',
                body: formData
            });

            if (res.ok) {
                const queued = await res.json();
                message = {
                    type: 'info',
                    text: queued.queued
                        ? 'Push queued. The V1 has not confirmed the settings yet.'
                        : 'Push request accepted; check Auto-Push status for the result.'
                };
            } else {
                let err = {};
                try {
                    err = await res.json();
                } catch {
                    // Fall back to the HTTP status when the device has no JSON error body.
                }
                message = {
                    type: 'error',
                    text: err.message || err.error || `Push failed (HTTP ${res.status})`
                };
            }
        } catch (e) {
            message = { type: 'error', text: 'Connection error' };
        } finally {
            busy = false;
        }
    }

    async function saveSlot(slot) {
        if (busy) return;
        if (editingSlot !== slot || !editingDraft) return;
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
            formData.append('mode', s.mode);
            formData.append('volumeConfigured', s.volumeConfigured ? 'true' : 'false');
            if (s.volumeConfigured) {
                formData.append('volume', s.volume);
                formData.append('muteVol', s.muteVolume);
            }
            formData.append('darkMode', s.darkMode ? 'true' : 'false');
            formData.append('muteToZero', s.muteToZero ? 'true' : 'false');
            formData.append('alertPersist', persist);
            formData.append('priorityArrowOnly', s.priorityArrowOnly ? 'true' : 'false');

            const res = await fetchWithTimeout('/api/autopush/slot', {
                method: 'POST',
                body: formData
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
                                        disabled={busy}
                                    >
                                        Save
                                    </button>
                                    <button class="btn btn-ghost btn-sm" onclick={cancelEdit}>
                                        Cancel
                                    </button>
                                {:else}
                                    <button
                                        class="btn btn-ghost btn-sm"
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
                                                >{editingDraft.profile} (saved)</option
                                            >
                                        {/if}
                                        {#each profiles as p}
                                            <option value={p.name}>{p.name}</option>
                                        {/each}
                                    </select>
                                </div>
                                <div class="field-control">
                                    <label class="label py-1" for={`slot-${i}-mode`}>
                                        <span class="field-label copy-caption">V1 Mode</span>
                                    </label>
                                    <select
                                        id={`slot-${i}-mode`}
                                        class="select w-full select-sm"
                                        bind:value={editingDraft.mode}
                                    >
                                        <option value={0}>Don't Change</option>
                                        <option value={1}>All Bogeys</option>
                                        <option value={2}>Logic</option>
                                        <option value={3}>Adv Logic</option>
                                    </select>
                                </div>
                                <div class="field-control col-span-2">
                                    <label class="label cursor-pointer justify-start gap-3 py-1">
                                        <input
                                            type="checkbox"
                                            class="toggle toggle-primary toggle-sm"
                                            bind:checked={editingDraft.volumeConfigured}
                                        />
                                        <span class="field-label copy-caption"
                                            >Set both V1 volume levels</span
                                        >
                                    </label>
                                    <span class="field-hint copy-micro"
                                        >The V1 applies main and mute volume as one pair.</span
                                    >
                                </div>
                                <div class="field-control">
                                    <label class="label py-1" for={`slot-${i}-volume`}>
                                        <span class="field-label copy-caption">Volume (0-9)</span>
                                    </label>
                                    <input
                                        id={`slot-${i}-volume`}
                                        type="number"
                                        class="input w-full input-sm"
                                        min="0"
                                        max="9"
                                        bind:value={editingDraft.volume}
                                        disabled={!editingDraft.volumeConfigured}
                                    />
                                </div>
                                <div class="field-control">
                                    <label class="label py-1" for={`slot-${i}-mute`}>
                                        <span class="field-label copy-caption"
                                            >Mute Volume (0-9)</span
                                        >
                                    </label>
                                    <input
                                        id={`slot-${i}-mute`}
                                        type="number"
                                        class="input w-full input-sm"
                                        min="0"
                                        max="9"
                                        bind:value={editingDraft.muteVolume}
                                        disabled={!editingDraft.volumeConfigured}
                                    />
                                </div>
                                <div class="field-control">
                                    <label class="label cursor-pointer justify-start gap-3 py-1">
                                        <input
                                            type="checkbox"
                                            class="toggle toggle-primary toggle-sm"
                                            bind:checked={editingDraft.darkMode}
                                        />
                                        <span class="field-label copy-caption"
                                            >Dark Mode (V1 display off)</span
                                        >
                                    </label>
                                </div>
                                <div class="field-control">
                                    <label class="label cursor-pointer justify-start gap-3 py-1">
                                        <input
                                            type="checkbox"
                                            class="toggle toggle-primary toggle-sm"
                                            bind:checked={editingDraft.muteToZero}
                                        />
                                        <span class="field-label copy-caption">Mute to Zero</span>
                                    </label>
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
                                <div class="font-medium">{slot.profile || '—'}</div>
                                <div class="copy-muted">Mode:</div>
                                <div class="font-medium">{modeNames[slot.mode] || '—'}</div>
                                <div class="copy-muted">Volume:</div>
                                <div class="font-medium">
                                    {slot.volumeConfigured
                                        ? `${slot.volume} / Mute: ${slot.muteVolume}`
                                        : "Don't change"}
                                </div>
                                <div class="copy-muted">Options:</div>
                                <div class="font-medium">
                                    {#if slot.darkMode}🌙 Dark{/if}
                                    {#if slot.darkMode && slot.muteToZero}
                                        ·
                                    {/if}
                                    {#if slot.muteToZero}🔇 MZ{/if}
                                    {#if (slot.darkMode || slot.muteToZero) && slot.priorityArrowOnly}
                                        ·
                                    {/if}
                                    {#if slot.priorityArrowOnly}↑ Prio Arrow{/if}
                                    {#if !slot.darkMode && !slot.muteToZero && !slot.priorityArrowOnly}—{/if}
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
                                        disabled={busy}
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
                                        $isMaintenance}
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
