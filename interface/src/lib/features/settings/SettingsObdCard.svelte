<script>
    import { onMount } from 'svelte';
    import { fetchJsonWithTimeout, fetchWithTimeout } from '#lib/utils/poll.js';
    import { createImmediateConfig } from './immediateConfig.svelte.js';
    import CardSectionHead from '#lib/components/CardSectionHead.svelte';
    import StatusAlert from '#lib/components/StatusAlert.svelte';

    let savedDevices = $state([]);
    let enabled = $state(false);
    let minRssi = $state(-80);
    let obdScanWindowMs = $state(15000);
    let obdRetryIntervalMs = $state(120000);
    let proxyOpenWindowMs = $state(60000);
    let v1SettleQuietMs = $state(500);
    let v1SettleFallbackMs = $state(1500);
    let cycleTeardownAckTimeoutMs = $state(100);
    let forgetting = $state(false);
    let renaming = $state(false);
    let devicesLoaded = $state(false);
    let loadingDevices = $state(false);
    let devicesMessage = $state(null);
    let editingAddress = $state('');
    let editName = $state('');
    const config = createImmediateConfig({
        name: 'OBD',
        async read() {
            const response = await fetchJsonWithTimeout('/api/obd/config');
            if (!response.ok) throw new Error(`OBD config request failed with status ${response.status}`);
            return response.data;
        },
        async write(fields) {
            return (await fetchWithTimeout('/api/obd/config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(fields)
            })).ok;
        },
        validateAndApply: applyConfig
    });

    onMount(() => {
        void config.load();
        void retryDevices();
        return config.dispose;
    });

    function applyConfig(data) {
        const ranges = [
            ['minRssi', -100, -40], ['obdScanWindowMs', 1000, 60000],
            ['obdRetryIntervalMs', 30000, 600000], ['proxyOpenWindowMs', 1000, 300000],
            ['v1SettleQuietMs', 100, 5000], ['v1SettleFallbackMs', 500, 10000],
            ['cycleTeardownAckTimeoutMs', 25, 1000]
        ];
        if (typeof data?.enabled !== 'boolean' || ranges.some(([key, min, max]) =>
            !Number.isInteger(data?.[key]) || data[key] < min || data[key] > max)) {
            throw new Error('Incomplete OBD settings');
        }
        enabled = data.enabled;
        minRssi = data.minRssi;
        obdScanWindowMs = data.obdScanWindowMs;
        obdRetryIntervalMs = data.obdRetryIntervalMs;
        proxyOpenWindowMs = data.proxyOpenWindowMs;
        v1SettleQuietMs = data.v1SettleQuietMs;
        v1SettleFallbackMs = data.v1SettleFallbackMs;
        cycleTeardownAckTimeoutMs = data.cycleTeardownAckTimeoutMs;
    }

    async function fetchObdDevices() {
        try {
            const res = await fetchJsonWithTimeout('/api/obd/devices');
            if (!res.ok) throw new Error(`OBD devices request failed with status ${res.status}`);
            const data = res.data;
            if (!Array.isArray(data?.devices) || data.devices.some((device) =>
                typeof device?.address !== 'string' || !device.address ||
                typeof device.name !== 'string' || typeof device.active !== 'boolean')) {
                throw new Error('Incomplete OBD device list');
            }
            savedDevices = data.devices.map(({ address, name, active }) => ({ address, name, active }));
            devicesLoaded = true;
            return true;
        } catch (error) {
            devicesLoaded = false;
            console.error('Failed to load saved OBD devices', error);
            devicesMessage = { type: 'error', text: 'Failed to load saved OBD devices.' };
            return false;
        }
    }

    async function retryDevices() {
        if (loadingDevices || forgetting || renaming) return;
        loadingDevices = true;
        devicesMessage = null;
        await fetchObdDevices();
        loadingDevices = false;
    }

    async function handleToggle() {
        await config.save({ enabled });
    }

    async function handleMinRssiChange() {
        await config.save({ minRssi });
    }

    async function handleTimingChange(field, value) {
        const parsed = Number(value);
        if (!Number.isFinite(parsed)) {
            return;
        }
        await config.save({ [field]: Math.trunc(parsed) });
    }

    async function forgetDevice() {
        if (!devicesLoaded || loadingDevices || forgetting || renaming) return;
        forgetting = true;
        devicesMessage = null;
        let saved = false;
        try {
            const res = await fetchWithTimeout('/api/obd/forget', { method: 'POST' });
            saved = res.ok;
            if (!saved) devicesMessage = { type: 'error', text: 'Failed to forget device.' };
        } catch (_) {
            devicesMessage = { type: 'error', text: 'Connection error.' };
        }
        const refreshed = await fetchObdDevices();
        if (saved && refreshed) {
            cancelRename();
            devicesMessage = savedDevices.length === 0
                ? { type: 'success', text: 'Device forgotten.' }
                : { type: 'error', text: 'The saved device could not be cleared.' };
        }
        forgetting = false;
    }

    function startRename(device) {
        if (!devicesLoaded || loadingDevices || forgetting || renaming) return;
        editingAddress = device.address;
        editName = device.name || '';
    }

    function cancelRename() {
        editingAddress = '';
        editName = '';
    }

    async function saveDeviceName(address) {
        if (!devicesLoaded || loadingDevices || forgetting || renaming) return;

        const submittedDraft = editName;
        const submittedName = submittedDraft.trim();
        renaming = true;
        devicesMessage = null;
        let saved = false;
        try {
            const formData = new FormData();
            formData.append('address', address);
            formData.append('name', submittedName);
            const res = await fetchWithTimeout('/api/obd/devices/name', {
                method: 'POST',
                body: formData
            });
            saved = res.ok;
            if (!saved) devicesMessage = { type: 'error', text: 'Failed to save OBD device name.' };
        } catch (_) {
            devicesMessage = { type: 'error', text: 'Failed to save OBD device name.' };
        }
        const refreshed = await fetchObdDevices();
        if (saved && refreshed) {
            devicesMessage = { type: 'success', text: 'OBD device name saved.' };
            if (editingAddress === address && editName === submittedDraft) cancelRename();
        }
        renaming = false;
    }
