<script>
    import { onMount } from 'svelte';
    import { postSettingsForm } from '$lib/api/settings';
    import {
        invalidateDeviceSettings,
        refreshDeviceSettings,
        retainDeviceSettings
    } from '$lib/stores/deviceSettings.svelte.js';
    import CardSectionHead from '$lib/components/CardSectionHead.svelte';
    import PageHeader from '$lib/components/PageHeader.svelte';
    import StatusAlert from '$lib/components/StatusAlert.svelte';

    let acknowledged = $state(false);
    const DEV_WARNING_ACK_BYPASS_KEY = 'v1simple:devWarningAckBypass';
    const PASSWORD_WARNING_DISMISSED_PERSIST_KEY = 'v1simple:passwordWarningDismissedPersist';
    const PASSWORD_WARNING_DISMISSED_SESSION_KEY = 'passwordWarningDismissed';
    const PASSWORD_WARNING_EVENT = 'v1simple-password-warning-dismissed-change';

    let warningPreferences = $state({
        hidePasswordWarningBanner: false,
        hideDevWarningBanner: false
    });

    let settings = $state({
        powerOffSdLog: false
    });
    let loading = $state(true);
    let saving = $state(false);
    let message = $state(null);
    let nvsDiag = $state(null);
    let settings_proxy_ble = $state(null);

    function setMessage(type, text) {
        message = { type, text };
    }

    function clearMessage() {
        message = null;
    }

    function loadWarningPreferences() {
        if (typeof window === 'undefined') return;
        warningPreferences.hidePasswordWarningBanner =
            localStorage.getItem(PASSWORD_WARNING_DISMISSED_PERSIST_KEY) === '1';
        warningPreferences.hideDevWarningBanner =
            localStorage.getItem(DEV_WARNING_ACK_BYPASS_KEY) === '1';
        if (warningPreferences.hideDevWarningBanner) {
            acknowledged = true;
        }
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

    function toggleDevWarningPreference(enabled) {
        warningPreferences.hideDevWarningBanner = enabled;
        if (typeof window !== 'undefined') {
            if (enabled) {
                localStorage.setItem(DEV_WARNING_ACK_BYPASS_KEY, '1');
            } else {
                localStorage.removeItem(DEV_WARNING_ACK_BYPASS_KEY);
            }
        }
        acknowledged = enabled ? true : false;
    }

    function applySettings(data) {
        if (!data) return;
        settings.powerOffSdLog = data.powerOffSdLog || false;
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

    async function saveSettings() {
        if (!acknowledged) {
            setMessage('warning', 'Please acknowledge the warning before saving');
            return;
        }

        saving = true;
        clearMessage();

        try {
            const formData = new FormData();
            formData.append('powerOffSdLog', settings.powerOffSdLog.toString());

            const response = await postSettingsForm(formData, '/api/device/settings');

            if (response.ok) {
                setMessage('success', 'Settings saved!');
                applySettings(await invalidateDeviceSettings());
            } else {
                setMessage('error', 'Failed to save settings');
            }
        } catch (error) {
            console.error('Save failed:', error);
            setMessage('error', 'Failed to save settings');
        } finally {
            saving = false;
        }
    }

    async function resetDefaults() {
        if (!acknowledged) {
            setMessage('warning', 'Please acknowledge the warning before resetting');
            return;
        }

        if (!confirm('Reset all development settings to defaults?')) return;

        settings.powerOffSdLog = false;
        await saveSettings();
    }
</script>

<div class="page-stack">
    <PageHeader
        title="Development Settings"
        subtitle="Local-only maintenance toggles. HTTP debug metrics and perf-file endpoints are intentionally disabled; use SD logs instead."
    />

    {#if loading}
        <div class="state-loading tall">
            <span class="loading loading-spinner loading-lg"></span>
        </div>
    {:else}
        <div class="surface-card">
            <div class="card-body">
                <CardSectionHead
                    title="Warning Acknowledgements"
                    subtitle="Local browser toggles for warning banners."
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

                <div class="field-control">
                    <label class="label cursor-pointer">
                        <div>
                            <span class="field-label font-semibold"
                                >Auto-accept development warning</span
                            >
                            <p class="copy-caption-soft mt-1">
                                Hide the advanced warning banner and keep maintenance controls
                                unlocked.
                            </p>
                        </div>
                        <input
                            type="checkbox"
                            class="toggle toggle-primary"
                            checked={warningPreferences.hideDevWarningBanner}
                            onchange={(event) =>
                                toggleDevWarningPreference(event.currentTarget.checked)}
                        />
                    </label>
                </div>
                <p class="copy-micro">Stored in this browser.</p>
            </div>
        </div>

        {#if !warningPreferences.hideDevWarningBanner}
            <div class="surface-alert alert-warning warning-strong">
                <svg
                    xmlns="http://www.w3.org/2000/svg"
                    class="stroke-current shrink-0 h-6 w-6"
                    fill="none"
                    viewBox="0 0 24 24"
                >
                    <path
                        stroke-linecap="round"
                        stroke-linejoin="round"
                        stroke-width="2"
                        d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z"
                    />
                </svg>
                <div class="flex-1">
                    <h3 class="font-bold">Warning: Advanced Settings</h3>
                    <div class="copy-subtle">
                        These settings can cause instability or unexpected behavior. Only modify if
                        you know what you're doing.
                    </div>
                    <div class="field-control mt-2">
                        <label class="label cursor-pointer justify-start gap-2">
                            <input
                                type="checkbox"
                                class="checkbox checkbox-warning"
                                bind:checked={acknowledged}
                            />
                            <span class="field-label font-semibold">I understand the risks</span>
                        </label>
                    </div>
                </div>
            </div>
        {/if}

        <StatusAlert {message} />

        <div class="surface-card" class:opacity-50={!acknowledged}>
            <div class="card-body">
                <CardSectionHead
                    title="Power Diagnostics"
                    subtitle="Maintenance-only shutdown logging for hardware validation."
                />

                <div class="field-control">
                    <label class="label cursor-pointer">
                        <div>
                            <span class="field-label font-semibold">Power-Off SD Log</span>
                            <p class="copy-caption-soft mt-1">
                                Log power-off diagnostics and boot reason to /poweroff.log on SD
                                card. For verifying battery-only shutdown.
                            </p>
                        </div>
                        <input
                            type="checkbox"
                            class="toggle toggle-primary"
                            bind:checked={settings.powerOffSdLog}
                            disabled={!acknowledged}
                        />
                    </label>
                </div>
            </div>
        </div>

        {#if nvsDiag}
            <div class="surface-card">
                <div class="card-body">
                    <CardSectionHead
                        title="NVS Persistence"
                        subtitle="Reads directly from flash — confirms settings survive reboot."
                    />
                    <div class="grid grid-cols-2 gap-2 text-sm">
                        <span class="opacity-60">Status</span>
                        <span
                            class={nvsDiag.healthy
                                ? 'text-success font-bold'
                                : 'text-error font-bold'}
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

        <div class="surface-card">
            <div class="card-body">
                <CardSectionHead
                    title="Runtime Diagnostics"
                    subtitle="Use SD logs instead of live HTTP debug polling."
                />
                <p class="copy-caption-soft">
                    The HTTP debug metrics, scenario playback, and perf-file endpoints are disabled
                    to reduce maintenance-server load. Perf CSV logging still writes to SD for
                    offline analysis.
                </p>
            </div>
        </div>

        <div class="flex gap-4">
            <button
                class="btn btn-primary flex-1"
                onclick={saveSettings}
                disabled={!acknowledged || saving}
            >
                {#if saving}
                    <span class="loading loading-spinner loading-sm"></span>
                {/if}
                Save Settings
            </button>
            <button
                class="btn btn-outline flex-1"
                onclick={resetDefaults}
                disabled={!acknowledged || saving}
            >
                Reset to Defaults
            </button>
        </div>
    {/if}
</div>
