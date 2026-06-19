<script>
	import { onMount } from 'svelte';
	import { fetchWithTimeout } from '$lib/utils/poll';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	import {
		isMaintenance,
		retainRuntimeStatus
	} from '$lib/stores/runtimeStatus.svelte.js';

	const fallbackSlotNames = ['DEFAULT', 'HIGHWAY', 'COMFORT'];

	let devices = $state([]);
	let slots = $state([]);
	let loading = $state(true);
	let message = $state(null);
	let editingAddress = $state('');
	let editName = $state('');
	let busyAddress = $state('');

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
		try {
			const res = await fetchWithTimeout('/api/v1/devices');
			if (!res.ok) {
				message = { type: 'error', text: 'Failed to load saved devices.' };
				return;
			}
			const data = await res.json();
			devices = (data.devices || []).map((device) => ({
				address: device.address || '',
				name: device.name || '',
				defaultProfile: Number(device.defaultProfile || 0),
				connected: !!device.connected
			}));
		} catch (e) {
			message = { type: 'error', text: 'Failed to load saved devices.' };
		}
	}

	async function fetchSlots() {
		try {
			const res = await fetchWithTimeout('/api/autopush/slots');
			if (!res.ok) return;
			const data = await res.json();
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
		editingAddress = device.address;
		editName = device.name || '';
	}

	function cancelRename() {
		editingAddress = '';
		editName = '';
	}

	async function saveName(address) {
		busyAddress = address;
		try {
			const formData = new FormData();
			formData.append('address', address);
			formData.append('name', editName.trim());

			const res = await fetchWithTimeout('/api/v1/devices/name', {
				method: 'POST',
				body: formData
			});
			if (!res.ok) {
				message = { type: 'error', text: 'Failed to save device name.' };
				return;
			}

			devices = devices.map((device) =>
				device.address === address
					? { ...device, name: editName.trim() }
					: device
			);
			message = { type: 'success', text: 'Device name saved.' };
			cancelRename();
		} catch (e) {
			message = { type: 'error', text: 'Failed to save device name.' };
		} finally {
			busyAddress = '';
		}
	}

	async function setDefaultProfile(address, profile) {
		busyAddress = address;
		try {
			const selected = Math.max(0, Math.min(3, Number(profile)));
			const formData = new FormData();
			formData.append('address', address);
			formData.append('profile', String(selected));

			const res = await fetchWithTimeout('/api/v1/devices/profile', {
				method: 'POST',
				body: formData
			});
			if (!res.ok) {
				message = { type: 'error', text: 'Failed to save default profile.' };
				await fetchDevices();
				return;
			}

			devices = devices.map((device) =>
				device.address === address
					? { ...device, defaultProfile: selected }
					: device
			);
			message = { type: 'success', text: 'Default profile updated.' };
		} catch (e) {
			message = { type: 'error', text: 'Failed to save default profile.' };
			await fetchDevices();
		} finally {
			busyAddress = '';
		}
	}

	async function deleteDevice(address) {
		const device = devices.find((d) => d.address === address);
		const displayName = device?.name || address;
		if (!confirm(`Remove ${displayName} from saved devices?`)) {
			return;
		}

		busyAddress = address;
		try {
			const formData = new FormData();
			formData.append('address', address);

			const res = await fetchWithTimeout('/api/v1/devices/delete', {
				method: 'POST',
				body: formData
			});
			if (!res.ok) {
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
			busyAddress = '';
		}
	}
</script>

<div class="page-stack">
	<PageHeader title="Saved V1 Devices" subtitle="Name saved V1 addresses and set a default auto-push profile for each.">
		<button class="btn btn-outline btn-sm" onclick={refresh} disabled={loading}>
			Refresh
		</button>
	</PageHeader>

	<StatusAlert {message} />

	<div class="surface-note">
		<p>
			Saved devices are discovered from real V1 connections. You can assign each device a friendly name and optional
			auto-push slot override.
		</p>
		{#if $isMaintenance}
			<p class="copy-caption">
				Live V1 connection state is hidden in maintenance mode because BLE is not running.
			</p>
		{/if}
	</div>

	{#if loading}
		<div class="state-loading">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else if devices.length === 0}
		<div class="surface-card">
			<div class="card-body">
				<p class="state-empty">No saved V1 devices yet. Connect to a V1 and it will appear here.</p>
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
										class="input input-sm w-full max-w-xs"
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
									<h3 class="font-semibold text-lg">{device.name || 'Unnamed V1'}</h3>
								{/if}
								<p class="copy-caption font-mono">{device.address}</p>
								{#if device.connected && !$isMaintenance}
									<span class="badge badge-success badge-sm">Connected</span>
								{/if}
							</div>
							<div class="flex gap-2">
								{#if editingAddress === device.address}
									<button
										class="btn btn-success btn-sm"
										onclick={() => saveName(device.address)}
										disabled={busyAddress === device.address}
									>
										Save
									</button>
									<button class="btn btn-ghost btn-sm" onclick={cancelRename} disabled={busyAddress === device.address}>
										Cancel
									</button>
								{:else}
									<button
										class="btn btn-ghost btn-sm"
										onclick={() => startRename(device)}
										disabled={busyAddress === device.address}
									>
										Rename
									</button>
									<button
										class="btn btn-error btn-outline btn-sm"
										onclick={() => deleteDevice(device.address)}
										disabled={busyAddress === device.address}
									>
										Delete
									</button>
								{/if}
							</div>
						</div>

						<div class="space-y-2">
							<label class="label py-0" for={profileSelectId(device.address)}>
								<span class="field-label copy-caption">Default Auto-Push Profile</span>
							</label>
							<select
								id={profileSelectId(device.address)}
								class="select select-sm w-full max-w-sm"
								value={String(device.defaultProfile || 0)}
								onchange={(e) => setDefaultProfile(device.address, Number(e.currentTarget.value))}
								disabled={busyAddress === device.address}
							>
								<option value="0">Use global active slot</option>
								{#each [1, 2, 3] as profileNumber}
									<option value={String(profileNumber)}>{slotLabel(profileNumber)}</option>
								{/each}
							</select>
							{#if Number(device.defaultProfile || 0) > 0}
								<p class="copy-caption">Will auto-push {slotLabel(Number(device.defaultProfile || 0))} on connect.</p>
							{/if}
						</div>
					</div>
				</div>
			{/each}
		</div>
	{/if}
</div>
