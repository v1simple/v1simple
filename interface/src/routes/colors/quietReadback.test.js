import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import { cloneDefaultColors } from '#lib/utils/colors.js';
import Page from './+page.svelte';

afterEach(() => { cleanup(); vi.restoreAllMocks(); });
const quietLoadError = 'Failed to load quiet screen setting. Retry before changing it.';
const quietReloadedMessage = 'Quiet screen response was lost. The current setting has been reloaded.';
const quietToggleName = /quiet screen between alerts/i;
function quietFixture(quietMatchers, otherMatchers = []) {
    return installFetchMock([
        ...quietMatchers, ...otherMatchers,
        { method: 'GET', match: '/api/display/settings', respond: jsonResponse(cloneDefaultColors()) }
    ], ({ method, url }) => { throw new Error(`Unexpected ${method} ${url}`); });
}
function postCalls(fetchMock) {
    return fetchMock.mock.calls.filter(([url, init]) => url === '/api/quiet/settings' && init?.method === 'POST');
}

describe('quiet screen readback and uncertainty', () => {
    it.each([false, true])('reloads the committed setting after a lost POST response (initial=%s)', async (initial) => {
        let saved = initial;
        const fetchMock = quietFixture([
            { method: 'GET', match: '/api/quiet/settings', respond: () => jsonResponse({ stealthEnabled: saved }) },
            { method: 'POST', match: '/api/quiet/settings', respond: ({ init }) => {
                saved = init.body.get('stealthEnabled') === 'true';
                return Promise.reject(new TypeError('response lost after commit'));
            } }
        ]);
        render(Page);
        const toggle = await screen.findByRole('checkbox', { name: quietToggleName });
        expect(toggle.checked).toBe(initial);
        await fireEvent.click(toggle);
        await screen.findByText(quietReloadedMessage);
        expect(saved).toBe(!initial);
        expect(toggle.checked).toBe(saved);
        expect(toggle).toBeEnabled();
        expect(postCalls(fetchMock)).toHaveLength(1);
    });

    it.each(['http', 'transport', 'incomplete'])('requires a successful read after an initial %s failure', async (failure) => {
        let ready = false;
        const fetchMock = quietFixture([{
            method: 'GET', match: '/api/quiet/settings', respond: () => {
                if (ready) return jsonResponse({ stealthEnabled: true });
                if (failure === 'transport') return Promise.reject(new TypeError('offline'));
                return failure === 'http' ? jsonResponse({ error: 'unavailable' }, 503) : jsonResponse({});
            }
        }]);
        render(Page);
        const toggle = await screen.findByRole('checkbox', { name: quietToggleName });
        await screen.findByText(quietLoadError);
        expect(toggle).toBeDisabled();
        expect(screen.getByText(/Quiet screen unavailable/)).toBeInTheDocument();
        await act(() => toggle.click());
        expect(postCalls(fetchMock)).toHaveLength(0);
        ready = true;
        await fireEvent.click(screen.getByRole('button', { name: 'Retry quiet screen setting' }));
        await waitFor(() => expect(toggle).toBeEnabled());
        expect(toggle).toBeChecked();
        expect(screen.queryByText(quietLoadError)).not.toBeInTheDocument();
        expect(postCalls(fetchMock)).toHaveLength(0);
    });

    it('keeps an ambiguous write disabled until retry reads authoritative state and never repeats it', async () => {
        let saved = false;
        let readAvailable = true;
        let releaseRetry;
        let pendingRetry = false;
        const fetchMock = quietFixture([
            { method: 'GET', match: '/api/quiet/settings', respond: () => {
                if (!readAvailable) return jsonResponse({ error: 'unavailable' }, 503);
                if (pendingRetry) return new Promise((resolve) => { releaseRetry = () => resolve(jsonResponse({ stealthEnabled: saved })); });
                return jsonResponse({ stealthEnabled: saved });
            } },
            { method: 'POST', match: '/api/quiet/settings', respond: () => {
                saved = true;
                readAvailable = false;
                return Promise.reject(new TypeError('response lost after commit'));
            } }
        ]);
        render(Page);
        const toggle = await screen.findByRole('checkbox', { name: quietToggleName });
        await fireEvent.click(toggle);
        await screen.findByText(quietLoadError);
        expect(toggle).toBeDisabled();
        await act(() => toggle.click());
        expect(postCalls(fetchMock)).toHaveLength(1);
        readAvailable = true;
        pendingRetry = true;
        const retry = screen.getByRole('button', { name: 'Retry quiet screen setting' });
        await fireEvent.click(retry);
        expect(retry).toBeDisabled();
        expect(toggle).toBeDisabled();
        await act(() => retry.click());
        await act(async () => releaseRetry());
        await waitFor(() => expect(toggle).toBeEnabled());
        expect(toggle).toBeChecked();
        expect(postCalls(fetchMock)).toHaveLength(1);
    });

    it('does not hide a quiet read failure behind display save success', async () => {
        let failQuiet = false;
        quietFixture([
            { method: 'GET', match: '/api/quiet/settings', respond: () => failQuiet
                ? jsonResponse({ error: 'unavailable' }, 503)
                : jsonResponse({ stealthEnabled: true }) }
        ], [
            { method: 'POST', match: '/api/display/settings', respond: () => {
                failQuiet = true;
                return jsonResponse({ success: true });
            } }
        ]);
        render(Page);
        const toggle = await screen.findByRole('checkbox', { name: quietToggleName });
        expect(toggle).toBeChecked();
        await fireEvent.click(screen.getByRole('button', { name: 'Save display settings' }));
        await screen.findByText(quietLoadError);
        expect(toggle).toBeDisabled();
        expect(screen.queryByText('Colors saved! Previewing on display...')).not.toBeInTheDocument();
    });

    it('does not start quiet readback after a pending write loses its response following navigation', async () => {
        let rejectWrite;
        const fetchMock = quietFixture([
            { method: 'GET', match: '/api/quiet/settings', respond: jsonResponse({ stealthEnabled: false }) },
            { method: 'POST', match: '/api/quiet/settings', respond: () => new Promise((_resolve, reject) => { rejectWrite = reject; }) }
        ]);
        const { unmount } = render(Page);
        const toggle = await screen.findByRole('checkbox', { name: quietToggleName });
        await fireEvent.click(toggle);
        unmount();
        await act(async () => rejectWrite(new TypeError('offline')));
        expect(fetchMock.mock.calls.filter(([url, init]) => url === '/api/quiet/settings' && !init?.method)).toHaveLength(1);
        expect(postCalls(fetchMock)).toHaveLength(1);
    });
});
