<script>
    import { onMount, tick } from 'svelte';
    import { page } from '$app/stores';

    let {
        open = false,
        onclose = () => {},
        trigger = null,
        status = {},
        statusLoading = true,
        statusError = null
    } = $props();

    const COLLAPSED_STORAGE_KEY = 'v1simple:sidebarCollapsed';
    const navGroups = [
        {
            label: '',
            links: [
                {
                    href: '/',
                    label: 'Dashboard',
                    paths: ['M3 3h7v9H3z', 'M14 3h7v5h-7z', 'M14 12h7v9h-7z', 'M3 16h7v5H3z']
                }
            ]
        },
        {
            label: 'Detector',
            links: [
                { href: '/profiles', label: 'Profiles', paths: ['M12 12a4 4 0 1 0 0-8 4 4 0 0 0 0 8z', 'M4 21v-1a8 8 0 0 1 16 0v1'] },
                { href: '/colors', label: 'Colors', paths: ['M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18z', 'M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6z'] },
                { href: '/audio', label: 'Audio & Quiet', paths: ['M11 5 6 9H3v6h3l5 4V5z', 'M15.5 8.5a5 5 0 0 1 0 7'] },
                { href: '/autopush', label: 'Auto-Push', paths: ['M12 19V5', 'm6 11 6-6 6 6'] }
            ]
        },
        {
            label: 'Integrations',
            links: [
                { href: '/alp', label: 'ALP', paths: ['M2 12h4l3-8 4 16 3-8h6'] },
                { href: '/obd', label: 'OBD', paths: ['M5 7h14a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V9a2 2 0 0 1 2-2z', 'M8 7V5h8v2'] },
                { href: '/gps', label: 'GPS', paths: ['M12 21s-7-5.5-7-11a7 7 0 0 1 14 0c0 5.5-7 11-7 11z', 'M12 12.5a2.5 2.5 0 1 0 0-5 2.5 2.5 0 0 0 0 5z'] },
                { href: '/devices', label: 'Devices', paths: ['M8 3h8a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z', 'M11 18h2'] }
            ]
        },
        {
            label: 'System',
            links: [
                { href: '/settings', label: 'Settings', paths: ['M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6z', 'M19 12a7 7 0 0 0-.1-1.2l2-1.6-2-3.4-2.4 1a7 7 0 0 0-2-1.2L14 3h-4l-.5 2.6a7 7 0 0 0-2 1.2l-2.4-1-2 3.4 2 1.6A7 7 0 0 0 5 12c0 .4 0 .8.1 1.2l-2 1.6 2 3.4 2.4-1a7 7 0 0 0 2 1.2L10 21h4l.5-2.6a7 7 0 0 0 2-1.2l2.4 1 2-3.4-2-1.6c.1-.4.1-.8.1-1.2z'] }
            ]
        }
    ];

    let collapsed = $state(false);
    let drawer = $state(null);
    let closeButton = $state(null);
    let wasOpen = false;

    onMount(() => {
        collapsed = localStorage.getItem(COLLAPSED_STORAGE_KEY) === '1';
    });

    $effect(() => {
        if (open) {
            wasOpen = true;
            const priorOverflow = document.body.style.overflow;
            document.body.style.overflow = 'hidden';
            void tick().then(() => closeButton?.focus());
            return () => {
                document.body.style.overflow = priorOverflow;
            };
        }

        if (wasOpen) {
            wasOpen = false;
            void tick().then(() => trigger?.focus());
        }
    });

    function isActivePath(href) {
        const path = $page.url.pathname;
        return href === '/' ? path === '/' : path === href || path.startsWith(`${href}/`);
    }

    function toggleCollapsed() {
        collapsed = !collapsed;
        localStorage.setItem(COLLAPSED_STORAGE_KEY, collapsed ? '1' : '0');
    }

    function handleDrawerKeydown(event) {
        if (event.key === 'Escape') {
            event.preventDefault();
            onclose();
            return;
        }
        if (event.key !== 'Tab') return;

        const focusable = drawer?.querySelectorAll('a[href], button:not([disabled]), [tabindex]:not([tabindex="-1"])');
        if (!focusable?.length) return;
        const first = focusable[0];
        const last = focusable[focusable.length - 1];
        if (event.shiftKey && document.activeElement === first) {
            event.preventDefault();
            last.focus();
        } else if (!event.shiftKey && document.activeElement === last) {
            event.preventDefault();
            first.focus();
        }
    }

    function activeIps() {
        const ips = [];
        if (status?.wifi?.sta_connected && status.wifi.sta_ip) ips.push(`STA ${status.wifi.sta_ip}`);
        if (status?.wifi?.ap_active && status.wifi.ap_ip) ips.push(`AP ${status.wifi.ap_ip}`);
        return ips.join(' · ') || 'IP unavailable';
    }
</script>

