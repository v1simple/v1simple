<script>
	import { onMount } from 'svelte';
	import { postSettingsForm } from '$lib/api/settings';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	import ToggleSetting from '$lib/components/ToggleSetting.svelte';
	import { invalidateDeviceSettings, refreshDeviceSettings, retainDeviceSettings } from '$lib/stores/deviceSettings.svelte.js';

	let settings = $state({
		alpEnabled: false,
		alpSdLogEnabled: false,
		alpAlertPersistSec: 0,
		alpDisableV1LaserOnPush: true
	});
	let loading = $state(true);
	let saving = $state(false);
	let message = $state(null);

	onMount(() => {
		const releaseDeviceSettings = retainDeviceSettings();
		void (async () => {
			try {
				await fetchSettings({ showLoadError: true });
			} finally {
				loading = false;
			}
		})();
		return releaseDeviceSettings;
	});

	function applySettings(data) {
		if (!data) return;
		if (typeof data.alpEnabled === 'boolean') settings.alpEnabled = data.alpEnabled;
		if (typeof data.alpSdLogEnabled === 'boolean') {
			settings.alpSdLogEnabled = data.alpSdLogEnabled;
		}
		if (typeof data.alpAlertPersistSec === 'number') {
			settings.alpAlertPersistSec = data.alpAlertPersistSec;
		}
		if (typeof data.alpDisableV1LaserOnPush === 'boolean') {
			settings.alpDisableV1LaserOnPush = data.alpDisableV1LaserOnPush;
		}
	}

	async function fetchSettings({ showLoadError = false, force = false } = {}) {
		try {
			const data = force ? await invalidateDeviceSettings() : await refreshDeviceSettings();
			if (!data) throw new Error('settings request failed');
			applySettings(data);
			return data;
		} catch (error) {
			if (showLoadError) {
				console.error('Failed to load ALP settings', error);
				message = { type: 'error', text: 'Failed to load ALP settings.' };
			}
			throw error;
		}
	}

	async function saveField(fields, successText = 'ALP setting saved.') {
		saving = true;
		message = null;
		try {
			const formData = new FormData();
			for (const [key, value] of Object.entries(fields)) {
				formData.append(key, String(value));
			}

			const res = await postSettingsForm(formData, '/api/device/settings');
			if (!res.ok) {
				message = { type: 'error', text: 'Failed to save ALP setting.' };
				await fetchSettings().catch(() => null);
				return false;
			}

			message = { type: 'success', text: successText };
			await fetchSettings({ force: true }).catch(() => null);
			return true;
		} catch (_) {
			message = { type: 'error', text: 'Connection error.' };
			await fetchSettings().catch(() => null);
			return false;
		} finally {
			saving = false;
		}
	}

	async function handleAlpEnabled(checked) {
		settings.alpEnabled = checked;
		const ok = await saveField(
			{ alpEnabled: checked },
			checked ? 'ALP listener enabled. Reboot to start the UART listener.' : 'ALP listener disabled.'
		);
		if (!ok) return;
	}

	async function handleDisableV1Laser(checked) {
		settings.alpDisableV1LaserOnPush = checked;
		await saveField({ alpDisableV1LaserOnPush: checked });
	}

	async function handleSdLog(checked) {
		settings.alpSdLogEnabled = checked;
		await saveField({ alpSdLogEnabled: checked });
	}

	async function handlePersistChange(event) {
		const value = Math.max(0, Math.min(5, Number(event.currentTarget.value) || 0));
		settings.alpAlertPersistSec = value;
		await saveField({ alpAlertPersistSec: value });
	}
</script>

<div class="page-stack">
	<PageHeader title="ALP" subtitle="AL Priority listener and V1 laser handoff settings." />

	<StatusAlert {message} />

	{#if loading}
		<div class="state-loading tall">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<div class="surface-card">
			<div class="card-body space-y-4">
				<CardSectionHead
					title="Active Laser Protection"
					subtitle="ALP owns laser alerting; V1 profile pushes can suppress duplicate V1 laser alerts."
				/>
				<p class="copy-caption-soft">Changes apply on next normal boot.</p>

				<ToggleSetting
					title="Enable ALP listener"
					description="Activates the UART2 listener for ALP HiFi serial data. Changing this setting requires a reboot."
					checked={settings.alpEnabled}
					disabled={saving}
					onChange={handleAlpEnabled}
				/>

				<ToggleSetting
					title="Disable V1 laser alerts on profile push"
					description="When ALP is enabled, every profile push clears the V1 Gen2 laser bit before writing user bytes. Saved profiles are not modified; disabling this restores their laser setting on the next push."
					checked={settings.alpDisableV1LaserOnPush}
					disabled={saving}
					onChange={handleDisableV1Laser}
				/>

				<ToggleSetting
					title="Enable ALP SD logging"
					description="Writes ALP state transitions, heartbeats, gun IDs, and session events to CSV on the SD card."
					checked={settings.alpSdLogEnabled}
					disabled={saving}
					onChange={handleSdLog}
				/>

				<div class="field-control">
					<label class="label" for="alp-persist-sec">
						<span class="field-label">ALP display persistence (seconds)</span>
					</label>
					<input
						id="alp-persist-sec"
						type="number"
						class="input w-24"
						min="0"
						max="5"
						bind:value={settings.alpAlertPersistSec}
						disabled={saving}
						onchange={handlePersistChange}
					/>
					<div class="label">
						<span class="field-hint">
							0 disables the post-alert visual tail; max 5 seconds.
						</span>
					</div>
				</div>

				{#if settings.alpEnabled && settings.alpDisableV1LaserOnPush}
					<StatusAlert
						message={{
							type: 'info',
							text: 'Next profile push will disable V1 laser alerts so ALP is the laser source of record.'
						}}
					/>
				{/if}
			</div>
		</div>
	{/if}
</div>
