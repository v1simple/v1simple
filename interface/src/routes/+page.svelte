<script>
	import { onMount } from 'svelte';
	import BrandMark from '$lib/components/BrandMark.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	import {
		retainRuntimeStatus,
		runtimeStatus,
		runtimeStatusError,
		runtimeStatusLoading,
		isMaintenance
	} from '$lib/stores/runtimeStatus.svelte.js';
	import { invalidateDeviceSettings, refreshDeviceSettings, retainDeviceSettings } from '$lib/stores/deviceSettings.svelte.js';
	import { fetchWithTimeout } from '$lib/utils/poll';

	let modeLoaded = false;
	let modeLoading = false;
	let modeSaving = false;
	let modeMessage = null;
	let proxyEnabled = false;
	let obdEnabled = false;
	let proxyName = 'V1-Proxy';
	let selectedMode = 'standalone';
	let pendingMode = 'standalone';
	let proxyNameSaving = false;

	onMount(() => {
		const releaseRuntimeStatus = retainRuntimeStatus({ needsStatus: true });
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

	async function loadOperatingMode() {
		modeLoading = true;
		try {
			const [deviceSettings, obdRes] = await Promise.all([
				refreshDeviceSettings(),
				fetchWithTimeout('/api/obd/config')
			]);
			if (!deviceSettings || !obdRes.ok) {
				throw new Error('mode fetch failed');
			}
			const obdSettings = await obdRes.json();
			proxyEnabled = !!deviceSettings.proxy_ble;
			proxyName = deviceSettings.proxy_name || proxyName;
			obdEnabled = !!obdSettings.enabled;
			selectedMode = resolveMode(proxyEnabled, obdEnabled);
			pendingMode = selectedMode;
			modeLoaded = true;
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
	}

	function selectOperatingMode(mode) {
		if (modeLoading || modeSaving || pendingMode === mode) return;
		pendingMode = mode;
		modeMessage = null;
	}

	async function saveOperatingMode() {
		if (modeLoading || modeSaving || !modeIsDirty()) return;

		const modeToSave = pendingMode;
		modeSaving = true;
		modeMessage = null;
		try {
			if (modeToSave === 'proxy') {
				// Disable OBD first so the device never spends a save cycle with
				// both V1 proxy and OBD selected.
				await setObdEnabled(false);
				await setProxyEnabled(true);
			} else if (modeToSave === 'obd') {
				await setProxyEnabled(false);
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
			modeMessage = { type: 'error', text: 'Failed to save operating mode.' };
			await loadOperatingMode();
		} finally {
			modeSaving = false;
		}
	}

	async function saveProxyName() {
		if (modeLoading || modeSaving || proxyNameSaving || !proxyName.trim()) return;

		proxyNameSaving = true;
		modeMessage = null;
		try {
			const formData = new FormData();
			formData.append('proxy_name', proxyName.trim());
			const res = await fetchWithTimeout('/api/device/settings', {
				method: 'POST',
				body: formData
			});
			if (!res.ok) {
				throw new Error('proxy name save failed');
			}
			await invalidateDeviceSettings();
			proxyName = proxyName.trim();
			modeMessage = { type: 'success', text: 'Proxy name saved.' };
		} catch (error) {
			console.error('Failed to save proxy name', error);
			modeMessage = { type: 'error', text: 'Failed to save proxy name.' };
			await loadOperatingMode();
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
	<PageHeader title="Dashboard" subtitle="Live system status and quick health checks." />

	<div class="surface-hero">
		<div class="text-center">
			<div class="mb-2 flex justify-center">
				<BrandMark />
			</div>
			<p class="mb-1 copy-caption">v{$runtimeStatus.device?.firmware_version || '...'}</p>
			<p class="copy-subtle">
				{#if $runtimeStatus.wifi.sta_connected}
					{$runtimeStatus.wifi.ssid} • {$runtimeStatus.wifi.sta_ip}
				{:else}
					AP Mode • {$runtimeStatus.wifi.ap_ip}
				{/if}
			</p>
		</div>
	</div>

	<div class="surface-card">
		<div class="card-body gap-4">
			<div class="flex flex-col gap-2 md:flex-row md:items-start md:justify-between">
				<div>
					<div class="copy-mini-title">Operating mode</div>
					<h2 class="card-title">Choose who controls V1 features</h2>
					<p class="copy-subtle">
						OBD and proxy are mutually exclusive to keep V1 BLE stable. Display, logging, and Wi-Fi stay the same in both modes.
					</p>
				</div>
				{#if modeLoading || modeSaving}
					<span class="loading loading-spinner loading-sm" aria-label="Loading operating mode"></span>
				{:else if modeLoaded}
					<span class="badge {modeIsDirty() ? 'badge-warning' : 'badge-outline'}">
						{modeIsDirty() ? `${modeLabel(pendingMode)} pending save` : `${modeLabel(selectedMode)} saved`}
					</span>
				{/if}
			</div>

			<div class="grid grid-cols-1 gap-3 lg:grid-cols-3">
				<button
					type="button"
					class="surface-panel text-left transition hover:border-primary"
					class:border-primary={pendingMode === 'standalone'}
					class:bg-base-200={pendingMode === 'standalone'}
					aria-pressed={pendingMode === 'standalone'}
					onclick={() => selectOperatingMode('standalone')}
					disabled={modeLoading || modeSaving}
				>
					<div class="flex items-center justify-between gap-2">
						<div class="font-semibold">Display only</div>
						<span class="badge {pendingMode === 'standalone' ? 'badge-primary' : 'badge-outline'}">
							{pendingMode === 'standalone' ? (modeIsDirty() ? 'Pending' : 'Selected') : 'Choose'}
						</span>
					</div>
					<p class="copy-subtle mt-2">
						TL;DR: V1 Simple owns the local display and logging only. OBD speed and phone proxy are off.
					</p>
				</button>

				<button
					type="button"
					class="surface-panel text-left transition hover:border-primary"
					class:border-primary={pendingMode === 'obd'}
					class:bg-base-200={pendingMode === 'obd'}
					aria-pressed={pendingMode === 'obd'}
					onclick={() => selectOperatingMode('obd')}
					disabled={modeLoading || modeSaving}
				>
					<div class="flex items-center justify-between gap-2">
						<div class="font-semibold">OBD</div>
						<span class="badge {pendingMode === 'obd' ? 'badge-primary' : 'badge-outline'}">
							{pendingMode === 'obd' ? (modeIsDirty() ? 'Pending' : 'Selected') : 'Choose'}
						</span>
					</div>
					<p class="copy-subtle mt-2">
						TL;DR: V1 Simple owns local speed mute, quiet controls, and V1 writes. OBD speed is used when configured; phone proxy is off.
					</p>
				</button>

				<button
					type="button"
					class="surface-panel text-left transition hover:border-primary"
					class:border-primary={pendingMode === 'proxy'}
					class:bg-base-200={pendingMode === 'proxy'}
					aria-pressed={pendingMode === 'proxy'}
					onclick={() => selectOperatingMode('proxy')}
					disabled={modeLoading || modeSaving}
				>
					<div class="flex items-center justify-between gap-2">
						<div class="font-semibold">Proxy / App</div>
						<span class="badge {pendingMode === 'proxy' ? 'badge-primary' : 'badge-outline'}">
							{pendingMode === 'proxy' ? (modeIsDirty() ? 'Pending' : 'Selected') : 'Choose'}
						</span>
					</div>
					<p class="copy-subtle mt-2">
						TL;DR: the phone app is trusted to manage muting. V1 Simple relays raw V1 data; OBD is off, and local V1 writes stop while the app is connected.
					</p>
				</button>
			</div>
			<div class="flex flex-col gap-2 sm:flex-row sm:items-center">
				<button
					type="button"
					class="btn btn-primary"
					onclick={saveOperatingMode}
					disabled={modeLoading || modeSaving || !modeIsDirty()}
				>
					{#if modeSaving}
						<span class="loading loading-spinner loading-sm"></span>
					{/if}
					Save operating mode
				</button>
				<p class="copy-caption">
					{#if modeIsDirty()}
						{modeLabel(pendingMode)} is selected but not saved yet.
					{:else}
						Saved mode applies on next normal boot; no V1 proxy or OBD runtime is active while maintenance mode is open.
					{/if}
				</p>
			</div>

			<div class="surface-panel space-y-3">
				<div>
					<div class="font-semibold">Proxy / App advertising name</div>
					<p class="copy-caption">
						BLE name shown to phone apps. It is saved with the mode settings and applies on next normal boot.
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
						disabled={modeLoading || modeSaving || proxyNameSaving}
					/>
					<button
						type="button"
						class="btn btn-outline"
						onclick={saveProxyName}
						disabled={modeLoading || modeSaving || proxyNameSaving || !proxyName.trim()}
					>
						{#if proxyNameSaving}
							<span class="loading loading-spinner loading-sm"></span>
						{/if}
						Save proxy name
					</button>
				</div>
			</div>

			<StatusAlert message={modeMessage} fallbackType="info" dismiss={() => modeMessage = null} />
		</div>
	</div>

	{#if !$isMaintenance}
		<div class="grid grid-cols-1 gap-4 md:grid-cols-2 lg:grid-cols-4">
			<div class="surface-card">
				<div class="card-body">
					<div class="copy-mini-title">Valentine One</div>
					{#if $runtimeStatusLoading}
						<span class="loading loading-spinner loading-sm"></span>
					{:else}
						<div class="status-heading {$runtimeStatus.v1_connected ? 'status-heading-success' : 'status-heading-warning'}">
							{$runtimeStatus.v1_connected ? 'Connected' : 'Scanning...'}
						</div>
						<div class="copy-caption">Bluetooth LE</div>
					{/if}
				</div>
			</div>

			<div class="surface-card">
				<div class="card-body">
					<div class="copy-mini-title">WiFi</div>
					{#if $runtimeStatusLoading}
						<span class="loading loading-spinner loading-sm"></span>
					{:else}
						<div class="status-heading {$runtimeStatus.wifi.sta_connected ? 'status-heading-success' : 'status-heading-info'}">
							{$runtimeStatus.wifi.sta_connected ? 'Online' : 'AP Only'}
						</div>
						{#if $runtimeStatus.wifi.sta_connected}
							<div class="copy-caption {getRssiClass($runtimeStatus.wifi.rssi)}">
								{$runtimeStatus.wifi.ssid} • {$runtimeStatus.wifi.rssi} dBm
							</div>
						{/if}
					{/if}
				</div>
			</div>

			<div class="surface-card">
				<div class="card-body">
					<div class="copy-mini-title">Uptime</div>
					{#if $runtimeStatusLoading}
						<span class="loading loading-spinner loading-sm"></span>
					{:else}
						<div class="status-heading">{formatUptime($runtimeStatus.device?.uptime || 0)}</div>
						<div class="copy-caption">
							{Math.round(($runtimeStatus.device?.heap_free || 0) / 1024)} KB free
						</div>
					{/if}
				</div>
			</div>

			<div class="surface-card">
				<div class="card-body">
					<div class="copy-mini-title">Alerts</div>
					{#if $runtimeStatusLoading}
						<span class="loading loading-spinner loading-sm"></span>
					{:else}
						<div class="status-heading-info">Display only</div>
						<div class="copy-caption">Live radar/ALP alerts stay on the LCD</div>
					{/if}
				</div>
			</div>
		</div>
	{/if}


	<StatusAlert message={$runtimeStatusError} fallbackType="error" />
</div>
