<script>
    import '../app.css';
    import { onMount } from 'svelte';
    import { page } from '$app/stores';
    import Sidebar from '$lib/components/Sidebar.svelte';
    import ThemeControls from '$lib/components/ThemeControls.svelte';
    import { createPoll, fetchWithTimeout } from '$lib/utils/poll';
    import { refreshDeviceSettings, retainDeviceSettings } from '$lib/stores/deviceSettings.svelte.js';
    import {
        isMaintenance,
        retainRuntimeStatus,
        runtimeStatus,
        runtimeStatusError,
        runtimeStatusLoading
    } from '$lib/stores/runtimeStatus.svelte.js';

    let { children } = $props();
    let showPasswordWarning = $state(false);
    let warningDismissed = $state(false);
    let drawerOpen = $state(false);
    let menuTrigger = $state(null);
    let maintenanceExitPending = $state(false);
    let maintenanceExitMessage = $state('');
    const DEFAULT_PASSWORD_CACHE_KEY = 'v1simple:isDefaultPassword';
    const DEFAULT_PASSWORD_DISMISSED_KEY = 'passwordWarningDismissed';
    const DEFAULT_PASSWORD_DISMISSED_PERSIST_KEY = 'v1simple:passwordWarningDismissedPersist';
    const PASSWORD_WARNING_EVENT = 'v1simple-password-warning-dismissed-change';
    const routeLabels = [
        { href: '/', label: 'Dashboard' },
        { href: '/autopush', label: 'Auto-Push' },
        { href: '/profiles', label: 'Profiles' },
        { href: '/devices', label: 'Devices' },
        { href: '/colors', label: 'Colors' },
        { href: '/audio', label: 'Audio & Quiet' },
        { href: '/alp', label: 'ALP' },
        { href: '/obd', label: 'OBD' },
        { href: '/gps', label: 'GPS' },
        { href: '/settings', label: 'Settings' }
    ];
    const currentRouteLabel = $derived(
        routeLabels.find(({ href }) =>
            href === '/'
                ? $page.url.pathname === '/'
                : $page.url.pathname === href || $page.url.pathname.startsWith(`${href}/`)
        )?.label || 'V1Simple'
    );

    // Every status response carries the refreshed idle deadline. Reconcile to
    // that device value, then tick locally once per second between polls.
    const DEFAULT_MAINTENANCE_TIMEOUT_MS = 10 * 60 * 1000;
    const MAINTENANCE_TICK_MS = 1000;
    let maintenanceAnchorRemainingMs = $state(0);
    let maintenanceAnchorAtMs = $state(0);
    let maintenanceNowMs = $state(0);
    const maintenanceRemainingMs = $derived(
        Math.max(0, maintenanceAnchorRemainingMs - Math.max(0, maintenanceNowMs - maintenanceAnchorAtMs))
    );
    const maintenanceExpiringSoon = $derived(maintenanceRemainingMs <= 60 * 1000);

    function reportedMaintenanceRemainingMs(status) {
        const timeoutMs = Number(status?.maintenanceBootTimeoutMs) > 0
            ? Number(status.maintenanceBootTimeoutMs)
            : DEFAULT_MAINTENANCE_TIMEOUT_MS;
        const elapsedMs = Number(status?.maintenanceBootUptimeMs) || 0;
        return Math.max(0, timeoutMs - elapsedMs);
    }

    function formatRemaining(milliseconds) {
        const seconds = Math.max(0, Math.ceil(milliseconds / 1000));
        const minutes = Math.floor(seconds / 60);
        const remainder = seconds % 60;
        return `${minutes}m ${remainder.toString().padStart(2, '0')}s`;
    }

    function runWhenIdle(callback, fallbackDelayMs = 250) {
        if (typeof window !== 'undefined' && 'requestIdleCallback' in window) {
            window.requestIdleCallback(callback, { timeout: 1500 });
            return;
        }
        setTimeout(callback, fallbackDelayMs);
    }

    async function refreshDefaultPasswordWarning() {
        try {
            const data = await refreshDeviceSettings();
            if (!data) return;
            const isDefaultPassword = data.isDefaultPassword === true;
            showPasswordWarning = isDefaultPassword;
            sessionStorage.setItem(DEFAULT_PASSWORD_CACHE_KEY, isDefaultPassword ? '1' : '0');
        } catch (error) {
            console.warn('Failed to refresh default password warning', error);
        }
    }

    onMount(() => {
        const releaseDeviceSettings = retainDeviceSettings();
        const releaseRuntimeStatus = retainRuntimeStatus({ needsStatus: true });
        const unsubscribeRuntimeStatus = runtimeStatus.subscribe((status) => {
            maintenanceAnchorRemainingMs = reportedMaintenanceRemainingMs(status);
            maintenanceAnchorAtMs = Date.now();
            maintenanceNowMs = maintenanceAnchorAtMs;
        });
        const maintenanceCountdown = createPoll(() => {
            maintenanceNowMs = Date.now();
        }, MAINTENANCE_TICK_MS);
        maintenanceCountdown.start();

        const handlePasswordWarningPreferenceChange = (event) => {
            const dismissed = event?.detail?.dismissed === true;
            warningDismissed = dismissed;
            if (dismissed) {
                sessionStorage.setItem(DEFAULT_PASSWORD_DISMISSED_KEY, 'true');
                return;
            }
            sessionStorage.removeItem(DEFAULT_PASSWORD_DISMISSED_KEY);
            const cachedDefaultPassword = sessionStorage.getItem(DEFAULT_PASSWORD_CACHE_KEY);
            if (cachedDefaultPassword !== null) {
                showPasswordWarning = cachedDefaultPassword === '1';
                return;
            }
            runWhenIdle(() => void refreshDefaultPasswordWarning(), 250);
        };
        window.addEventListener(PASSWORD_WARNING_EVENT, handlePasswordWarningPreferenceChange);

        if (
            sessionStorage.getItem(DEFAULT_PASSWORD_DISMISSED_KEY) ||
            localStorage.getItem(DEFAULT_PASSWORD_DISMISSED_PERSIST_KEY) === '1'
        ) {
            warningDismissed = true;
        } else {
            const cachedDefaultPassword = sessionStorage.getItem(DEFAULT_PASSWORD_CACHE_KEY);
            if (cachedDefaultPassword !== null) {
                showPasswordWarning = cachedDefaultPassword === '1';
            } else {
                runWhenIdle(() => void refreshDefaultPasswordWarning(), 600);
            }
        }

        return () => {
            maintenanceCountdown.stop();
            unsubscribeRuntimeStatus();
            releaseDeviceSettings();
            releaseRuntimeStatus();
            window.removeEventListener(PASSWORD_WARNING_EVENT, handlePasswordWarningPreferenceChange);
        };
    });

    function dismissWarning() {
        warningDismissed = true;
        sessionStorage.setItem(DEFAULT_PASSWORD_DISMISSED_KEY, 'true');
    }

    async function exitMaintenance() {
        if (!$isMaintenance || maintenanceExitPending || $runtimeStatusLoading) return;
        maintenanceExitPending = true;
        maintenanceExitMessage = '';
        try {
            const response = await fetchWithTimeout('/api/system/reboot-normal', { method: 'POST' });
            if (!response.ok) {
                let detail = '';
                try {
                    const body = await response.json();
                    detail = body?.error || body?.message || '';
                } catch {
                    // The status code is still useful when the body is not JSON.
                }
                throw new Error(detail || `Request failed (${response.status})`);
            }
            maintenanceExitMessage = 'Reboot requested. Reconnect after normal startup.';
        } catch (error) {
            maintenanceExitPending = false;
            maintenanceExitMessage = `Could not exit maintenance: ${error.message}`;
        }
    }
