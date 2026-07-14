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
});
