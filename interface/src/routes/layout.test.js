import { createRawSnippet } from 'svelte';
import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, installFixtureFetchMock, jsonResponse } from '../test/fetch-mock.js';

const pageStore = vi.hoisted(() => {
    let value = { url: new URL('http://localhost/') };
    const subscribers = new Set();
    return {
        page: {
            subscribe(callback) {
                callback(value);
                subscribers.add(callback);
                return () => subscribers.delete(callback);
            }
        },
        setPath(path) {
            value = { url: new URL(path, 'http://localhost') };
            for (const callback of subscribers) callback(value);
        }
    };
});

vi.mock('$app/stores', () => ({ page: pageStore.page }));

import Layout from './+layout.svelte';

function createMemoryStorage() {
    const values = new Map();
    return {
        getItem: vi.fn((key) => (values.has(key) ? values.get(key) : null)),
        setItem: vi.fn((key, value) => values.set(String(key), String(value))),
        removeItem: vi.fn((key) => values.delete(key)),
        clear: vi.fn(() => values.clear())
    };
}

function installStorage(name) {
    const storage = createMemoryStorage();
    Object.defineProperty(window, name, { value: storage, configurable: true });
    vi.stubGlobal(name, storage);
    return storage;
}

function childSnippet(text = 'Route content') {
    return createRawSnippet(() => ({
        render: () => `<section aria-label="test child">${text}</section>`
    }));
}

function renderLayout(props = {}) {
    return render(Layout, { children: childSnippet(), ...props });
}

function installLayoutFetch() {
    installFetchMock(
        [{ method: 'GET', match: '/api/device/settings', respond: jsonResponse({}) }],
        jsonResponse({})
    );
}

