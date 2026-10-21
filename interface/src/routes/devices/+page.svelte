<script>
    import { onMount } from 'svelte';
    import { fetchJsonWithTimeout, fetchWithTimeout } from '#lib/utils/poll.js';
    import PageHeader from '#lib/components/PageHeader.svelte';
    import StatusAlert from '#lib/components/StatusAlert.svelte';
    import { isMaintenance, retainRuntimeStatus } from '#lib/stores/runtimeStatus.svelte.js';

    const fallbackSlotNames = ['DEFAULT', 'HIGHWAY', 'COMFORT'];

    let devices = $state([]);
    let slots = $state([]);
    let loading = $state(true);
    let message = $state(null);
    let editingAddress = $state('');
    let editName = $state('');
    let busyAddresses = $state([]);
    let renameGeneration = 0;
    let activeDeviceFetch = null;

    onMount(() => {
        const releaseRuntimeStatus = retainRuntimeStatus({ needsStatus: true });
        void (async () => {
            try {
                await refresh();
            } finally {
                loading = false;
            }
        })();
        return releaseRuntimeStatus;
    });

    async function refresh() {
        await Promise.all([fetchDevices(), fetchSlots()]);
    }

    async function fetchDevices() {
        const request = { changedAddresses: new Set() };
        activeDeviceFetch = request;
        try {
            const res = await fetchJsonWithTimeout('/api/v1/devices');
            if (activeDeviceFetch !== request) return;
            if (!res.ok) {
                message = { type: 'error', text: 'Failed to load saved devices.' };
                return;
            }
            const data = res.data;
            const loaded = (data.devices || []).map((device) => ({
                address: device.address || '',
                name: device.name || '',
                defaultProfile: Number(device.defaultProfile || 0),
                connected: !!device.connected
            }));
            const merged = new Map(loaded.map((device) => [device.address, device]));
            // This snapshot cannot replace an operation that overlapped its read.
            // Missing current rows also preserve deletions until a later fresh read.
            for (const address of request.changedAddresses) {
                const current = devices.find((device) => device.address === address);
                if (current) merged.set(address, { ...current });
                else merged.delete(address);
            }
            devices = [...merged.values()];
        } catch (e) {
            if (activeDeviceFetch === request) {
                message = { type: 'error', text: 'Failed to load saved devices.' };
            }
        } finally {
            if (activeDeviceFetch === request) activeDeviceFetch = null;
        }
    }

    async function fetchSlots() {
        try {
            const res = await fetchJsonWithTimeout('/api/autopush/slots');
            if (!res.ok) return;
            const data = res.data;
            slots = data.slots || [];
        } catch (e) {
            // Best-effort only; fallback labels are used when slots cannot be loaded.
        }
    }

    function slotLabel(profileNumber) {
        if (profileNumber === 0) {
            return 'Use global active slot';
        }
        const slotIndex = profileNumber - 1;
        const slot = slots[slotIndex];
        const name = slot?.name || fallbackSlotNames[slotIndex] || `Slot ${profileNumber}`;
        return `${name} (slot ${profileNumber})`;
    }

    function profileSelectId(address) {
        return `device-profile-${address.replace(/[^a-zA-Z0-9]/g, '-')}`;
    }

    function startRename(device) {
        renameGeneration += 1;
        editingAddress = device.address;
        editName = device.name || '';
    }

    function cancelRename() {
        editingAddress = '';
        editName = '';
    }

    function beginDeviceOperation(address) {
        if (busyAddresses.includes(address)) return false;
        busyAddresses.push(address);
        activeDeviceFetch?.changedAddresses.add(address);
        return true;
    }

    function finishDeviceOperation(address) {
        activeDeviceFetch?.changedAddresses.add(address);
        busyAddresses = busyAddresses.filter((pending) => pending !== address);
    }

    async function readMutationResult(res) {
        let payload;
        try {
            payload = await res.json();
        } catch (e) {
            return 'failed';
        }
        if (!res.ok || payload?.success !== true) return 'failed';
        if (res.status === 200 && !payload.operationPending && !payload.mirrorSyncPending) {
            return 'complete';
        }
        if (res.status === 202 && payload.operationPending === true) return 'operation-pending';
        if (res.status === 202 && payload.mirrorSyncPending === true) return 'mirror-pending';
        return 'failed';
    }

    async function saveName(address) {
        if (!beginDeviceOperation(address)) return;
        const submittedDraft = editName;
        const submittedName = submittedDraft.trim();
        const submittedRenameGeneration = renameGeneration;
        try {
            const formData = new FormData();
            formData.append('address', address);
            formData.append('name', submittedName);

            const result = await fetchWithTimeout('/api/v1/devices/name', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
                body: new URLSearchParams(formData)
            }, undefined, readMutationResult);
            if (result === 'failed' || result === 'operation-pending') {
                message = { type: 'error', text: 'Failed to save device name.' };
                return;
            }

            devices = devices.map((device) =>
                device.address === address ? { ...device, name: submittedName } : device
            );
            message = result === 'mirror-pending'
                ? {
                      type: 'warning',
                      text: 'Device name saved on primary storage; mirror sync is pending.'
                  }
                : { type: 'success', text: 'Device name saved.' };
            if (editingAddress === address && renameGeneration === submittedRenameGeneration &&
                editName === submittedDraft) {
                cancelRename();
            }
        } catch (e) {
            message = { type: 'error', text: 'Failed to save device name.' };
        } finally {
            finishDeviceOperation(address);
        }
    }

    async function setDefaultProfile(address, profile) {
        if (!beginDeviceOperation(address)) return;
        try {
            const selected = Math.max(0, Math.min(3, Number(profile)));
            const formData = new FormData();
            formData.append('address', address);
            formData.append('profile', String(selected));

            const result = await fetchWithTimeout('/api/v1/devices/profile', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
                body: new URLSearchParams(formData)
            }, undefined, readMutationResult);
            if (result === 'failed' || result === 'operation-pending') {
                message = { type: 'error', text: 'Failed to save default profile.' };
                await fetchDevices();
                return;
            }

            devices = devices.map((device) =>
                device.address === address ? { ...device, defaultProfile: selected } : device
            );
            message = result === 'mirror-pending'
                ? {
                      type: 'warning',
                      text: 'Default profile saved on primary storage; mirror sync is pending.'
                  }
                : { type: 'success', text: 'Default profile updated.' };
        } catch (e) {
            message = { type: 'error', text: 'Failed to save default profile.' };
            await fetchDevices();
        } finally {
            finishDeviceOperation(address);
        }
    }

    async function deleteDevice(address) {
        if (!beginDeviceOperation(address)) return;
        try {
            const device = devices.find((d) => d.address === address);
            const displayName = device?.name || address;
            if (!confirm(`Remove ${displayName} from saved devices?`)) return;
            const formData = new FormData();
            formData.append('address', address);

            const result = await fetchWithTimeout('/api/v1/devices/delete', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
                body: new URLSearchParams(formData)
            }, undefined, readMutationResult);
            if (result === 'operation-pending') {
                message = {
                    type: 'warning',
                    text: 'Device removal is pending; recovery will continue automatically.'
                };
                return;
            }
            if (result !== 'complete') {
                message = { type: 'error', text: 'Failed to remove device.' };
                return;
            }

            devices = devices.filter((device) => device.address !== address);
            message = { type: 'success', text: 'Device removed.' };
            if (editingAddress === address) {
                cancelRename();
            }
        } catch (e) {
            message = { type: 'error', text: 'Failed to remove device.' };
        } finally {
            finishDeviceOperation(address);
        }
    }
