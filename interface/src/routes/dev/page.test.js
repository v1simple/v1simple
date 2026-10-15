import { fireEvent, render, screen } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function createStorageMock() {
    const data = new Map();

    return {
        getItem(key) {
            return data.has(key) ? data.get(key) : null;
        },
        setItem(key, value) {
            data.set(String(key), String(value));
        },
        removeItem(key) {
            data.delete(String(key));
        }
    };
}

function installDefaultFetch(overrides = []) {
    return installFetchMock(
        [
            ...overrides,
            {
                method: 'GET',
                match: '/api/device/settings',
                respond: jsonResponse({})
            }
        ],
        jsonResponse({})
    );
}

describe('dev route page', () => {
    beforeEach(() => {
        const localStorageMock = createStorageMock();
        const sessionStorageMock = createStorageMock();
        Object.defineProperty(window, 'localStorage', {
            configurable: true,
            value: localStorageMock
        });
        Object.defineProperty(window, 'sessionStorage', {
            configurable: true,
            value: sessionStorageMock
        });
        Object.defineProperty(globalThis, 'localStorage', {
            configurable: true,
            value: localStorageMock
        });
        Object.defineProperty(globalThis, 'sessionStorage', {
            configurable: true,
            value: sessionStorageMock
        });
        global.confirm = vi.fn(() => true);
    });

    afterEach(() => {
        vi.restoreAllMocks();
    });

    it('loads development settings without polling debug endpoints', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('Development Settings');
        expect(
            screen.getByText(/Local-only maintenance information and warning preferences/i)
        ).toBeInTheDocument();
        expect(fetchMock.mock.calls.some(([url]) => String(url).startsWith('/api/debug/'))).toBe(
            false
        );

        unmount();
    });

    it('loads current NVS diagnostics each time the page opens', async () => {
        let settings = {
            proxy_ble: true,
            nvsDiag: {
                healthy: true,
                ns: 'settings_a',
                valid: 1,
                ver: 2,
                bright: 180,
                proxy: 1,
                autoPush: 0
            }
        };
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/device/settings',
                respond: () => jsonResponse(settings)
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('NVS Persistence');
        expect(screen.getByText('settings_a')).toBeInTheDocument();
        expect(screen.getByText('In-memory proxy_ble=true')).toBeInTheDocument();

        unmount();
        settings = {
            proxy_ble: false,
            nvsDiag: { ...settings.nvsDiag, ns: 'settings_b', bright: 90, proxy: 0 }
        };
        const reopened = render(Page);
        expect(await screen.findByText('settings_b')).toBeInTheDocument();
        expect(screen.getByText('90')).toBeInTheDocument();
        expect(screen.getByText('In-memory proxy_ble=false')).toBeInTheDocument();
        reopened.unmount();
    });

    it('preserves the password banner preference and notifies the layout when toggled', async () => {
        installDefaultFetch();
        localStorage.setItem('v1simple:passwordWarningDismissedPersist', '1');
        sessionStorage.setItem('passwordWarningDismissed', 'true');
        const dismissed = [];
        const onPreferenceChange = (event) => dismissed.push(event.detail.dismissed);
        window.addEventListener('v1simple-password-warning-dismissed-change', onPreferenceChange);
        const { unmount } = render(Page);
        try {
            const toggle = await screen.findByRole('checkbox', { name: /Hide default password banner/i });
            expect(toggle).toBeChecked();

            await fireEvent.click(toggle);
            expect(toggle).not.toBeChecked();
            expect(localStorage.getItem('v1simple:passwordWarningDismissedPersist')).toBeNull();
            expect(sessionStorage.getItem('passwordWarningDismissed')).toBeNull();

            await fireEvent.click(toggle);
            expect(toggle).toBeChecked();
            expect(localStorage.getItem('v1simple:passwordWarningDismissedPersist')).toBe('1');
            expect(sessionStorage.getItem('passwordWarningDismissed')).toBe('true');
            expect(dismissed).toEqual([false, true]);
        } finally {
            unmount();
            window.removeEventListener('v1simple-password-warning-dismissed-change', onPreferenceChange);
        }
    });
});
