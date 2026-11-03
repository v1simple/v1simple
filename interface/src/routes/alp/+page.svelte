<script>
    import { onMount } from 'svelte';
    import { postSettingsForm } from '#lib/api/settings.js';
    import CardSectionHead from '#lib/components/CardSectionHead.svelte';
    import PageHeader from '#lib/components/PageHeader.svelte';
    import StatusAlert from '#lib/components/StatusAlert.svelte';
    import ToggleSetting from '#lib/components/ToggleSetting.svelte';
    import {
        invalidateDeviceSettings,
        refreshDeviceSettings,
        retainDeviceSettings
    } from '#lib/stores/deviceSettings.svelte.js';

    let settings = $state({
        alpEnabled: false,
        alpAlertPersistSec: 0,
        alpDisableV1LaserOnPush: true
    });
    let loading = $state(true);
    let loaded = $state(false);
    let saving = $state(false);
    let message = $state(null);

    onMount(() => {
        const releaseDeviceSettings = retainDeviceSettings();
        void fetchSettings().finally(() => { loading = false; });
        return releaseDeviceSettings;
    });

    async function fetchSettings({ force = false } = {}) {
        try {
            const data = force ? await invalidateDeviceSettings() : await refreshDeviceSettings();
            if (typeof data?.alpEnabled !== 'boolean' ||
                typeof data?.alpDisableV1LaserOnPush !== 'boolean' ||
                !Number.isInteger(data?.alpAlertPersistSec) ||
                data.alpAlertPersistSec < 0 || data.alpAlertPersistSec > 5) {
                throw new Error('Incomplete ALP settings');
            }
            settings = {
                alpEnabled: data.alpEnabled,
                alpAlertPersistSec: data.alpAlertPersistSec,
                alpDisableV1LaserOnPush: data.alpDisableV1LaserOnPush
            };
            loaded = true;
            return true;
        } catch (error) {
            loaded = false;
            console.error('Failed to load ALP settings', error);
            message = { type: 'error', text: 'Failed to load ALP settings.' };
            return false;
        }
    }

    async function retryLoad() {
        if (loading || saving) return;
        loading = true;
        message = null;
        await fetchSettings({ force: true });
        loading = false;
    }

    async function saveField(fields, successText = 'ALP setting saved.') {
        if (!loaded || loading || saving) return false;
        saving = true;
        message = null;
        let saved = false;
        try {
            const formData = new FormData();
            for (const [key, value] of Object.entries(fields)) {
                formData.append(key, String(value));
            }
            const res = await postSettingsForm(formData, '/api/device/settings');
            saved = res.ok;
            if (!saved) message = { type: 'error', text: 'Failed to save ALP setting.' };
        } catch (_) {
            message = { type: 'error', text: 'Connection error.' };
        }
        // A lost response may follow a committed write. Read once; never repeat it.
        const refreshed = await fetchSettings({ force: true });
        if (saved && refreshed) message = { type: 'success', text: successText };
        saving = false;
        return saved && refreshed;
    }

    async function handleAlpEnabled(checked) {
        if (!loaded || loading || saving) return;
        settings.alpEnabled = checked;
        const ok = await saveField(
            { alpEnabled: checked },
            checked
                ? 'ALP listener enabled. Reboot to start the UART listener.'
                : 'ALP listener disabled.'
        );
        if (!ok) return;
    }

    async function handleDisableV1Laser(checked) {
        if (!loaded || loading || saving) return;
        settings.alpDisableV1LaserOnPush = checked;
        await saveField({ alpDisableV1LaserOnPush: checked });
    }

    async function handlePersistChange(event) {
        if (!loaded || loading || saving) return;
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
            <span class="loading loading-lg loading-spinner"></span>
        </div>
    {:else if !loaded}
        <button class="btn btn-outline btn-sm" onclick={retryLoad}>Retry ALP settings</button>
    {:else}
        <div class="surface-card">
            <div class="card-body space-y-4">
                <CardSectionHead
                    title="Active Laser Protection"
                    subtitle="ALP owns laser alerting; Auto-Push can suppress duplicate V1 laser alerts."
                />
                <p class="copy-caption-soft">
                    Listener enable starts on the next normal boot. Display persistence applies
                    during normal operation, and laser handoff applies on the next automatic
                    profile push.
                </p>

                <ToggleSetting
                    title="Enable ALP listener"
                    description="Activates the UART2 listener for ALP HiFi serial data. Changing this setting requires a reboot."
                    checked={settings.alpEnabled}
                    disabled={saving}
                    onChange={handleAlpEnabled}
                />

                <ToggleSetting
                    title="Disable V1 laser alerts on Auto-Push"
                    description="When ALP is enabled, Auto-Push clears the V1 Gen2 laser bit before writing user bytes. Saved profiles are not modified; disabling this restores their laser setting on the next automatic push."
                    checked={settings.alpDisableV1LaserOnPush}
                    disabled={saving}
                    onChange={handleDisableV1Laser}
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
                            text: 'The next automatic profile push will disable V1 laser alerts so ALP is the laser source of record.'
                        }}
                    />
                {/if}
            </div>
        </div>
    {/if}
</div>