describe('root layout', () => {
    beforeEach(() => {
        installStorage('sessionStorage');
        installStorage('localStorage');
        document.documentElement.dataset.theme = 'amethyst';
        document.documentElement.classList.remove('light');
        document.documentElement.classList.add('dark');
        document.documentElement.style.colorScheme = 'dark';
        pageStore.setPath('/');
    });

    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
    });

    it('renders grouped navigation and marks the active route with aria-current', () => {
        installFetchMock(
            [{ method: 'GET', match: '/api/device/settings', respond: jsonResponse({}) }],
            jsonResponse({})
        );
        pageStore.setPath('/colors');

        const { unmount } = renderLayout();

        expect(screen.getByRole('region', { name: /test child/i })).toHaveTextContent(
            'Route content'
        );
        expect(screen.getAllByText('Detector').length).toBeGreaterThan(0);
        expect(screen.getAllByText('Integrations').length).toBeGreaterThan(0);
        expect(screen.getAllByText('System').length).toBeGreaterThan(0);
        const colorLinks = screen.getAllByRole('link', { name: 'Colors' });
        expect(colorLinks.every((link) => link.getAttribute('aria-current') === 'page')).toBe(true);
        expect(screen.getAllByRole('link', { name: 'Profiles' }).every((link) => !link.hasAttribute('aria-current'))).toBe(true);
        expect(screen.queryByRole('link', { name: 'Logs' })).not.toBeInTheDocument();

        unmount();
    });

    it('defaults to the amethyst theme in dark mode', async () => {
        installLayoutFetch();
        const { unmount } = renderLayout();

        await waitFor(() => {
            expect(document.documentElement).toHaveAttribute('data-theme', 'amethyst');
            expect(document.documentElement).toHaveClass('dark');
        });
        expect(document.documentElement).not.toHaveClass('light');
        expect(localStorage.getItem('v1simple:theme')).toBe('amethyst');
        expect(localStorage.getItem('v1simple:colorMode')).toBe('dark');

        unmount();
    });

    it('falls back to amethyst and dark for invalid stored appearance values', async () => {
        localStorage.setItem('v1simple:theme', 'unknown');
        localStorage.setItem('v1simple:colorMode', 'sepia');
        document.documentElement.dataset.theme = 'unknown';
        document.documentElement.className = 'light';
        installLayoutFetch();

        const { unmount } = renderLayout();

        await waitFor(() => {
            expect(document.documentElement).toHaveAttribute('data-theme', 'amethyst');
            expect(document.documentElement).toHaveClass('dark');
        });
        expect(localStorage.getItem('v1simple:theme')).toBe('amethyst');
        expect(localStorage.getItem('v1simple:colorMode')).toBe('dark');

        unmount();
    });

    it('selects and persists a color theme through the accessible controls', async () => {
        installLayoutFetch();
        const { unmount } = renderLayout();

        await fireEvent.click(screen.getAllByRole('button', { name: 'Use Ocean theme' })[0]);

        expect(document.documentElement).toHaveAttribute('data-theme', 'ocean');
        expect(localStorage.getItem('v1simple:theme')).toBe('ocean');
        expect(
            screen.getAllByRole('button', { name: 'Use Ocean theme' }).every((button) =>
                button.getAttribute('aria-pressed') === 'true'
            )
        ).toBe(true);

        unmount();
    });

    it('toggles and persists light and dark mode', async () => {
        installLayoutFetch();
        const { unmount } = renderLayout();

        await fireEvent.click(screen.getByRole('button', { name: 'Use light mode' }));

        expect(document.documentElement).toHaveClass('light');
        expect(document.documentElement).not.toHaveClass('dark');
        expect(document.documentElement.style.colorScheme).toBe('light');
        expect(localStorage.getItem('v1simple:colorMode')).toBe('light');
        expect(screen.getByRole('button', { name: 'Use dark mode' })).toBeInTheDocument();

        unmount();
    });

    it('restores the selected theme and mode when the layout reloads', async () => {
        installLayoutFetch();
        const first = renderLayout();
        await fireEvent.click(screen.getAllByRole('button', { name: 'Use Forest theme' })[0]);
        await fireEvent.click(screen.getByRole('button', { name: 'Use light mode' }));
        first.unmount();

        document.documentElement.dataset.theme = 'amethyst';
        document.documentElement.className = 'dark';
        document.documentElement.style.colorScheme = 'dark';

        const second = renderLayout();
        await waitFor(() => {
            expect(document.documentElement).toHaveAttribute('data-theme', 'forest');
            expect(document.documentElement).toHaveClass('light');
        });
        expect(localStorage.getItem('v1simple:theme')).toBe('forest');
        expect(localStorage.getItem('v1simple:colorMode')).toBe('light');

        second.unmount();
    });

    it('provides modal mobile navigation with focus, escape, backdrop, and navigation close semantics', async () => {
        installFetchMock(
            [{ method: 'GET', match: '/api/device/settings', respond: jsonResponse({}) }],
            jsonResponse({})
        );
        const { unmount } = renderLayout();
        const preventAnchorNavigation = (event) => {
            if (event.target?.closest?.('a')) event.preventDefault();
        };
        document.addEventListener('click', preventAnchorNavigation, { capture: true });

        const menuButton = screen.getByRole('button', { name: /open navigation menu/i });
        expect(menuButton).toHaveAttribute('aria-expanded', 'false');
        expect(menuButton).toHaveAttribute('aria-controls', 'mobile-navigation-drawer');
        await fireEvent.click(menuButton);
        expect(menuButton).toHaveAttribute('aria-expanded', 'true');
        const drawer = screen.getByRole('dialog', { name: 'Main navigation' });
        expect(drawer).toHaveAttribute('aria-modal', 'true');
        await waitFor(() => expect(screen.getAllByRole('button', { name: /close navigation menu/i })[1]).toHaveFocus());
        expect(document.body.style.overflow).toBe('hidden');

        await fireEvent.keyDown(drawer, { key: 'Escape' });
        expect(menuButton).toHaveAttribute('aria-expanded', 'false');
        await waitFor(() => expect(menuButton).toHaveFocus());

        await fireEvent.click(menuButton);
        await fireEvent.click(screen.getByRole('dialog', { name: 'Main navigation' }).querySelector('a[href="/settings"]'));
        expect(menuButton).toHaveAttribute('aria-expanded', 'false');

        await fireEvent.click(menuButton);
        await fireEvent.click(screen.getAllByRole('button', { name: /close navigation menu/i })[0]);
        expect(menuButton).toHaveAttribute('aria-expanded', 'false');
        expect(document.body.style.overflow).toBe('');

        document.removeEventListener('click', preventAnchorNavigation, { capture: true });
        unmount();
    });

    it('persists desktop sidebar collapse state across remounts', async () => {
        installFetchMock(
            [{ method: 'GET', match: '/api/device/settings', respond: jsonResponse({}) }],
            jsonResponse({})
        );
        const first = renderLayout();
        await fireEvent.click(screen.getByRole('button', { name: 'Collapse sidebar' }));
        expect(localStorage.getItem('v1simple:sidebarCollapsed')).toBe('1');
        expect(screen.getByRole('button', { name: 'Expand sidebar' })).toBeInTheDocument();
        first.unmount();

        const second = renderLayout();
        expect(await screen.findByRole('button', { name: 'Expand sidebar' })).toBeInTheDocument();
        second.unmount();
    });

    it('shows and dismisses the default-password warning', async () => {
        vi.useFakeTimers();
        installFetchMock(
            [
                {
                    method: 'GET',
                    match: '/api/device/settings',
                    respond: jsonResponse({ isDefaultPassword: true })
                }
            ],
            jsonResponse({})
        );
        const { unmount } = renderLayout();

        await vi.advanceTimersByTimeAsync(700);
        const alert = await screen.findByRole('alert');
        expect(alert).toHaveTextContent('Default Password Detected');

        await fireEvent.click(screen.getByRole('button', { name: /dismiss warning/i }));
        await waitFor(() => expect(screen.queryByRole('alert')).not.toBeInTheDocument());
        expect(sessionStorage.getItem('passwordWarningDismissed')).toBe('true');

        unmount();
    });

    it('shows the maintenance deadline and can request a normal reboot', async () => {
        const fetchMock = installFixtureFetchMock('frontend_core_routes', [
            {
                method: 'GET',
                match: '/api/status',
                respond: jsonResponse({
                    maintenanceBoot: true,
                    maintenanceBootUptimeMs: 125000,
                    maintenanceBootTimeoutMs: 600000
                })
            },
            {
                method: 'GET',
                match: '/api/device/settings',
                respond: jsonResponse({})
            }
        ]);
        const { unmount } = renderLayout();

        const status = await screen.findByRole('status');
        await waitFor(() => expect(status).toHaveTextContent('Maintenance · 7m 55s'));
        expect(status).toHaveTextContent('Activity keeps the 10-minute idle window alive automatically');

        await fireEvent.click(screen.getByRole('button', { name: 'Exit maintenance' }));

        await waitFor(() => {
            expect(
                fetchMock.mock.calls.some(
                    ([url, init]) =>
                        url === '/api/system/reboot-normal' &&
                        init?.method === 'POST' &&
                        init?.headers?.['X-V1Simple-Request'] === 'maintenance-ui'
                )
            ).toBe(true);
        });
        expect(await screen.findByText(/Reboot requested/)).toBeInTheDocument();
        expect(screen.getByRole('button', { name: 'Rebooting…' })).toBeDisabled();

        unmount();
    });

    it('surfaces a maintenance exit failure and allows retry', async () => {
        installFetchMock(
            [
                {
                    method: 'GET',
                    match: '/api/status',
                    respond: jsonResponse({
                        maintenanceBoot: true,
                        maintenanceBootUptimeMs: 570000,
                        maintenanceBootTimeoutMs: 600000
                    })
                },
                {
                    method: 'GET',
                    match: '/api/device/settings',
                    respond: jsonResponse({})
                },
                {
                    method: 'POST',
                    match: '/api/system/reboot-normal',
                    respond: jsonResponse({ error: 'restart unavailable' }, 503)
                }
            ],
            jsonResponse({})
        );
        const { unmount } = renderLayout();

        const status = await screen.findByRole('status');
        await waitFor(() => expect(status).toHaveTextContent('Maintenance · 0m 30s'));
        await fireEvent.click(screen.getByRole('button', { name: 'Exit maintenance' }));

        await waitFor(() =>
            expect(status).toHaveTextContent('Could not exit maintenance: restart unavailable')
        );
        expect(screen.getByRole('button', { name: 'Exit maintenance' })).toBeEnabled();

        unmount();
    });

    it('shows maintenance hostname and active AP/STA addresses in the sidebar footer', async () => {
        installFetchMock(
            [
                {
                    method: 'GET',
                    match: '/api/status',
                    respond: jsonResponse({
                        maintenanceBoot: true,
                        maintenanceBootUptimeMs: 1000,
                        maintenanceBootTimeoutMs: 600000,
                        device: { hostname: 'garage-v1' },
                        wifi: {
                            sta_connected: true,
                            sta_ip: '192.168.1.23',
                            ap_active: true,
                            ap_ip: '192.168.35.5'
                        }
                    })
                },
                { method: 'GET', match: '/api/device/settings', respond: jsonResponse({}) }
            ],
            jsonResponse({})
        );
        const { unmount } = renderLayout();

        await waitFor(() => expect(screen.getAllByText('garage-v1').length).toBeGreaterThan(0));
        expect(screen.getAllByText('STA 192.168.1.23 · AP 192.168.35.5').length).toBeGreaterThan(0);
        expect(screen.getAllByText('Maintenance session').length).toBeGreaterThan(0);

        unmount();
    });
});
