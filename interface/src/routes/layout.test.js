import { createRawSnippet } from 'svelte';
import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../test/fetch-mock.js';

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

describe('root layout', () => {
    beforeEach(() => {
        installStorage('sessionStorage');
        installStorage('localStorage');
        pageStore.setPath('/');
    });

    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
    });

    it('renders route content and marks the active nav path', () => {
        installFetchMock(
            [{ method: 'GET', match: '/api/device/settings', respond: jsonResponse({}) }],
            jsonResponse({})
        );
        pageStore.setPath('/colors');

        const { unmount } = renderLayout();

        expect(screen.getByRole('region', { name: /test child/i })).toHaveTextContent(
            'Route content'
        );
        const colorLinks = screen.getAllByRole('link', { name: 'Colors' });
        expect(colorLinks.some((link) => link.classList.contains('active'))).toBe(true);
        expect(screen.getAllByRole('link', { name: 'Logs' })).toHaveLength(2);

        unmount();
    });

    it('toggles and closes the mobile navigation menu', async () => {
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
        await fireEvent.click(menuButton);
        expect(menuButton).toHaveAttribute('aria-expanded', 'true');

        await fireEvent.click(screen.getAllByRole('link', { name: 'Settings' })[0]);
        expect(menuButton).toHaveAttribute('aria-expanded', 'false');

        document.removeEventListener('click', preventAnchorNavigation, { capture: true });
        unmount();
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
        const fetchMock = installFetchMock(
            [
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
                },
                {
                    method: 'POST',
                    match: '/api/system/reboot-normal',
                    respond: jsonResponse({ success: true, rebooting: true }, 202)
                }
            ],
            jsonResponse({})
        );
        const { unmount } = renderLayout();

        const status = await screen.findByRole('status');
        expect(status).toHaveTextContent('Maintenance mode');
        expect(status).toHaveTextContent('7m 55s remaining before automatic normal reboot');

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
        expect(status).toHaveTextContent('0m 30s remaining');
        await fireEvent.click(screen.getByRole('button', { name: 'Exit maintenance' }));

        expect(
            await screen.findByText('Could not exit maintenance: restart unavailable')
        ).toBeInTheDocument();
        expect(screen.getByRole('button', { name: 'Exit maintenance' })).toBeEnabled();

        unmount();
    });
});
