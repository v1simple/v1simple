<script>
    import { onMount } from 'svelte';
    import BrandMark from '#lib/components/BrandMark.svelte';
    import PageHeader from '#lib/components/PageHeader.svelte';
    import StatusAlert from '#lib/components/StatusAlert.svelte';
    import {
        retainRuntimeStatus,
        runtimeStatus,
        runtimeStatusError,
        runtimeStatusLoading,
        isMaintenance
    } from '#lib/stores/runtimeStatus.svelte.js';
    import {
        invalidateDeviceSettings,
        refreshDeviceSettings,
        retainDeviceSettings
    } from '#lib/stores/deviceSettings.svelte.js';
    import { fetchJsonWithTimeout, fetchWithTimeout } from '#lib/utils/poll.js';

    function batteryPowerSource(battery) {
        if (battery.source === 'battery') return 'On battery power';
        if (battery.source === 'usb') return 'External power';
        if (battery.source !== undefined) return 'Power source unavailable';
        return battery.on_battery ? 'On battery power' : 'External power';
    }

    let modeLoaded = $state(false);
    let modeLoading = $state(false);
    let modeSaving = $state(false);
    let modeMessage = $state(null);
    let proxyEnabled = $state(false);
    let obdEnabled = $state(false);
    let proxyName = $state('V1-Proxy');
    let savedProxyName = 'V1-Proxy';
    let selectedMode = $state('standalone');
    let pendingMode = $state('standalone');
    let proxyNameSaving = $state(false);

    onMount(() => {
        const releaseRuntimeStatus = retainRuntimeStatus();
        const releaseDeviceSettings = retainDeviceSettings();
        void loadOperatingMode();
        return () => {
            releaseRuntimeStatus();
            releaseDeviceSettings();
        };
    });

    function resolveMode(proxy, obd) {
        if (proxy) return 'proxy';
        if (obd) return 'obd';
        return 'standalone';
    }

    function modeLabel(mode) {
        if (mode === 'proxy') return 'Proxy / App';
        if (mode === 'obd') return 'OBD';
        return 'Display only';
    }

    function modeIsDirty() {
        return pendingMode !== selectedMode;
    }

    async function loadOperatingMode(preserveProxyName = false) {
        modeLoading = true;
        modeLoaded = false;
        try {
            const [deviceSettings, obdRes] = await Promise.all([
                refreshDeviceSettings(),
                fetchJsonWithTimeout('/api/obd/config')
            ]);
            if (!deviceSettings || typeof deviceSettings.proxy_ble !== 'boolean' ||
                typeof deviceSettings.proxy_name !== 'string' || !obdRes.ok ||
                typeof obdRes.data?.enabled !== 'boolean') {
                throw new Error('mode fetch failed');
            }
            const obdSettings = obdRes.data;
            proxyEnabled = !!deviceSettings.proxy_ble;
            savedProxyName = deviceSettings.proxy_name || savedProxyName;
            if (!preserveProxyName) proxyName = savedProxyName;
            obdEnabled = !!obdSettings.enabled;
            selectedMode = resolveMode(proxyEnabled, obdEnabled);
            pendingMode = selectedMode;
            modeLoaded = true;
            if (modeMessage?.text === 'Failed to load operating mode.') modeMessage = null;
        } catch (error) {
            console.error('Failed to load operating mode', error);
            modeMessage = { type: 'error', text: 'Failed to load operating mode.' };
        } finally {
            modeLoading = false;
        }
    }

    async function setProxyEnabled(enabled) {
        const formData = new FormData();
        formData.append('proxy_ble', enabled ? 'true' : 'false');
        if (proxyName) {
            formData.append('proxy_name', proxyName);
        }
        const res = await fetchWithTimeout('/api/device/settings', {
            method: 'POST',
            body: formData
        });
        if (!res.ok) {
            throw new Error('proxy save failed');
        }
        await invalidateDeviceSettings();
    }

    async function setObdEnabled(enabled) {
        const res = await fetchWithTimeout('/api/obd/config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ enabled })
        });
        if (!res.ok) {
            throw new Error('obd save failed');
        }
        // Enabling OBD also disables proxy mode in the same firmware mutation.
        await invalidateDeviceSettings();
    }

    async function setProxyName(name) {
        const formData = new FormData();
        formData.append('proxy_name', name);
        const res = await fetchWithTimeout('/api/device/settings', {
            method: 'POST',
            body: formData
        });
        if (!res.ok) throw new Error('proxy name save failed');
        savedProxyName = name;
        await invalidateDeviceSettings();
    }

    function selectOperatingMode(mode) {
        if (!modeLoaded || modeLoading || modeSaving || pendingMode === mode) return;
        pendingMode = mode;
        modeMessage = null;
    }

    async function saveOperatingMode() {
        if (!modeLoaded || modeLoading || modeSaving || !modeIsDirty()) return;

        const modeToSave = pendingMode;
        modeSaving = true;
        modeMessage = null;
        let nameSaved = false;
        try {
            if (modeToSave === 'proxy') {
                // Each firmware enable mutation also disables the competing mode.
                await setProxyEnabled(true);
            } else if (modeToSave === 'obd') {
                if (proxyName && proxyName !== savedProxyName) {
                    await setProxyName(proxyName);
                    nameSaved = true;
                }
                await setObdEnabled(true);
            } else {
                await setProxyEnabled(false);
                await setObdEnabled(false);
            }
            modeMessage = {
                type: 'success',
                text: `${modeLabel(modeToSave)} mode saved. Reboot normally to apply.`
            };
            await loadOperatingMode();
        } catch (error) {
            console.error('Failed to save operating mode', error);
            modeMessage = {
                type: 'error',
                text: nameSaved
                    ? 'Proxy name saved, but failed to save operating mode.'
                    : 'Failed to save operating mode.'
            };
            // A lost response may follow a committed write; discard older shared reads.
            await invalidateDeviceSettings();
            await loadOperatingMode(true);
        } finally {
            modeSaving = false;
        }
    }

    async function saveProxyName() {
        if (!modeLoaded || modeLoading || modeSaving || proxyNameSaving || !proxyName.trim()) return;

        proxyNameSaving = true;
        modeMessage = null;
        try {
            await setProxyName(proxyName.trim());
            proxyName = proxyName.trim();
            modeMessage = { type: 'success', text: 'Proxy name saved.' };
        } catch (error) {
            console.error('Failed to save proxy name', error);
            modeMessage = { type: 'error', text: 'Failed to save proxy name.' };
            await invalidateDeviceSettings();
            await loadOperatingMode(true);
        } finally {
            proxyNameSaving = false;
        }
    }

    function formatUptime(seconds) {
        const d = Math.floor(seconds / 86400);
        const h = Math.floor((seconds % 86400) / 3600);
        const m = Math.floor((seconds % 3600) / 60);
        if (d > 0) return `${d}d ${h}h ${m}m`;
        if (h > 0) return `${h}h ${m}m`;
        return `${m}m`;
    }
    function getRssiClass(rssi) {
        if (rssi >= -50) return 'text-success';
        if (rssi >= -70) return 'text-warning';
        return 'text-error';
    }