</script>

<div class="surface-card">
    <div class="card-body space-y-4">
        <CardSectionHead
            title="OBD-II Speed Source"
            subtitle="Connect an OBDLink CX for vehicle speed data. Enabling OBD disables proxy mode."
        />

        <StatusAlert message={config.message} />
        {#if !config.loaded}
            {#if config.loading}
                <p class="copy-caption">Loading OBD settings…</p>
            {:else}
                <button class="btn btn-outline btn-sm" onclick={config.load}>Retry OBD settings</button>
            {/if}
        {/if}

        {#if config.loaded}
            <label class="label cursor-pointer">
                <span class="field-label">Enable OBD</span>
                <input
                    type="checkbox"
                    class="toggle toggle-primary"
                    bind:checked={enabled}
                    onchange={handleToggle}
                    disabled={config.saving}
                />
            </label>

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
                        min="-100"
                        max="-40"
                        placeholder="-80"
                        disabled={config.saving}
                        onchange={handleMinRssiChange}
                    />
                </div>

                <div class="surface-note space-y-3">
                    <div class="copy-caption font-semibold tracking-wide uppercase">
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
                                disabled={config.saving}
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
                                disabled={config.saving}
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
                                disabled={config.saving}
                                onchange={() =>
                                    handleTimingChange('proxyOpenWindowMs', proxyOpenWindowMs)}
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
                                disabled={config.saving}
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
                                disabled={config.saving}
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
                                disabled={config.saving}
                                onchange={() =>
                                    handleTimingChange(
                                        'cycleTeardownAckTimeoutMs',
                                        cycleTeardownAckTimeoutMs
                                    )}
                            />
                        </div>
                    </div>
                </div>
            {/if}

            <div class="surface-note copy-muted space-y-1 text-sm">
                <p>
                    These settings apply after exiting maintenance and returning to normal
                    operation. A saved adapter reconnects automatically.
                </p>
                <p>
                    To pair a new OBDLink CX, enable OBD, return to normal operation, then hold and
                    release BOOT for about 10 seconds while OBD is disconnected.
                </p>
            </div>

        {/if}

        <StatusAlert message={devicesMessage} />
        {#if !devicesLoaded}
            {#if loadingDevices}
                <p class="copy-caption">Loading saved OBD devices…</p>
            {:else}
                <button class="btn btn-outline btn-sm" onclick={retryDevices}>Retry saved OBD devices</button>
            {/if}
        {:else}
            <div class="flex gap-2">
                <button
                    class="btn btn-outline btn-error btn-sm"
                    onclick={forgetDevice}
                    disabled={forgetting || renaming || savedDevices.length === 0}
                >
                    Forget Device
                </button>
            </div>

            <div class="space-y-2">
                <div class="copy-caption font-semibold tracking-wide uppercase">
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
                                                class="input w-full max-w-xs input-sm"
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
                                        </div>
                                    </div>
                                    <div class="flex gap-2">
                                        {#if editingAddress === device.address}
                                            <button
                                                class="btn btn-sm btn-success"
                                                onclick={() => saveDeviceName(device.address)}
                                                disabled={renaming || forgetting}
                                            >
                                                Save
                                            </button>
                                            <button
                                                class="btn btn-ghost btn-sm"
                                                onclick={cancelRename}
                                                disabled={renaming || forgetting}
                                            >
                                                Cancel
                                            </button>
                                        {:else}
                                            <button
                                                class="btn btn-ghost btn-sm"
                                                onclick={() => startRename(device)}
                                                disabled={renaming || forgetting}
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
