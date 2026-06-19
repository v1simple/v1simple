<script>
	import StatusAlert from '$lib/components/StatusAlert.svelte';

	let {
		open = false,
		title = 'Select WiFi Network',
		selectedPrompt = 'Connect to',
		actionLabel = 'Connect',
		wifiScanning = false,
		wifiNetworks = [],
		selectedNetwork = $bindable(null),
		wifiPassword = $bindable(''),
		wifiConnecting = false,
		onstartWifiScan,
		onselectNetwork,
		onconnectToNetwork,
		oncloseWifiModal
	} = $props();

	function handlePasswordKeydown(event) {
		if (event.key !== 'Enter' || wifiConnecting || (selectedNetwork?.secure && !wifiPassword)) {
			return;
		}

		onconnectToNetwork();
	}

	function handleBack() {
		if (wifiConnecting) return;
		selectedNetwork = null;
	}

	function handleClose() {
		if (wifiConnecting) return;
		oncloseWifiModal();
	}

	function handleBackdropClick() {
		if (wifiConnecting) return;
		oncloseWifiModal();
	}
</script>

{#if open}
	<div class="modal modal-open">
		<div class="modal-box surface-modal max-w-md">
			<h3 class="font-bold text-lg">{title}</h3>

			{#if wifiScanning}
				<div class="state-loading stack">
					<span class="loading loading-spinner loading-lg"></span>
					<p class="mt-4 copy-muted">Scanning for networks...</p>
				</div>
			{:else if selectedNetwork}
				<div class="py-4 space-y-4">
					<p>{selectedPrompt} <strong>{selectedNetwork.ssid}</strong></p>

					{#if selectedNetwork.secure}
						<div class="field-control">
							<label class="label" for="wifi-password">
								<span class="field-label">Password</span>
							</label>
							<input
								id="wifi-password"
								type="password"
								class="input w-full"
								bind:value={wifiPassword}
								placeholder="Enter WiFi password"
								onkeydown={handlePasswordKeydown}
							/>
						</div>
					{:else}
						<StatusAlert message="This is an open network" fallbackType="warning" />
					{/if}

					<div class="flex gap-2 justify-end">
						<button class="btn btn-ghost" onclick={handleBack} disabled={wifiConnecting}>
							Back
						</button>
						<button class="btn btn-primary" onclick={onconnectToNetwork} disabled={wifiConnecting || (selectedNetwork.secure && !wifiPassword)}>
							{#if wifiConnecting}
								<span class="loading loading-spinner loading-sm"></span>
							{/if}
							{actionLabel}
						</button>
					</div>
				</div>
			{:else}
				<div class="py-4">
					{#if wifiNetworks.length === 0}
						<p class="state-empty center">No networks found</p>
					{:else}
						<ul class="menu surface-menu max-h-64 overflow-y-auto">
							{#each wifiNetworks as network}
								<li>
									<button onclick={() => onselectNetwork(network)} class="flex justify-between">
										<span class="flex items-center gap-2">
											{#if network.secure}🔒{:else}🔓{/if}
											{network.ssid}
										</span>
										<span class="copy-muted">
											{network.rssi} dBm
										</span>
									</button>
								</li>
							{/each}
						</ul>
					{/if}

					<div class="flex gap-2 justify-end mt-4">
						<button class="btn btn-ghost btn-sm" onclick={() => onstartWifiScan?.()}>
							Rescan
						</button>
					</div>
				</div>
			{/if}

			<div class="modal-action">
				<button class="btn" onclick={handleClose} disabled={wifiConnecting}>Close</button>
			</div>
		</div>
		<div class="modal-backdrop bg-black/50" role="presentation" onclick={handleBackdropClick}></div>
	</div>
{/if}