</script>

<div class="page-stack">
    <PageHeader
        title="Saved V1 Devices"
        subtitle="Name saved V1 addresses and choose an Auto-Push slot for each."
    >
        <button class="btn btn-outline btn-sm" onclick={refresh} disabled={loading}>
            Refresh
        </button>
    </PageHeader>

    <StatusAlert {message} />

    <div class="surface-note">
        <p>
            Saved devices are discovered from real V1 connections. You can assign each device a
            friendly name and optional auto-push slot override.
        </p>
        {#if $isMaintenance}
            <p class="copy-caption">
                Live V1 connection state is hidden in maintenance mode because BLE is not running.
            </p>
        {/if}
    </div>

    {#if loading}
        <div class="state-loading">
            <span class="loading loading-lg loading-spinner"></span>
        </div>
    {:else if devices.length === 0}
        <div class="surface-card">
            <div class="card-body">
                <p class="state-empty">
                    No saved V1 devices yet. Connect to a V1 and it will appear here.
                </p>
            </div>
        </div>
    {:else}
        <div class="grid gap-4">
            {#each devices as device (device.address)}
                <div class="surface-card">
                    <div class="card-body space-y-4">
                        <div class="flex items-start justify-between gap-3">
                            <div class="space-y-1">
                                {#if editingAddress === device.address}
                                    <input
                                        type="text"
                                        class="input w-full max-w-xs input-sm"
                                        bind:value={editName}
                                        maxlength="32"
                                        onkeydown={(e) => {
                                            if (e.key === 'Enter') {
                                                saveName(device.address);
                                            }
                                            if (e.key === 'Escape') {
                                                cancelRename();
                                            }
                                        }}
                                    />
                                {:else}
                                    <h3 class="text-lg font-semibold">
                                        {device.name || 'Unnamed V1'}
                                    </h3>
                                {/if}
                                <p class="copy-caption font-mono">{device.address}</p>
                                {#if device.connected && !$isMaintenance}
                                    <span class="badge badge-sm badge-success">Connected</span>
                                {/if}
                            </div>
                            <div class="flex gap-2">
                                {#if editingAddress === device.address}
                                    <button
                                        class="btn btn-sm btn-success"
                                        onclick={() => saveName(device.address)}
                                        disabled={busyAddresses.includes(device.address)}
                                    >
                                        Save
                                    </button>
                                    <button
                                        class="btn btn-ghost btn-sm"
                                        onclick={cancelRename}
                                    >
                                        Cancel
                                    </button>
                                {:else}
                                    <button
                                        class="btn btn-ghost btn-sm"
                                        onclick={() => startRename(device)}
                                    >
                                        Rename
                                    </button>
                                    <button
                                        class="btn btn-outline btn-error btn-sm"
                                        onclick={() => deleteDevice(device.address)}
                                        disabled={busyAddresses.includes(device.address)}
                                    >
                                        Delete
                                    </button>
                                {/if}
                            </div>
                        </div>

                        <div class="space-y-2">
                            <label class="label py-0" for={profileSelectId(device.address)}>
                                <span class="field-label copy-caption"
                                    >Default Auto-Push slot</span
                                >
                            </label>
                            <select
                                id={profileSelectId(device.address)}
                                class="select w-full max-w-sm select-sm"
                                value={String(device.defaultProfile || 0)}
                                onchange={(e) => {
                                    const profile = Number(e.currentTarget.value);
                                    // Keep the selection on the confirmed value until the save completes.
                                    e.currentTarget.value = String(device.defaultProfile || 0);
                                    void setDefaultProfile(device.address, profile);
                                }}
                                disabled={busyAddresses.includes(device.address)}
                            >
                                <option value="0">Use global active slot</option>
                                {#each [1, 2, 3] as profileNumber}
                                    <option value={String(profileNumber)}
                                        >{slotLabel(profileNumber)}</option
                                    >
                                {/each}
                            </select>
                            {#if Number(device.defaultProfile || 0) > 0}
                                <p class="copy-caption">
                                    Will auto-push {slotLabel(Number(device.defaultProfile || 0))} on
                                    connect.
                                </p>
                            {/if}
                        </div>
                    </div>
                </div>
            {/each}
        </div>
    {/if}
</div>