</script>

<div class="app-shell shell-frame">
    <Sidebar
        open={drawerOpen}
        onclose={() => (drawerOpen = false)}
        trigger={menuTrigger}
        status={$runtimeStatus}
        statusLoading={$runtimeStatusLoading}
        statusError={$runtimeStatusError}
    />

    <div class="shell-workspace" inert={drawerOpen}>
        <header class="surface-chrome shell-topbar">
            <button
                bind:this={menuTrigger}
                type="button"
                class="btn btn-ghost btn-square md:hidden"
                aria-label="Open navigation menu"
                aria-expanded={drawerOpen}
                aria-controls="mobile-navigation-drawer"
                onclick={() => (drawerOpen = true)}
            >
                <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 6h16M4 12h16M4 18h16"></path></svg>
            </button>

            <h1 class="shell-route-title">{currentRouteLabel}</h1>
            <div class="shell-topbar-spacer"></div>

            <ThemeControls />

            <div class="maintenance-chrome" class:expiring={maintenanceExpiringSoon && $isMaintenance} role="status" aria-live="polite">
                {#if $runtimeStatusLoading}
                    <strong>Checking status…</strong>
                    <span>Confirming the maintenance session.</span>
                {:else if $runtimeStatusError}
                    <strong>Status unavailable</strong>
                    <span>Maintenance controls stay unavailable until device status returns.</span>
                {:else if $isMaintenance}
                    <strong>Maintenance · {formatRemaining(maintenanceRemainingMs)}</strong>
                    <span>
                        Activity keeps the 10-minute idle window alive automatically.
                        {maintenanceExitMessage}
                    </span>
                {:else}
                    <strong>Maintenance unconfirmed</strong>
                    <span>Waiting for a maintenance-mode status response.</span>
                {/if}
            </div>

            <button
                type="button"
                class="btn btn-primary btn-sm shell-exit-button"
                aria-label={maintenanceExitPending ? 'Rebooting…' : 'Exit maintenance'}
                disabled={!$isMaintenance || $runtimeStatusLoading || maintenanceExitPending}
                onclick={exitMaintenance}
            >
                {maintenanceExitPending ? 'Rebooting…' : 'Exit'}
            </button>
        </header>

        <div class="shell-below-topbar">
            {#if showPasswordWarning && !warningDismissed}
                <div class="surface-alert banner alert-warning password-warning" role="alert">
                    <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 9v2m0 4h.01M5.1 19h13.8a2 2 0 0 0 1.73-3L13.73 4a2 2 0 0 0-3.46 0L3.37 16a2 2 0 0 0 1.73 3z"></path></svg>
                    <div>
                        <h2 class="font-bold">Default Password Detected</h2>
                        <div class="copy-caption">
                            Change your WiFi password in <a href="/settings" class="link font-semibold link-primary">Settings</a> to secure your device.
                        </div>
                    </div>
                    <button type="button" class="btn btn-ghost btn-sm" onclick={dismissWarning} aria-label="Dismiss warning">✕</button>
                </div>
            {/if}

            <main class="app-main">{@render children()}</main>
        </div>
    </div>
</div>
