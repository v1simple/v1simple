import { act, cleanup, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, expect, it, vi } from 'vitest';
import { installFetchMock, jsonResponse } from '../test/fetch-mock.js';
import Page from './+page.svelte';
afterEach(() => { cleanup(); vi.restoreAllMocks(); });
it('cannot overwrite an unread advertising name when the initial mode fetch failed', async () => {
    vi.spyOn(console, 'error').mockImplementation(() => {});
    const state = { proxy_ble: false, proxy_name: 'My Saved Proxy', enabled: false };
    let reads = 0;
    const fetchMock = installFetchMock([
        { method: 'GET', match: '/api/status', respond: jsonResponse({ maintenanceBoot: true, wifi: {}, device: {} }) },
        { method: 'GET', match: '/api/device/settings', respond: () => ++reads === 1
            ? jsonResponse({ error: 'read unavailable' }, 503)
            : jsonResponse(state) },
        { method: 'GET', match: '/api/obd/config', respond: jsonResponse({ enabled: false }) },
        { method: 'POST', match: '/api/device/settings', respond: ({ init }) => {
            state.proxy_ble = init.body.get('proxy_ble') === 'true';
            if (init.body.has('proxy_name')) state.proxy_name = init.body.get('proxy_name');
            return jsonResponse({ success: true });
        } }
    ], ({ method, url }) => { throw new Error(`Unexpected ${method} ${url}`); });
    render(Page);
    await screen.findByText('Failed to load operating mode.');
    await act(() => screen.getByRole('button', { name: /^Proxy \/ App/ }).click());
    await act(() => screen.getByRole('button', { name: 'Save operating mode' }).click());
    await waitFor(() => expect(state.proxy_name).toBe('My Saved Proxy'));
    expect(fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST')).toHaveLength(0);
});

it.each(['device', 'obd', 'incomplete'])('retries %s mode data before allowing changes and preserves the saved name', async (failure) => {
    vi.spyOn(console, 'error').mockImplementation(() => {});
    const state = { proxy_ble: false, proxy_name: 'My Saved Proxy', enabled: false };
    let fail = true;
    const fetchMock = installFetchMock([
        { method: 'GET', match: '/api/status', respond: jsonResponse({ maintenanceBoot: true, wifi: {}, device: {} }) },
        { method: 'GET', match: '/api/device/settings', respond: () => {
            if (fail && failure === 'device') return jsonResponse({ error: 'read unavailable' }, 503);
            if (fail && failure === 'incomplete') return jsonResponse({ proxy_ble: false });
            return jsonResponse(state);
        } },
        { method: 'GET', match: '/api/obd/config', respond: () => fail && failure === 'obd'
            ? jsonResponse({ error: 'read unavailable' }, 503)
            : jsonResponse({ enabled: state.enabled }) },
        { method: 'POST', match: '/api/device/settings', respond: ({ init }) => {
            if (init.body.has('proxy_ble')) state.proxy_ble = init.body.get('proxy_ble') === 'true';
            if (init.body.has('proxy_name')) state.proxy_name = init.body.get('proxy_name');
            return jsonResponse({ success: true });
        } }
    ], ({ method, url }) => { throw new Error(`Unexpected ${method} ${url}`); });
    render(Page);
    await screen.findByText('Failed to load operating mode.');
    expect(screen.getByRole('button', { name: /^Proxy \/ App/ })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Save operating mode' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Save proxy name' })).toBeDisabled();
    expect(screen.getByLabelText('Proxy Name')).toBeDisabled();
    fail = false;
    await act(() => screen.getByRole('button', { name: 'Retry operating mode' }).click());
    await screen.findByText('Display only saved');
    expect(screen.getByLabelText('Proxy Name')).toHaveValue('My Saved Proxy');
    expect(screen.queryByText('Failed to load operating mode.')).not.toBeInTheDocument();
    await act(() => screen.getByRole('button', { name: /^Proxy \/ App/ }).click());
    await act(() => screen.getByRole('button', { name: 'Save operating mode' }).click());
    await screen.findByText('Proxy / App mode saved. Reboot normally to apply.');
    expect(state.proxy_ble).toBe(true);
    expect(state.proxy_name).toBe('My Saved Proxy');
    expect(fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST')).toHaveLength(1);
});
