<script>
    import { nativeDialog } from '#lib/utils/nativeDialog.js';
    import StatusAlert from '#lib/components/StatusAlert.svelte';

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
        saveBlocked = false,
        slotChanged = false,
        appendUnavailable = false,
        recovery,
        onstartWifiScan,
        onselectNetwork,
        onconnectToNetwork,
        oncloseWifiModal
    } = $props();

    let dialog = $state();

    // Selection replaces the focused network button; keep focus in the new view.
    $effect(() => {
        const showingPassword = !wifiScanning && selectedNetwork;
        if (dialog?.open) {
            dialog.querySelector(showingPassword ? 'input, button:not(:disabled)' : 'button:not(:disabled)')?.focus();
        }
    });

    function handlePasswordKeydown(event) {
        if (event.key !== 'Enter' || wifiConnecting || saveBlocked || (selectedNetwork?.secure && !wifiPassword)) {
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

</script>

{#if open}
    <dialog bind:this={dialog} use:nativeDialog class="modal" aria-label={title}
        oncancel={(event) => { event.preventDefault(); handleClose(); }}>
        <div class="surface-modal modal-box max-w-md">
            <h3 class="text-lg font-bold">{title}</h3>
            {@render recovery?.()}
            {#if slotChanged}
                <p class="surface-note" role="status">This saved network slot changed. Close the picker and select the current slot again.</p>
            {/if}
            {#if appendUnavailable}
                <p class="surface-note" role="status">Reorder saved networks or add this network manually with a chosen priority.</p>
            {/if}

            {#if wifiScanning}
                <div class="state-loading stack">
                    <span class="loading loading-lg loading-spinner"></span>
                    <p class="copy-muted mt-4">Scanning for networks...</p>
                </div>
            {:else if selectedNetwork}
                <div class="space-y-4 py-4">
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

                    <div class="flex justify-end gap-2">
                        <button
                            class="btn btn-ghost"
                            onclick={handleBack}
                            disabled={wifiConnecting}
                        >
                            Back
                        </button>
                        <button
                            class="btn btn-primary"
                            onclick={onconnectToNetwork}
                            disabled={wifiConnecting || saveBlocked || (selectedNetwork.secure && !wifiPassword)}
                        >
                            {#if wifiConnecting}
                                <span class="loading loading-sm loading-spinner"></span>
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
                        <ul class="surface-menu menu max-h-64 overflow-y-auto">
                            {#each wifiNetworks as network}
                                <li>
                                    <button
                                        onclick={() => onselectNetwork(network)}
                                        class="flex justify-between"
                                    >
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

                    <div class="mt-4 flex justify-end gap-2">
                        <button class="btn btn-ghost btn-sm" onclick={() => onstartWifiScan?.()} disabled={saveBlocked}>
                            Rescan
                        </button>
                    </div>
                </div>
            {/if}

            <div class="modal-action">
                <button class="btn" onclick={handleClose} disabled={wifiConnecting}>Close</button>
            </div>
        </div>
        <button
            class="modal-backdrop bg-black/50"
            tabindex="-1"
            aria-label="Close WiFi picker"
            onclick={handleClose}
            disabled={wifiConnecting}
        ></button>
    </dialog>
{/if}
