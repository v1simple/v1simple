<script>
    import { onMount } from 'svelte';
    import {
        invalidateDeviceSettings,
        refreshDeviceSettings,
        retainDeviceSettings
    } from '#lib/stores/deviceSettings.svelte.js';
    import CardSectionHead from '#lib/components/CardSectionHead.svelte';
    import PageHeader from '#lib/components/PageHeader.svelte';
    import StatusAlert from '#lib/components/StatusAlert.svelte';

    const PASSWORD_WARNING_DISMISSED_PERSIST_KEY = 'v1simple:passwordWarningDismissedPersist';
    const PASSWORD_WARNING_DISMISSED_SESSION_KEY = 'passwordWarningDismissed';
    const PASSWORD_WARNING_EVENT = 'v1simple-password-warning-dismissed-change';

    let warningPreferences = $state({
        hidePasswordWarningBanner: false
    });

    let loading = $state(true);
    let message = $state(null);
    let nvsDiag = $state(null);
    let settings_proxy_ble = $state(null);

    function setMessage(type, text) {
        message = { type, text };
    }

    function loadWarningPreferences() {
        if (typeof window === 'undefined') return;
        warningPreferences.hidePasswordWarningBanner =
            localStorage.getItem(PASSWORD_WARNING_DISMISSED_PERSIST_KEY) === '1';
    }

    function togglePasswordWarningPreference(enabled) {
        warningPreferences.hidePasswordWarningBanner = enabled;
        if (typeof window === 'undefined') return;
        if (enabled) {
            localStorage.setItem(PASSWORD_WARNING_DISMISSED_PERSIST_KEY, '1');
            sessionStorage.setItem(PASSWORD_WARNING_DISMISSED_SESSION_KEY, 'true');
        } else {
            localStorage.removeItem(PASSWORD_WARNING_DISMISSED_PERSIST_KEY);
            sessionStorage.removeItem(PASSWORD_WARNING_DISMISSED_SESSION_KEY);
        }
        window.dispatchEvent(
            new CustomEvent(PASSWORD_WARNING_EVENT, { detail: { dismissed: enabled } })
        );
    }

    function applySettings(data) {
        if (!data) return;
        nvsDiag = data.nvsDiag || null;
        settings_proxy_ble = data.proxy_ble ?? null;
    }

    onMount(() => {
        const releaseDeviceSettings = retainDeviceSettings();
        loadWarningPreferences();
        void loadSettings();
        return releaseDeviceSettings;
    });

    async function loadSettings({ force = false } = {}) {
        try {
            const data = force ? await invalidateDeviceSettings() : await refreshDeviceSettings();
            if (!data) throw new Error('settings request failed');
            applySettings(data);
            loading = false;
        } catch (error) {
            console.error('Failed to load settings:', error);
            setMessage('error', 'Failed to load settings');
            loading = false;
        }
    }

</script>

<div class="page-stack">
    <PageHeader
        title="Development Settings"
        subtitle="Local-only maintenance information and warning preferences."
    />

    {#if loading}
        <div class="state-loading tall">
            <span class="loading loading-lg loading-spinner"></span>
        </div>
    {:else}
        <div class="surface-card">
            <div class="card-body">
                <CardSectionHead
                    title="Warning Preference"
                    subtitle="Local browser setting for the password banner."
                />

                <div class="field-control">
                    <label class="label cursor-pointer">
                        <div>
                            <span class="field-label font-semibold"
                                >Hide default password banner</span
                            >
                            <p class="copy-caption-soft mt-1">
                                Suppress the top security banner when default AP password is
                                detected.
                            </p>
                        </div>
                        <input
                            type="checkbox"
                            class="toggle toggle-primary"
                            checked={warningPreferences.hidePasswordWarningBanner}
                            onchange={(event) =>
                                togglePasswordWarningPreference(event.currentTarget.checked)}
                        />
                    </label>
                </div>

                <p class="copy-micro">Stored in this browser.</p>
            </div>
        </div>

        <StatusAlert {message} />

        {#if nvsDiag}
            <div class="surface-card">
                <div class="card-body">
                    <CardSectionHead
                        title="NVS Persistence"
                        subtitle="Current settings read directly from flash."
                    />
                    <div class="grid grid-cols-2 gap-2 text-sm">
                        <span class="opacity-60">Status</span>
                        <span
                            class={nvsDiag.healthy
                                ? 'font-bold text-success'
                                : 'font-bold text-error'}
                        >
                            {nvsDiag.healthy ? 'Healthy' : 'UNHEALTHY'}
                        </span>
                        <span class="opacity-60">Namespace</span>
                        <span class="font-mono">{nvsDiag.ns}</span>
                        <span class="opacity-60">Valid Marker</span>
                        <span class="font-mono">{nvsDiag.valid}</span>
                        <span class="opacity-60">Settings Ver</span>
                        <span class="font-mono">{nvsDiag.ver}</span>
                        <span class="opacity-60">NVS Brightness</span>
                        <span class="font-mono">{nvsDiag.bright}</span>
                        <span class="opacity-60">NVS Proxy BLE</span>
                        <span class="font-mono">{nvsDiag.proxy}</span>
                        <span class="opacity-60">NVS Auto-Push</span>
                        <span class="font-mono">{nvsDiag.autoPush}</span>
                    </div>
                    <div class="mt-2 text-xs opacity-50">
                        In-memory proxy_ble={String(settings_proxy_ble)}
                    </div>
                </div>
            </div>
        {/if}

    {/if}
</div>
