<script>
    import '../app.css';
    import { onMount } from 'svelte';
    import { page } from '$app/stores';
    import BrandMark from '$lib/components/BrandMark.svelte';
    import { createPoll, fetchWithTimeout } from '$lib/utils/poll';
    import {
        refreshDeviceSettings,
        retainDeviceSettings
    } from '$lib/stores/deviceSettings.svelte.js';
    import {
        isMaintenance,
        retainRuntimeStatus,
        runtimeStatus
    } from '$lib/stores/runtimeStatus.svelte.js';

    let { children } = $props();
    let showPasswordWarning = $state(false);
    let warningDismissed = $state(false);
    let menuOpen = $state(false);
    let maintenanceExitPending = $state(false);
    let maintenanceExitMessage = $state('');
    const DEFAULT_PASSWORD_CACHE_KEY = 'v1simple:isDefaultPassword';
    const DEFAULT_PASSWORD_DISMISSED_KEY = 'passwordWarningDismissed';
    const DEFAULT_PASSWORD_DISMISSED_PERSIST_KEY = 'v1simple:passwordWarningDismissedPersist';
    const PASSWORD_WARNING_EVENT = 'v1simple-password-warning-dismissed-change';
    const navLinks = [
        { href: '/', label: 'Dashboard' },
        { href: '/autopush', label: 'Auto-Push' },
        { href: '/profiles', label: 'Profiles' },
        { href: '/devices', label: 'Devices' },
        { href: '/colors', label: 'Colors' },
        { href: '/audio', label: 'Audio & Quiet' },
        { href: '/alp', label: 'ALP' },
        { href: '/obd', label: 'OBD' },
        { href: '/gps', label: 'GPS' },
        { href: '/logs', label: 'Logs' },
        { href: '/settings', label: 'Settings' }
    ];
    // The maintenance deadline is no longer a fixed 10 minutes from boot: the
    // firmware extends it while the UI is being used, bounded by a hard cap
    // (see MainRuntimePolicy::evaluateMaintenanceSession in
    // include/main_runtime_state.h). /api/status keeps reporting
    // maintenanceBootUptimeMs as time elapsed against the CURRENT deadline, so
    // `maintenanceBootTimeoutMs - maintenanceBootUptimeMs` is still the
    // device's real remaining time — but only as of the last poll.
    //
    // Two things follow, and both are handled below:
    //   * The value must be re-anchored on every poll rather than treated as a
    //     monotonic function of boot time, otherwise an extended deadline makes
    //     the countdown disagree with the device.
    //   * Polls land every 3s, so a countdown driven only by them visibly jumps
    //     (#19). Tick locally once a second between polls and reconcile on each
    //     poll, which bounds drift at one poll interval.
    const DEFAULT_MAINTENANCE_TIMEOUT_MS = 10 * 60 * 1000;
    const MAINTENANCE_TICK_MS = 1000;
    let maintenanceAnchorRemainingMs = $state(0);
    let maintenanceAnchorAtMs = $state(0);
    let maintenanceNowMs = $state(0);
    const maintenanceRemainingMs = $derived(
        Math.max(
            0,
            maintenanceAnchorRemainingMs - Math.max(0, maintenanceNowMs - maintenanceAnchorAtMs)
        )
    );
    const maintenanceExpiringSoon = $derived(maintenanceRemainingMs <= 60 * 1000);

    function reportedMaintenanceRemainingMs(status) {
        const timeoutMs =
            Number(status?.maintenanceBootTimeoutMs) > 0
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

    // Check if using default password on mount
    onMount(() => {
        const releaseDeviceSettings = retainDeviceSettings();
        const releaseRuntimeStatus = retainRuntimeStatus({ needsStatus: true });

        // Reconcile the maintenance countdown against every status snapshot,
        // then tick it locally between polls.
        const unsubscribeRuntimeStatus = runtimeStatus.subscribe((status) => {
            maintenanceAnchorRemainingMs = reportedMaintenanceRemainingMs(status);
            maintenanceAnchorAtMs = Date.now();
            maintenanceNowMs = maintenanceAnchorAtMs;
        });
        // createPoll rather than a bare setInterval: the frontend HTTP
        // resilience contract (scripts/check_frontend_http_resilience_contract.py)
        // routes every recurring timer through the shared poll utility.
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
            runWhenIdle(() => {
                void refreshDefaultPasswordWarning();
            }, 250);
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
                runWhenIdle(() => {
                    void refreshDefaultPasswordWarning();
                }, 600);
            }
        }

        return () => {
            maintenanceCountdown.stop();
            unsubscribeRuntimeStatus();
            releaseDeviceSettings();
            releaseRuntimeStatus();
            window.removeEventListener(
                PASSWORD_WARNING_EVENT,
                handlePasswordWarningPreferenceChange
            );
        };
    });

    function dismissWarning() {
        warningDismissed = true;
        sessionStorage.setItem(DEFAULT_PASSWORD_DISMISSED_KEY, 'true');
    }

    function toggleMenu() {
        menuOpen = !menuOpen;
    }

    function closeMenu() {
        menuOpen = false;
    }

    async function exitMaintenance() {
        if (!$isMaintenance || maintenanceExitPending) return;

        maintenanceExitPending = true;
        maintenanceExitMessage = '';
        try {
            const response = await fetchWithTimeout('/api/system/reboot-normal', {
                method: 'POST'
            });
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

    function isActivePath(href) {
        const path = $page.url.pathname;
        if (href === '/') {
            return path === '/';
        }
        return path === href || path.startsWith(`${href}/`);
    }
</script>

<div class="app-shell">
    {#if $isMaintenance}
        <div
            class="surface-alert banner"
            class:alert-warning={maintenanceExpiringSoon}
            class:alert-info={!maintenanceExpiringSoon}
            role="status"
            aria-live="polite"
        >
            <div class="flex-1">
                <h3 class="font-bold">Maintenance mode</h3>
                <div class="copy-caption">
                    {formatRemaining(maintenanceRemainingMs)} remaining before automatic normal reboot.
                    Save any open edits before the timer expires.
                </div>
                {#if maintenanceExitMessage}
                    <div class="copy-caption mt-1">{maintenanceExitMessage}</div>
                {/if}
            </div>
            <button
                type="button"
                class="btn btn-sm btn-primary"
                disabled={maintenanceExitPending}
                onclick={exitMaintenance}
            >
                {maintenanceExitPending ? 'Rebooting…' : 'Exit maintenance'}
            </button>
        </div>
    {/if}

    <!-- Security Warning Banner -->
    {#if showPasswordWarning && !warningDismissed}
        <div class="surface-alert alert-warning banner" role="alert">
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
            <div>
                <h3 class="font-bold">Default Password Detected</h3>
                <div class="copy-caption">
                    Change your WiFi password in <a
                        href="/settings"
                        class="link link-primary font-semibold">Settings</a
                    > to secure your device.
                </div>
            </div>
            <button
                class="btn btn-sm btn-ghost"
                onclick={dismissWarning}
                aria-label="Dismiss warning">✕</button
            >
        </div>
    {/if}

    <!-- Navigation -->
    <nav class="navbar surface-chrome border-b" aria-label="Main navigation">
        <div class="navbar-start">
            <div class="dropdown" class:dropdown-open={menuOpen}>
                <button
                    type="button"
                    aria-label="Open navigation menu"
                    aria-expanded={menuOpen}
                    class="btn btn-ghost xl:hidden"
                    onclick={toggleMenu}
                >
                    <svg
                        xmlns="http://www.w3.org/2000/svg"
                        class="h-5 w-5"
                        fill="none"
                        viewBox="0 0 24 24"
                        stroke="currentColor"
                        aria-hidden="true"
                    >
                        <path
                            stroke-linecap="round"
                            stroke-linejoin="round"
                            stroke-width="2"
                            d="M4 6h16M4 12h8m-8 6h16"
                        />
                    </svg>
                </button>
                <ul class="menu menu-sm dropdown-content mt-3 z-[1] w-52 surface-menu" role="menu">
                    {#each navLinks as link}
                        <li>
                            <a
                                href={link.href}
                                class="nav-link"
                                class:active={isActivePath(link.href)}
                                onclick={closeMenu}>{link.label}</a
                            >
                        </li>
                    {/each}
                </ul>
            </div>
            <a href="/" class="btn btn-ghost h-auto min-h-0 px-2 py-1">
                <BrandMark compact />
            </a>
        </div>
        <div class="navbar-center hidden xl:flex">
            <ul class="menu menu-horizontal px-1">
                {#each navLinks as link}
                    <li>
                        <a href={link.href} class="nav-link" class:active={isActivePath(link.href)}
                            >{link.label}</a
                        >
                    </li>
                {/each}
            </ul>
        </div>
        <div class="navbar-end" aria-hidden="true"></div>
    </nav>

    <!-- Main Content -->
    <main class="app-main">
        {@render children()}
    </main>

    <!-- Footer -->
    <footer class="footer footer-center p-4 surface-chrome border-t text-base-content mt-8">
        <aside>
            <div class="flex flex-wrap items-center justify-center gap-2">
                <BrandMark compact />
                <span class="copy-muted">•</span>
                <a href="https://github.com/v1simple/v1simple" class="link link-primary">GitHub</a>
            </div>
        </aside>
    </footer>
</div>