{#snippet navContents(closeAfterNavigation = false)}
    <nav class="sidebar-nav" aria-label="Main navigation">
        {#each navGroups as group}
            <div class="sidebar-group">
                {#if group.label}<div class="sidebar-group-label">{group.label}</div>{/if}
                {#each group.links as link}
                    <a
                        href={link.href}
                        class="sidebar-link"
                        class:active={isActivePath(link.href)}
                        aria-current={isActivePath(link.href) ? 'page' : undefined}
                        title={collapsed ? link.label : undefined}
                        onclick={() => closeAfterNavigation && onclose()}
                    >
                        <svg viewBox="0 0 24 24" aria-hidden="true">
                            {#each link.paths as path}<path d={path}></path>{/each}
                        </svg>
                        <span class="sidebar-link-label">{link.label}</span>
                    </a>
                {/each}
            </div>
        {/each}
    </nav>
{/snippet}

{#snippet sessionFooter()}
    <div class="sidebar-footer" aria-live="polite">
        <span class="session-dot" class:pending={statusLoading || statusError}></span>
        <div class="sidebar-footer-copy">
            {#if statusLoading}
                <strong>Checking status…</strong>
            {:else if statusError}
                <strong>Status unavailable</strong>
                <span>Waiting for the device</span>
            {:else}
                <strong>{status.maintenanceBoot ? 'Maintenance session' : 'Maintenance unconfirmed'}</strong>
                <span>{status.device?.hostname || 'Hostname unavailable'}</span>
                <span>{activeIps()}</span>
            {/if}
        </div>
    </div>
{/snippet}

<aside class="desktop-sidebar surface-chrome" class:collapsed aria-label="Application sidebar">
    <a href="/" class="sidebar-brand" aria-label="V1 Simple dashboard">
        <span class="sidebar-brand-mark" aria-hidden="true">V1</span>
        <span class="sidebar-brand-name">V1Simple</span>
    </a>
    {@render navContents()}
    {@render sessionFooter()}
    <button
        type="button"
        class="btn btn-ghost btn-sm sidebar-collapse"
        aria-label={collapsed ? 'Expand sidebar' : 'Collapse sidebar'}
        aria-expanded={!collapsed}
        onclick={toggleCollapsed}
    >
        <svg viewBox="0 0 24 24" aria-hidden="true"><path d={collapsed ? 'm9 18 6-6-6-6' : 'm15 18-6-6 6-6'}></path></svg>
        <span>{collapsed ? 'Expand' : 'Collapse'}</span>
    </button>
</aside>

<div class="mobile-drawer-layer" class:open aria-hidden={!open}>
    <button type="button" class="mobile-drawer-backdrop" tabindex="-1" aria-label="Close navigation menu" onclick={onclose}></button>
    <div
        id="mobile-navigation-drawer"
        class="mobile-drawer surface-chrome"
        role="dialog"
        aria-modal="true"
        aria-label="Main navigation"
        aria-hidden={!open}
        inert={!open}
        bind:this={drawer}
        onkeydown={handleDrawerKeydown}
    >
        <div class="mobile-drawer-head">
            <a href="/" class="sidebar-brand" aria-label="V1 Simple dashboard" onclick={onclose}>
                <span class="sidebar-brand-mark" aria-hidden="true">V1</span>
                <span class="sidebar-brand-name">V1Simple</span>
            </a>
            <button bind:this={closeButton} type="button" class="btn btn-ghost btn-square btn-sm" aria-label="Close navigation menu" onclick={onclose}>
                <svg viewBox="0 0 24 24" aria-hidden="true"><path d="m6 6 12 12M18 6 6 18"></path></svg>
            </button>
        </div>
        {@render navContents(true)}
        {@render sessionFooter()}
    </div>
</div>

<style>
    .desktop-sidebar { display: none; }
    .sidebar-brand { display: flex; min-width: 0; align-items: center; gap: .65rem; color: inherit; text-decoration: none; }
    .sidebar-brand-mark { display: grid; width: 2.1rem; height: 2.1rem; flex: 0 0 2.1rem; place-items: center; border: 1px solid color-mix(in oklab, var(--color-primary) 58%, transparent); border-radius: .55rem; background: color-mix(in oklab, var(--color-primary) 18%, var(--color-base-200)); color: var(--color-primary); font-size: .72rem; font-weight: 800; letter-spacing: .04em; }
    .sidebar-brand-name { overflow: hidden; font-weight: 750; white-space: nowrap; }
    .sidebar-nav { min-height: 0; flex: 1; overflow-y: auto; padding: .55rem; }
    .sidebar-group + .sidebar-group { margin-top: .65rem; }
    .sidebar-group-label { padding: .35rem .65rem .25rem; color: color-mix(in oklab, var(--color-base-content) 52%, transparent); font-size: .65rem; font-weight: 700; letter-spacing: .11em; text-transform: uppercase; }
    .sidebar-link { display: flex; min-height: 2.45rem; align-items: center; gap: .75rem; border-radius: .55rem; padding: .5rem .65rem; color: color-mix(in oklab, var(--color-base-content) 72%, transparent); font-size: .88rem; font-weight: 600; text-decoration: none; transition: background-color 150ms ease, color 150ms ease; }
    .sidebar-link:hover { background: color-mix(in oklab, var(--color-primary) 10%, transparent); color: var(--color-base-content); }
    .sidebar-link.active { background: color-mix(in oklab, var(--color-primary) 17%, transparent); color: var(--color-primary); box-shadow: inset 0 0 0 1px color-mix(in oklab, var(--color-primary) 28%, transparent); }
    .sidebar-link svg, .sidebar-collapse svg, .mobile-drawer-head button svg { width: 1.15rem; height: 1.15rem; flex: 0 0 1.15rem; fill: none; stroke: currentColor; stroke-linecap: round; stroke-linejoin: round; stroke-width: 1.8; }
    .sidebar-footer { display: flex; min-height: 4.6rem; align-items: flex-start; gap: .6rem; border-top: 1px solid var(--app-border-color-muted); padding: .75rem; }
    .session-dot { width: .55rem; height: .55rem; flex: 0 0 .55rem; margin-top: .28rem; border-radius: 999px; background: var(--color-success); box-shadow: 0 0 .5rem color-mix(in oklab, var(--color-success) 55%, transparent); }
    .session-dot.pending { background: var(--color-warning); box-shadow: none; }
    .sidebar-footer-copy { display: flex; min-width: 0; flex-direction: column; font-size: .7rem; line-height: 1.35; }
    .sidebar-footer-copy strong, .sidebar-footer-copy span { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .sidebar-footer-copy span { color: color-mix(in oklab, var(--color-base-content) 60%, transparent); }
    .sidebar-collapse { margin: 0 .55rem .55rem; justify-content: flex-start; gap: .65rem; }
    .mobile-drawer-layer { position: fixed; z-index: 70; inset: 0; visibility: hidden; pointer-events: none; }
    .mobile-drawer-layer.open { visibility: visible; pointer-events: auto; }
    .mobile-drawer-backdrop { position: absolute; inset: 0; border: 0; background: rgb(0 0 0 / .65); opacity: 0; transition: opacity 180ms ease; }
    .mobile-drawer-layer.open .mobile-drawer-backdrop { opacity: 1; }
    .mobile-drawer { position: absolute; inset: 0 auto 0 0; display: flex; width: min(19rem, calc(100vw - 3rem)); flex-direction: column; border-right: 1px solid var(--app-border-color-muted); transform: translateX(-100%); transition: transform 180ms ease; }
    .mobile-drawer-layer.open .mobile-drawer { transform: translateX(0); }
    .mobile-drawer-head { display: flex; min-height: 4rem; align-items: center; justify-content: space-between; gap: 1rem; border-bottom: 1px solid var(--app-border-color-muted); padding: .75rem; }
    @media (prefers-reduced-motion: reduce) { .mobile-drawer, .mobile-drawer-backdrop { transition: none; } }
    @media (min-width: 48rem) {
        .desktop-sidebar { position: sticky; top: 0; display: flex; width: 15.5rem; height: 100dvh; flex: 0 0 15.5rem; flex-direction: column; overflow: hidden; border-right: 1px solid var(--app-border-color-muted); transition: width 180ms ease, flex-basis 180ms ease; }
        .desktop-sidebar > .sidebar-brand { min-height: 4rem; border-bottom: 1px solid var(--app-border-color-muted); padding: .75rem; }
        .desktop-sidebar.collapsed { width: 4.5rem; flex-basis: 4.5rem; }
        .desktop-sidebar.collapsed .sidebar-brand { justify-content: center; padding-inline: 0; }
        .desktop-sidebar.collapsed .sidebar-brand-name, .desktop-sidebar.collapsed .sidebar-link-label, .desktop-sidebar.collapsed .sidebar-group-label, .desktop-sidebar.collapsed .sidebar-footer-copy, .desktop-sidebar.collapsed .sidebar-collapse span { display: none; }
        .desktop-sidebar.collapsed .sidebar-nav { padding-inline: .55rem; }
        .desktop-sidebar.collapsed .sidebar-group + .sidebar-group { margin-top: .5rem; border-top: 1px solid var(--app-border-color-muted); padding-top: .5rem; }
        .desktop-sidebar.collapsed .sidebar-link, .desktop-sidebar.collapsed .sidebar-collapse, .desktop-sidebar.collapsed .sidebar-footer { justify-content: center; padding-inline: 0; }
        .desktop-sidebar.collapsed .sidebar-footer { align-items: center; }
        .desktop-sidebar.collapsed .sidebar-collapse { margin-inline: .55rem; }
        .mobile-drawer-layer { display: none; }
    }
</style>
