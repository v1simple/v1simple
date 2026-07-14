<script>
    import { onMount, onDestroy } from 'svelte';
    import { createPoll, fetchWithTimeout } from '$lib/utils/poll';
    import CardSectionHead from '$lib/components/CardSectionHead.svelte';
    import StatusAlert from '$lib/components/StatusAlert.svelte';
    import { isMaintenance, retainRuntimeStatus } from '$lib/stores/runtimeStatus.svelte.js';

    const STATE_NAMES = {
        0: 'Idle',
        1: 'WaitBoot',
        2: 'Scanning',
        4: 'Connecting',
        5: 'Securing',
        6: 'Discovering',
        7: 'ATInit',
        8: 'Polling',
        9: 'ErrorBackoff',
        10: 'Disconnected',
        11: 'EcuIdle'
    };

    let obdStatus = $state(null);
    let obdMessage = $state(null);
    let savedDevices = $state([]);
    let enabled = $state(false);
    let minRssi = $state(-80);
    let obdScanWindowMs = $state(15000);
    let obdRetryIntervalMs = $state(120000);
    let proxyOpenWindowMs = $state(60000);
    let wifiOpenTimeoutMs = $state(30000);
    let v1SettleQuietMs = $state(500);
    let v1SettleFallbackMs = $state(1500);
    let cycleTeardownAckTimeoutMs = $state(100);
    let saving = $state(false);
    let scanning = $state(false);
    let forgetting = $state(false);
    let renaming = $state(false);
    let statusFetchInFlight = false;
    let loaded = $state(false);
    let editingAddress = $state('');
    let editName = $state('');
    let lastSavedAddressSeen = '';

    const statusPoll = createPoll(async () => {
        await fetchObdStatus();
    }, 2000);

    onMount(() => {
        const releaseRuntimeStatus = retainRuntimeStatus({ needsStatus: true });
        void (async () => {
            await fetchObdStatus();
            if (obdStatus) {
                enabled = obdStatus.enabled;
                lastSavedAddressSeen = obdStatus.savedAddress || '';
            }
            await fetchObdConfig({ showLoadError: true }).catch(() => null);
            await fetchObdDevices({ showLoadError: true }).catch(() => null);
            loaded = true;
            syncStatusPollToEnabled();
        })();
        return releaseRuntimeStatus;
    });

    onDestroy(() => {
        statusPoll.stop();
    });

    $effect(() => {
        if (loaded) {
            syncStatusPollToEnabled();
        }
    });

    function syncStatusPollToEnabled() {
        if (enabled && !$isMaintenance) {
            statusPoll.start();
            return;
        }
        statusPoll.stop();
    }

    async function fetchObdConfig({ showLoadError = false } = {}) {
        try {
            const res = await fetchWithTimeout('/api/obd/config');
            if (!res.ok) {
                throw new Error(`OBD config request failed with status ${res.status}`);
            }
            const data = await res.json();
            if (typeof data.minRssi === 'number') minRssi = data.minRssi;
            if (typeof data.enabled === 'boolean') enabled = data.enabled;
            if (typeof data.obdScanWindowMs === 'number') obdScanWindowMs = data.obdScanWindowMs;
            if (typeof data.obdRetryIntervalMs === 'number')
                obdRetryIntervalMs = data.obdRetryIntervalMs;
            if (typeof data.proxyOpenWindowMs === 'number')
                proxyOpenWindowMs = data.proxyOpenWindowMs;
            if (typeof data.wifiOpenTimeoutMs === 'number')
                wifiOpenTimeoutMs = data.wifiOpenTimeoutMs;
            if (typeof data.v1SettleQuietMs === 'number') v1SettleQuietMs = data.v1SettleQuietMs;
            if (typeof data.v1SettleFallbackMs === 'number')
                v1SettleFallbackMs = data.v1SettleFallbackMs;
            if (typeof data.cycleTeardownAckTimeoutMs === 'number') {
                cycleTeardownAckTimeoutMs = data.cycleTeardownAckTimeoutMs;
            }
            return data;
        } catch (error) {
            if (showLoadError) {
                console.error('Failed to load OBD settings', error);
                obdMessage = { type: 'error', text: 'Failed to load OBD settings.' };
            }
            throw error;
        }
    }

    async function fetchObdDevices({ showLoadError = false } = {}) {
        try {
            const res = await fetchWithTimeout('/api/obd/devices');
            if (!res.ok) {
                throw new Error(`OBD devices request failed with status ${res.status}`);
            }
            const data = await res.json();
            savedDevices = (data.devices || []).map((device) => ({
                address: device.address || '',
                name: device.name || '',
                connected: !!device.connected,
                active: !!device.active
            }));
            return data;
        } catch (error) {
            if (showLoadError) {
                console.error('Failed to load saved OBD devices', error);
                obdMessage = { type: 'error', text: 'Failed to load saved OBD devices.' };
            }
            throw error;
        }
    }

    async function reconcileObdUiStateAfterSaveFailure() {
        try {
            await fetchObdConfig();
            if (enabled) {
                syncStatusPollToEnabled();
                await fetchObdStatus();
            } else {
                syncStatusPollToEnabled();
                obdStatus = null;
            }
        } catch (error) {
            console.warn('Failed to reconcile OBD settings after save failure', error);
        }
    }

    async function saveConfig(fields) {
        saving = true;
        obdMessage = null;
        try {
            const res = await fetchWithTimeout('/api/obd/config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(fields)
            });
            if (!res.ok) {
                obdMessage = { type: 'error', text: 'Failed to save OBD setting.' };
                await reconcileObdUiStateAfterSaveFailure();
                return false;
            }
            return true;
        } catch (_) {
            obdMessage = { type: 'error', text: 'Connection error.' };
            await reconcileObdUiStateAfterSaveFailure();
            return false;
        } finally {
            saving = false;
        }
    }

    async function handleToggle() {
        const saved = await saveConfig({ enabled });
        if (!saved) {
            return;
        }
        if (enabled) {
            syncStatusPollToEnabled();
            await fetchObdStatus();
        } else {
            syncStatusPollToEnabled();
            obdStatus = null;
        }
    }

    async function handleMinRssiChange() {
        await saveConfig({ minRssi });
    }

    async function handleTimingChange(field, value) {
        const parsed = Number(value);
        if (!Number.isFinite(parsed)) {
            return;
        }
        await saveConfig({ [field]: Math.trunc(parsed) });
    }

    async function fetchObdStatus() {
        if (statusFetchInFlight) return;
        statusFetchInFlight = true;
        try {
            const res = await fetchWithTimeout('/api/obd/status');
            if (res.ok) {
                const nextStatus = await res.json();
                obdStatus = nextStatus;
                const nextSavedAddress = nextStatus.savedAddress || '';
                if (nextSavedAddress !== lastSavedAddressSeen) {
                    lastSavedAddressSeen = nextSavedAddress;
                    await fetchObdDevices().catch(() => null);
                }
            }
        } catch (error) {
            console.warn('Failed to poll OBD status', error);
        } finally {
            statusFetchInFlight = false;
        }
    }

    async function startScan() {
        if ($isMaintenance) {
            obdMessage = {
                type: 'info',
                text: 'OBD scanning requires normal runtime; it is not available in maintenance mode.'
            };
            return;
        }

        scanning = true;
        obdMessage = null;
        try {
            const res = await fetchWithTimeout('/api/obd/scan', { method: 'POST' });
            if (res.ok) {
                obdMessage = { type: 'success', text: 'Scan started.' };
            } else {
                obdMessage = { type: 'error', text: 'Failed to start scan.' };
            }
        } catch (_) {
            obdMessage = { type: 'error', text: 'Connection error.' };
        } finally {
            scanning = false;
        }
    }

    async function forgetDevice() {
        forgetting = true;
        obdMessage = null;
        try {
            const res = await fetchWithTimeout('/api/obd/forget', { method: 'POST' });
            if (res.ok) {
                obdMessage = { type: 'success', text: 'Device forgotten.' };
                obdStatus = null;
                savedDevices = [];
                lastSavedAddressSeen = '';
                cancelRename();
            } else {
                obdMessage = { type: 'error', text: 'Failed to forget device.' };
            }
        } catch (_) {
            obdMessage = { type: 'error', text: 'Connection error.' };
        } finally {
            forgetting = false;
        }
    }

    function stateName(s) {
        return STATE_NAMES[s] ?? `Unknown(${s})`;
    }

    function startRename(device) {
        editingAddress = device.address;
        editName = device.name || '';
    }

    function cancelRename() {
        editingAddress = '';
        editName = '';
    }

    async function saveDeviceName(address) {
        renaming = true;
        obdMessage = null;
        try {
            const formData = new FormData();
            formData.append('address', address);
            formData.append('name', editName.trim());

            const res = await fetchWithTimeout('/api/obd/devices/name', {
                method: 'POST',
                body: formData
            });
            if (!res.ok) {
                obdMessage = { type: 'error', text: 'Failed to save OBD device name.' };
                return;
            }

            savedDevices = savedDevices.map((device) =>
                device.address === address ? { ...device, name: editName.trim() } : device
            );
            obdMessage = { type: 'success', text: 'OBD device name saved.' };
            cancelRename();
        } catch (_) {
            obdMessage = { type: 'error', text: 'Failed to save OBD device name.' };
        } finally {
            renaming = false;
        }
    }
