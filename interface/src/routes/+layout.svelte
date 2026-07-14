<script>
    import '../app.css';
    import { onMount } from 'svelte';
    import { page } from '$app/stores';
    import BrandMark from '$lib/components/BrandMark.svelte';
    import {
        refreshDeviceSettings,
        retainDeviceSettings
    } from '$lib/stores/deviceSettings.svelte.js';

    let { children } = $props();
    let showPasswordWarning = $state(false);
    let warningDismissed = $state(false);
    let menuOpen = $state(false);
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
        { href: '/settings', label: 'Settings' }
    ];

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
            releaseDeviceSettings();
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

    function isActivePath(href) {
        const path = $page.url.pathname;
        if (href === '/') {
            return path === '/';
        }
        return path === href || path.startsWith(`${href}/`);
    }
</script>

<div class="app-shell">
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
                    class="btn btn-ghost lg:hidden"
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
        <div class="navbar-center hidden lg:flex">
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