</script>

<div class="page-stack">
    <PageHeader title="Dashboard" subtitle="Maintenance system status and saved configuration." />

    <div class="surface-note flex flex-wrap items-center justify-between gap-3">
        <p>New to V1Simple? Learn the setup, display and controls before your first drive.</p>
        <a class="btn btn-outline btn-sm" href="/first-use.html" data-sveltekit-reload>First use guide</a>
    </div>

    {#if $runtimeStatusLoading}
        <div class="surface-hero" role="status" aria-live="polite">
            <div class="state-loading stack">
                <span class="loading loading-lg loading-spinner"></span>
                <p class="copy-subtle">Checking maintenance device status…</p>
            </div>
        </div>
    {:else if $runtimeStatusError}
        <div class="surface-alert alert-error" role="alert">
            <div>
                <h2 class="font-bold">Device status unavailable</h2>
                <p class="copy-caption">
                    The maintenance shell remains available while the next status poll retries.
                </p>
            </div>
        </div>
    {:else}
        <div class="surface-hero">
            <div class="text-center">
                <div class="mb-2 flex justify-center"><BrandMark /></div>
                <p class="copy-caption mb-1">v{$runtimeStatus.device?.firmware_version || 'Unknown'}</p>
                <p class="copy-subtle">
                    {$runtimeStatus.device?.hostname || 'Hostname unavailable'}
                    {#if $runtimeStatus.wifi.sta_connected && $runtimeStatus.wifi.sta_ip}
                        • STA {$runtimeStatus.wifi.sta_ip}
                    {:else if $runtimeStatus.wifi.ap_active && $runtimeStatus.wifi.ap_ip}
                        • AP {$runtimeStatus.wifi.ap_ip}
                    {/if}
                </p>
                {#if !$isMaintenance}
                    <p class="copy-warning mt-2">
                        Maintenance status is not confirmed. Runtime-only actions stay unavailable.
                    </p>
                {/if}
            </div>
        </div>

        <div class="grid grid-cols-1 gap-4 sm:grid-cols-2 lg:grid-cols-3">
            <div class="surface-card">
                <div class="card-body">
                    <div class="copy-mini-title">WiFi</div>
                    <div class="status-heading {$runtimeStatus.wifi.sta_connected ? 'status-heading-success' : 'status-heading-info'}">
                        {$runtimeStatus.wifi.sta_connected
                            ? 'STA connected'
                            : $runtimeStatus.wifi.ap_active
                              ? 'Access point active'
                              : 'Status unavailable'}
                    </div>
                    <div class="copy-caption {getRssiClass($runtimeStatus.wifi.rssi)}">
                        {#if $runtimeStatus.wifi.sta_connected}
                            {$runtimeStatus.wifi.ssid || 'STA'}
                            {#if $runtimeStatus.wifi.rssi}• {$runtimeStatus.wifi.rssi} dBm{/if}
                        {:else if $runtimeStatus.wifi.ap_active}
                            {$runtimeStatus.wifi.ssid || 'Maintenance access point'}
                        {/if}
                    </div>
                </div>
            </div>

            <div class="surface-card">
                <div class="card-body">
                    <div class="copy-mini-title">Device</div>
                    <div class="status-heading">{formatUptime($runtimeStatus.device?.uptime || 0)}</div>
                    <div class="copy-caption">
                        {Math.round(($runtimeStatus.device?.heap_free || 0) / 1024)} KB heap free
                    </div>
                </div>
            </div>

            <div class="surface-card">
                <div class="card-body">
                    <div class="copy-mini-title">Battery</div>
                    {#if $runtimeStatus.battery?.has_battery}
                        <div class="status-heading">{Math.round($runtimeStatus.battery.percentage || 0)}%</div>
                        <div class="copy-caption">
                            {batteryPowerSource($runtimeStatus.battery)}
                            {#if $runtimeStatus.battery.voltage_mv}• {$runtimeStatus.battery.voltage_mv} mV{/if}
                        </div>
                    {:else}
                        <div class="status-heading-muted">Not installed</div>
                        <div class="copy-caption">No battery reported by this hardware.</div>
                    {/if}
                </div>
            </div>
        </div>
    {/if}

    <div class="surface-card">
        <div class="card-body gap-4">
            <div class="flex flex-col gap-2 md:flex-row md:items-start md:justify-between">
                <div>
                    <div class="copy-mini-title">Operating mode</div>
                    <h2 class="card-title">Choose who controls V1 features</h2>
                    <p class="copy-subtle">
                        OBD and proxy are mutually exclusive to keep V1 BLE stable. Display and
                        Wi-Fi stay the same in both modes.
                    </p>
                </div>
                {#if modeLoading || modeSaving}
                    <span
                        class="loading loading-sm loading-spinner"
                        aria-label="Loading operating mode"
                    ></span>
                {:else if modeLoaded}
                    <span class="badge {modeIsDirty() ? 'badge-warning' : 'badge-outline'}">
                        {modeIsDirty()
                            ? `${modeLabel(pendingMode)} pending save`
                            : `${modeLabel(selectedMode)} saved`}
                    </span>
                {/if}
            </div>

            {#if !modeLoaded && !modeLoading}
                <button class="btn btn-outline self-start" disabled={modeSaving || proxyNameSaving}
                    onclick={() => loadOperatingMode()}>Retry operating mode</button>
            {/if}

            <div class="grid grid-cols-1 gap-3 lg:grid-cols-3">
                <button
                    type="button"
                    class="surface-panel text-left transition hover:border-primary"
                    class:border-primary={pendingMode === 'standalone'}
                    class:bg-base-200={pendingMode === 'standalone'}
                    aria-pressed={pendingMode === 'standalone'}
                    onclick={() => selectOperatingMode('standalone')}
                    disabled={!modeLoaded || modeLoading || modeSaving}
                >
                    <div class="flex items-center justify-between gap-2">
                        <div class="font-semibold">Display only</div>
                        <span
                            class="badge {pendingMode === 'standalone'
                                ? 'badge-primary'
                                : 'badge-outline'}"
                        >
                            {pendingMode === 'standalone'
                                ? modeIsDirty()
                                    ? 'Pending'
                                    : 'Selected'
                                : 'Choose'}
                        </span>
                    </div>
                    <p class="copy-subtle mt-2">
                        TL;DR: V1 Simple owns the local display. OBD speed and phone proxy are off.
                    </p>
                </button>

                <button
                    type="button"
                    class="surface-panel text-left transition hover:border-primary"
                    class:border-primary={pendingMode === 'obd'}
                    class:bg-base-200={pendingMode === 'obd'}
                    aria-pressed={pendingMode === 'obd'}
                    onclick={() => selectOperatingMode('obd')}
                    disabled={!modeLoaded || modeLoading || modeSaving}
                >
                    <div class="flex items-center justify-between gap-2">
                        <div class="font-semibold">OBD</div>
                        <span
                            class="badge {pendingMode === 'obd'
                                ? 'badge-primary'
                                : 'badge-outline'}"
                        >
                            {pendingMode === 'obd'
                                ? modeIsDirty()
                                    ? 'Pending'
                                    : 'Selected'
                                : 'Choose'}
                        </span>
                    </div>
                    <p class="copy-subtle mt-2">
                        TL;DR: V1 Simple owns local speed mute, quiet controls, and V1 writes. OBD
                        speed is used when configured; phone proxy is off.
                    </p>
                </button>

                <button
                    type="button"
                    class="surface-panel text-left transition hover:border-primary"
                    class:border-primary={pendingMode === 'proxy'}
                    class:bg-base-200={pendingMode === 'proxy'}
                    aria-pressed={pendingMode === 'proxy'}
                    onclick={() => selectOperatingMode('proxy')}
                    disabled={!modeLoaded || modeLoading || modeSaving}
                >
                    <div class="flex items-center justify-between gap-2">
                        <div class="font-semibold">Proxy / App</div>
                        <span
                            class="badge {pendingMode === 'proxy'
                                ? 'badge-primary'
                                : 'badge-outline'}"
                        >
                            {pendingMode === 'proxy'
                                ? modeIsDirty()
                                    ? 'Pending'
                                    : 'Selected'
                                : 'Choose'}
                        </span>
                    </div>
                    <p class="copy-subtle mt-2">
                        TL;DR: the phone app is trusted to manage muting. V1 Simple relays raw V1
                        data; OBD is off, and local V1 writes stop while the app is connected.
                    </p>
                </button>
            </div>
            <div class="flex flex-col gap-2 sm:flex-row sm:items-center">
                <button
                    type="button"
                    class="btn btn-primary"
                    onclick={saveOperatingMode}
                    disabled={!modeLoaded || modeLoading || modeSaving || !modeIsDirty()}
                >
                    {#if modeSaving}
                        <span class="loading loading-sm loading-spinner"></span>
                    {/if}
                    Save operating mode
                </button>
                <p class="copy-caption">
                    {#if modeIsDirty()}
                        {modeLabel(pendingMode)} is selected but not saved yet.
                    {:else}
                        Saved mode applies on next normal boot; no V1 proxy or OBD runtime is active
                        while maintenance mode is open.
                    {/if}
                </p>
            </div>

            <div class="surface-panel space-y-3">
                <div>
                    <div class="font-semibold">Proxy / App advertising name</div>
                    <p class="copy-caption">
                        BLE name shown to phone apps. It is saved with the mode settings and applies
                        on next normal boot.
                    </p>
                </div>
                <div class="flex flex-col gap-2 sm:flex-row">
                    <label class="sr-only" for="dashboard-proxy-name">Proxy Name</label>
                    <input
                        id="dashboard-proxy-name"
                        type="text"
                        class="input flex-1"
                        bind:value={proxyName}
                        placeholder="V1C-LE-S3"
                        disabled={!modeLoaded || modeLoading || modeSaving || proxyNameSaving}
                    />
                    <button
                        type="button"
                        class="btn btn-outline"
                        onclick={saveProxyName}
                        disabled={!modeLoaded || modeLoading || modeSaving || proxyNameSaving || !proxyName.trim()}
                    >
                        {#if proxyNameSaving}
                            <span class="loading loading-sm loading-spinner"></span>
                        {/if}
                        Save proxy name
                    </button>
                </div>
            </div>

            <StatusAlert
                message={modeMessage}
                fallbackType="info"
                dismiss={() => (modeMessage = null)}
            />
        </div>
    </div>

</div>