</script>

<div class="surface-card">
    <div class="card-body space-y-4">
        <CardSectionHead
            title="OBD-II Speed Source"
            subtitle="Connect an OBDLink CX for vehicle speed data. Enabling OBD disables proxy mode."
        />

        {#if loaded}
            <label class="label cursor-pointer">
                <span class="field-label">Enable OBD</span>
                <input
                    type="checkbox"
                    class="toggle toggle-primary"
                    bind:checked={enabled}
                    onchange={handleToggle}
                    disabled={saving}
                />
            </label>

            {#if obdMessage}
                <StatusAlert message={obdMessage} />
            {/if}

            {#if enabled}
                <div class="field-control">
                    <label class="label" for="obd-min-rssi">
                        <span class="field-label">Min RSSI (dBm)</span>
                    </label>
                    <input
                        id="obd-min-rssi"
                        type="number"
                        class="input w-24"
                        bind:value={minRssi}
                        min="-90"
                        max="-40"
                        placeholder="-80"
                        onchange={handleMinRssiChange}
                    />
                </div>

                <div class="surface-note space-y-3">
                    <div class="copy-caption font-semibold uppercase tracking-wide">
                        Connection Cycle
                    </div>
                    <div class="grid gap-3 md:grid-cols-2">
                        <div class="field-control">
                            <label class="label" for="obd-scan-window-ms">
                                <span class="field-label">OBD Scan Window (ms)</span>
                            </label>
                            <input
                                id="obd-scan-window-ms"
                                type="number"
                                class="input w-full"
                                bind:value={obdScanWindowMs}
                                min="1000"
                                max="60000"
                                disabled={saving}
                                onchange={() =>
                                    handleTimingChange('obdScanWindowMs', obdScanWindowMs)}
                            />
                        </div>

                        <div class="field-control">
                            <label class="label" for="obd-retry-interval-ms">
                                <span class="field-label">OBD Retry Interval (ms)</span>
                            </label>
                            <input
                                id="obd-retry-interval-ms"
                                type="number"
                                class="input w-full"
                                bind:value={obdRetryIntervalMs}
                                min="30000"
                                max="600000"
                                disabled={saving}
                                onchange={() =>
                                    handleTimingChange('obdRetryIntervalMs', obdRetryIntervalMs)}
                            />
                        </div>

                        <div class="field-control">
                            <label class="label" for="proxy-open-window-ms">
                                <span class="field-label">Proxy Open Window (ms)</span>
                            </label>
                            <input
                                id="proxy-open-window-ms"
                                type="number"
                                class="input w-full"
                                bind:value={proxyOpenWindowMs}
                                min="1000"
                                max="300000"
                                disabled={saving}
                                onchange={() =>
                                    handleTimingChange('proxyOpenWindowMs', proxyOpenWindowMs)}
                            />
                        </div>

                        <div class="field-control">
                            <label class="label" for="wifi-open-timeout-ms">
                                <span class="field-label">WiFi Open Timeout (ms)</span>
                            </label>
                            <input
                                id="wifi-open-timeout-ms"
                                type="number"
                                class="input w-full"
                                bind:value={wifiOpenTimeoutMs}
                                min="1000"
                                max="120000"
                                disabled={saving}
                                onchange={() =>
                                    handleTimingChange('wifiOpenTimeoutMs', wifiOpenTimeoutMs)}
                            />
                        </div>

                        <div class="field-control">
                            <label class="label" for="v1-settle-quiet-ms">
                                <span class="field-label">V1 Settle Quiet (ms)</span>
                            </label>
                            <input
                                id="v1-settle-quiet-ms"
                                type="number"
                                class="input w-full"
                                bind:value={v1SettleQuietMs}
                                min="100"
                                max="5000"
                                disabled={saving}
                                onchange={() =>
                                    handleTimingChange('v1SettleQuietMs', v1SettleQuietMs)}
                            />
                        </div>

                        <div class="field-control">
                            <label class="label" for="v1-settle-fallback-ms">
                                <span class="field-label">V1 Settle Fallback (ms)</span>
                            </label>
                            <input
                                id="v1-settle-fallback-ms"
                                type="number"
                                class="input w-full"
                                bind:value={v1SettleFallbackMs}
                                min="500"
                                max="10000"
                                disabled={saving}
                                onchange={() =>
                                    handleTimingChange('v1SettleFallbackMs', v1SettleFallbackMs)}
                            />
                        </div>

                        <div class="field-control">
                            <label class="label" for="cycle-teardown-ack-timeout-ms">
                                <span class="field-label">Teardown Ack Timeout (ms)</span>
                            </label>
                            <input
                                id="cycle-teardown-ack-timeout-ms"
                                type="number"
                                class="input w-full"
                                bind:value={cycleTeardownAckTimeoutMs}
                                min="25"
                                max="1000"
                                disabled={saving}
                                onchange={() =>
                                    handleTimingChange(
                                        'cycleTeardownAckTimeoutMs',
                                        cycleTeardownAckTimeoutMs
                                    )}
                            />
                        </div>
                    </div>
                </div>

                {#if $isMaintenance}
                    <div class="surface-note copy-muted text-sm">
                        OBD live state is not running in maintenance mode. Settings and saved
                        adapters remain editable and apply on next normal boot.
                    </div>
                {:else if obdStatus}
                    <div class="surface-note copy-muted text-sm space-y-1">
                        <div><strong>State:</strong> {stateName(obdStatus.state)}</div>
                        {#if obdStatus.connected}
                            <div>
                                <strong>Speed:</strong>
                                {obdStatus.speedValid
                                    ? `${obdStatus.speedMph.toFixed(1)} mph`
                                    : 'N/A'}
                            </div>
                            <div><strong>RSSI:</strong> {obdStatus.rssi} dBm</div>
                        {/if}
                        {#if obdStatus.savedAddressValid}
                            <div><strong>Saved device:</strong> Yes</div>
                        {/if}
                        {#if obdStatus.pollCount > 0}
                            <div>
                                <strong>Polls:</strong>
                                {obdStatus.pollCount} ({obdStatus.pollErrors} errors)
                            </div>
                        {/if}
                    </div>
                {/if}

                <div class="flex gap-2">
                    <button
                        class="btn btn-primary btn-sm"
                        onclick={startScan}
                        disabled={scanning || obdStatus?.scanInProgress || $isMaintenance}
                        title={$isMaintenance ? 'Scanning requires normal runtime.' : ''}
                    >
                        {#if scanning || obdStatus?.scanInProgress}
                            <span class="loading loading-spinner loading-xs"></span>
                        {/if}
                        Scan Now
                    </button>
                    <button
                        class="btn btn-error btn-outline btn-sm"
                        onclick={forgetDevice}
                        disabled={forgetting || savedDevices.length === 0}
                    >
                        Forget Device
                    </button>
                </div>
            {/if}

            <div class="space-y-2">
                <div class="copy-caption font-semibold uppercase tracking-wide">
                    Saved OBD Devices
                </div>
                {#if savedDevices.length === 0}
                    <p class="copy-caption">No saved OBD adapters yet.</p>
                {:else}
                    <div class="grid gap-3">
                        {#each savedDevices as device (device.address)}
                            <div class="surface-note space-y-2">
                                <div class="flex items-start justify-between gap-3">
                                    <div class="space-y-1">
                                        {#if editingAddress === device.address}
                                            <input
                                                type="text"
                                                class="input input-sm w-full max-w-xs"
                                                bind:value={editName}
                                                maxlength="32"
                                                onkeydown={(e) => {
                                                    if (e.key === 'Enter')
                                                        saveDeviceName(device.address);
                                                    if (e.key === 'Escape') cancelRename();
                                                }}
                                            />
                                        {:else}
                                            <div class="font-medium">
                                                {device.name || 'Unnamed OBD adapter'}
                                            </div>
                                        {/if}
                                        <div class="copy-caption font-mono">{device.address}</div>
                                        <div class="flex gap-2">
                                            {#if device.active}
                                                <span class="badge badge-outline badge-sm"
                                                    >Saved</span
                                                >
                                            {/if}
                                            {#if device.connected && !$isMaintenance}
                                                <span class="badge badge-success badge-sm"
                                                    >Connected</span
                                                >
                                            {/if}
                                        </div>
                                    </div>
                                    <div class="flex gap-2">
                                        {#if editingAddress === device.address}
                                            <button
                                                class="btn btn-success btn-sm"
                                                onclick={() => saveDeviceName(device.address)}
                                                disabled={renaming}
                                            >
                                                Save
                                            </button>
                                            <button
                                                class="btn btn-ghost btn-sm"
                                                onclick={cancelRename}
                                                disabled={renaming}
                                            >
                                                Cancel
                                            </button>
                                        {:else}
                                            <button
                                                class="btn btn-ghost btn-sm"
                                                onclick={() => startRename(device)}
                                                disabled={renaming}
                                            >
                                                Rename
                                            </button>
                                        {/if}
                                    </div>
                                </div>
                            </div>
                        {/each}
                    </div>
                {/if}
            </div>
        {/if}
    </div>
</div>
