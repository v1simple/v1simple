import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

import * as settingsLazyComponents from '#lib/features/settings/settingsLazyComponents.js';
import { apiFixtureMatchers, installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

const defaultSlots = [
    {
        index: 0,
        ssid: 'HomeWifi',
        label: 'Garage',
        priority: 0,
        hasPassword: true,
        lastConnectedAtSec: 100,
        configured: true
    },
    {
        index: 1,
        ssid: 'PhoneHotspot',
        label: 'Phone',
        priority: 1,
        hasPassword: true,
        lastConnectedAtSec: 50,
        configured: true
    },
    {
        index: 2,
        ssid: '',
        label: '',
        priority: 2,
        hasPassword: false,
        lastConnectedAtSec: 0,
        configured: false
    },
    {
        index: 3,
        ssid: '',
        label: '',
        priority: 3,
        hasPassword: false,
        lastConnectedAtSec: 0,
        configured: false
    }
];

function jsonBodies(fetchMock, url, method = 'POST') {
    return fetchMock.mock.calls
        .filter(([requestUrl, init]) => requestUrl === url && (init?.method || 'GET') === method)
        .map(([, init]) => JSON.parse(init?.body || '{}'));
}

function createDeferred() {
    let resolve;
    let reject;
    const promise = new Promise((res, rej) => {
        resolve = res;
        reject = rej;
    });
    return { promise, resolve, reject };
}

function rejectUnexpectedRequest({ method, url }) {
    const path = new URL(String(url), 'http://v1simple.test').pathname;
    throw new Error(`Unexpected settings fixture request: ${method} ${path}`);
}

function installDefaultFetch(
    overrides = [],
    { slots = defaultSlots, wifiStatus = undefined } = {}
) {
    const wifiStatusMatchers =
        wifiStatus === undefined
            ? apiFixtureMatchers('wifi_status_default')
            : [
                  {
                      method: 'GET',
                      match: '/api/wifi/status',
                      respond: jsonResponse(wifiStatus)
                  }
              ];
    return installFetchMock(
        [
            ...overrides,
            ...wifiStatusMatchers,
            {
                method: 'GET',
                match: '/api/device/settings',
                respond: jsonResponse({ ap_ssid: 'V1' })
            },
            { method: 'GET', match: '/api/wifi/networks', respond: jsonResponse({ slots }) },
            {
                method: 'GET',
                match: '/api/wifi/scan',
                respond: jsonResponse({ scanning: false, networks: [] })
            },
            {
                method: 'GET',
                match: '/api/status',
                respond: jsonResponse({ time: { valid: false } })
            },
            {
                method: 'POST',
                match: '/api/device/settings',
                respond: jsonResponse({ success: true })
            },
            { method: 'POST', match: '/api/wifi/enable', respond: jsonResponse({ success: true }) },
            {
                method: 'POST',
                match: '/api/wifi/disconnect',
                respond: jsonResponse({ success: true })
            },
            {
                method: 'POST',
                match: '/api/wifi/scan',
                respond: jsonResponse({ scanning: false, networks: [] })
            },
            {
                method: 'POST',
                match: (url) => url === '/api/wifi/networks/delete',
                respond: jsonResponse({ success: true })
            },
            {
                method: 'POST',
                match: (url) => url === '/api/wifi/networks/test',
                respond: jsonResponse({ success: true })
            },
            {
                method: 'POST',
                match: (url) => url === '/api/wifi/networks',
                respond: jsonResponse({ success: true, index: 0 })
            },
            {
                method: 'POST',
                match: (url) => url === '/api/wifi/networks/priorities',
                respond: jsonResponse({ success: true })
            }
        ],
        rejectUnexpectedRequest
    );
}

async function selectRestoreFile(contents, name = 'backup.json') {
    await screen.findByRole('button', { name: /restore from backup/i });
    const input = document.querySelector('input[type="file"]');
    expect(input).not.toBeNull();
    const file = new File([contents], name, { type: 'application/json' });
    await fireEvent.change(input, { target: { files: [file] } });
    return file;
}

async function startRestore(contents, name = 'backup.json') {
    await selectRestoreFile(contents, name);
    await fireEvent.click(await screen.findByRole('button', { name: /restore from backup/i }));
}

async function openManualEditor() {
    const buttons = await screen.findAllByRole('button', { name: /enter manually/i });
    await fireEvent.click(buttons[0]);
    await screen.findByText('Add Saved Network');
}

async function openScanModal() {
    const buttons = await screen.findAllByRole('button', { name: /pick from scan/i });
    await fireEvent.click(buttons[0]);
    await screen.findByText('Pick WiFi Network to Save');
}

async function performSavedWifiOperation(operation) {
    if (operation === 'manual save') {
        await openManualEditor();
        await fireEvent.input(screen.getByLabelText('SSID'), { target: { value: 'New network' } });
        const save = screen.getByRole('button', { name: /^Save$/ });
        expect(save).toBeEnabled();
        await fireEvent.click(save);
    } else if (operation === 'scan save') {
        await openScanModal();
        await fireEvent.click(await screen.findByRole('button', { name: /Open network/ }));
        await fireEvent.click(screen.getByRole('button', { name: /^Save Network$/ }));
    } else if (operation === 'reorder') {
        await fireEvent.click(screen.getByRole('button', { name: 'Move PhoneHotspot up' }));
    } else if (operation === 'restore') {
        await startRestore(JSON.stringify({ _type: 'v1simple_backup', _version: 1 }));
    } else {
        const card = screen.getByText('WiFi Client').closest('.surface-card');
        await fireEvent.click(card.querySelector('input[type="checkbox"]'));
    }
}

const wifiStatusMutationPaths = ['manual save', 'scan save', 'delete', 'restore', 'disconnect'];
const connectedWifiStatus = {
    enabled: true,
    state: 'connected',
    connectedSSID: 'HomeWifi',
    connectedSlotIndex: 0,
    rssi: -48
};

function wifiClientToggle() {
    return screen.getByText('WiFi Client').closest('.surface-card').querySelector('input[type="checkbox"]');
}

async function performWifiStatusMutation(operation) {
    if (operation === 'manual save') {
        await fireEvent.click(screen.getAllByRole('button', { name: 'Edit' })[0]);
        await fireEvent.input(screen.getByLabelText('SSID'), { target: { value: 'ReplacementWifi' } });
        await fireEvent.click(screen.getByRole('button', { name: /^Save$/ }));
    } else if (operation === 'scan save') {
        await openScanModal();
        // An existing SSID updates its active slot, including its password.
        await fireEvent.click(await screen.findByRole('button', { name: /HomeWifi.*dBm/ }));
        await fireEvent.input(screen.getByLabelText('Password'), { target: { value: 'placeholder' } });
        await fireEvent.click(screen.getByRole('button', { name: /^Save Network$/ }));
    } else if (operation === 'delete') {
        await fireEvent.click(screen.getAllByRole('button', { name: 'Delete' })[0]);
    } else if (operation === 'restore') {
        await startRestore(JSON.stringify({ _type: 'v1simple_backup', _version: 1, wifiClientEnabled: false }));
    } else {
        await fireEvent.click(screen.getByRole('button', { name: 'Disconnect' }));
    }
}

function installWifiStatusMutationFetch(operation, {
    pendingStatus = false,
    outcome = 'success',
    pendingMutation = null
} = {}) {
    const oldStatus = createDeferred();
    const state = { committed: false, statusReads: 0, mutations: 0 };
    const mutationUrl = {
        'manual save': '/api/wifi/networks',
        'scan save': '/api/wifi/networks',
        delete: '/api/wifi/networks/delete',
        restore: '/api/settings/restore',
        disconnect: '/api/wifi/disconnect'
    }[operation];
    const fetchMock = installDefaultFetch([
        { method: 'GET', match: '/api/wifi/status', respond: () => {
            state.statusReads += 1;
            if (pendingStatus && state.statusReads === 2) return oldStatus.promise;
            return jsonResponse(!state.committed ? connectedWifiStatus : {
                enabled: operation !== 'restore',
                state: operation === 'restore' ? 'disabled' : 'disconnected'
            });
        } },
        { method: 'GET', match: '/api/wifi/networks', respond: () => {
            const slots = state.committed && operation === 'delete'
                ? defaultSlots.map((slot) => slot.index === 0 ? { ...slot, ssid: '', label: '', configured: false } : slot)
                : defaultSlots;
            return jsonResponse({ slots });
        } },
        { method: 'POST', match: (url) => url === mutationUrl, respond: () => {
            state.mutations += 1;
            if (pendingMutation) return pendingMutation.promise;
            // A server can commit even when its reply is lost or unsuccessful.
            state.committed = true;
            if (outcome === 'transport error') throw new Error('mutation reply lost');
            return jsonResponse({ success: outcome === 'success' }, outcome === 'HTTP error' ? 500 : 200);
        } },
        { method: 'POST', match: '/api/wifi/scan', respond: jsonResponse({
            scanning: false,
            networks: [{ ssid: 'HomeWifi', secure: true, rssi: -35 }]
        }) }
    ]);
    vi.stubGlobal('confirm', vi.fn(() => true));
    return { fetchMock, oldStatus, state };
}

async function expectReconciledWifiStatus(operation) {
    await waitFor(() => {
        expect(screen.queryByRole('button', { name: 'Disconnect' })).not.toBeInTheDocument();
        expect(screen.queryByText(/Connected to HomeWifi •/)).not.toBeInTheDocument();
        if (operation === 'restore') expect(wifiClientToggle()).not.toBeChecked();
        else expect(wifiClientToggle()).toBeChecked();
    });
    expect(screen.getByText(operation === 'restore'
        ? /WiFi client is disabled/i
        : 'Saved networks will be tried in priority order while AP stays online.')).toBeInTheDocument();
}

describe('settings route page', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
    });

    it('loads settings, wifi status, and saved network slots', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('Settings');
        await screen.findByText('Saved Networks');
        await screen.findByText('Garage');
        await screen.findByText('Phone');
        await screen.findByText('Slot 3: Empty slot');
        expect(screen.getAllByText(/connected previously/i)).toHaveLength(2);
        expect(screen.queryByText(/last connected \d+s/i)).not.toBeInTheDocument();
        expect(fetchMock.mock.calls.some(([url]) => url === '/api/device/settings')).toBe(true);
        expect(fetchMock.mock.calls.some(([url]) => url === '/api/wifi/status')).toBe(true);
        expect(fetchMock.mock.calls.some(([url]) => url === '/api/wifi/networks')).toBe(true);
        expect(screen.queryByText('Bluetooth Proxy')).not.toBeInTheDocument();

        unmount();
    });

    it('preserves the legacy AP timeout without offering an ineffective control', async () => {
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/device/settings',
                respond: jsonResponse({
                    ap_ssid: 'V1',
                    ap_password: '********',
                    autoPowerOffMinutes: 30,
                    apTimeoutMinutes: 15
                })
            }
        ]);
        const { unmount } = render(Page);

        try {
            const name = await screen.findByLabelText('AP Name');
            expect(screen.queryByRole('checkbox', {
                name: /disable ap inactivity timeout/i
            })).not.toBeInTheDocument();
            expect(screen.queryByLabelText('Auto-off after (minutes)')).not.toBeInTheDocument();
            expect(screen.queryByText(/AP will turn off after/i)).not.toBeInTheDocument();
            expect(screen.getByText(/returns to normal mode after 10 idle minutes or 30 minutes total/i))
                .toBeInTheDocument();

            await fireEvent.input(name, { target: { value: 'UpdatedAP' } });
            await fireEvent.click(screen.getByRole('button', { name: /save settings/i }));
            await screen.findByText('Settings saved! New AP credentials apply the next time the AP starts.');
            const post = fetchMock.mock.calls.find(([url, init]) =>
                url === '/api/device/settings' && init?.method === 'POST');
            expect(Object.fromEntries(post[1].body)).toEqual({
                ap_ssid: 'UpdatedAP',
                ap_password: '********',
                autoPowerOffMinutes: '30',
                apTimeoutMinutes: '15'
            });
        } finally {
            unmount();
        }
    });

    it('shows load error when settings endpoint fails', async () => {
        installFetchMock(
            [
                {
                    method: 'GET',
                    match: '/api/device/settings',
                    respond: () => Promise.reject(new Error('boom'))
                },
                {
                    method: 'GET',
                    match: '/api/wifi/status',
                    respond: jsonResponse({ enabled: false, state: 'disabled' })
                },
                {
                    method: 'GET',
                    match: '/api/wifi/networks',
                    respond: jsonResponse({ slots: [] })
                },
                {
                    method: 'GET',
                    match: '/api/status',
                    respond: jsonResponse({ time: { valid: false } })
                }
            ],
            jsonResponse({})
        );
        const { unmount } = render(Page);

        await screen.findByText('Failed to load settings');
        unmount();
    });

    it.each([
        ['connection failure', false],
        ['API failure', true]
    ])('requires a successful settings retry after %s before saving', async (failure, disablePowerOff) => {
        let reads = 0;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/device/settings',
                respond: () => {
                    if (++reads <= 2) {
                        return failure === 'connection failure'
                            ? Promise.reject(new Error('settings unavailable'))
                            : jsonResponse({ error: 'settings unavailable' }, 500);
                    }
                    return jsonResponse({
                        ap_ssid: 'OriginalAP',
                        ap_password: '********',
                        autoPowerOffMinutes: 30,
                        apTimeoutMinutes: 15
                    });
                }
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Failed to load settings');
        const save = screen.getByRole('button', { name: /save settings/i });
        expect(save).toBeDisabled();
        expect(screen.getByLabelText('AP Name')).toBeDisabled();
        expect(screen.getByLabelText(/minutes after disconnect/i)).toBeDisabled();
        await fireEvent.click(save);
        expect(fetchMock.mock.calls.some(([url, init]) =>
            url === '/api/device/settings' && init?.method === 'POST')).toBe(false);
        await screen.findByText('Garage');
        expect(screen.getByRole('button', { name: /download backup/i })).toBeEnabled();

        await fireEvent.click(screen.getByRole('button', { name: /retry settings/i }));
        await screen.findByRole('button', { name: /retry settings/i });
        expect(screen.getByText('Failed to load settings')).toBeInTheDocument();
        expect(screen.getByRole('button', { name: /save settings/i })).toBeDisabled();
        await fireEvent.click(screen.getByRole('button', { name: /retry settings/i }));
        await screen.findByDisplayValue('OriginalAP');
        expect(screen.queryByText('Failed to load settings')).not.toBeInTheDocument();
        expect(screen.getByLabelText(/minutes after disconnect/i)).toHaveValue(30);

        await fireEvent.input(screen.getByLabelText('AP Name'), { target: { value: 'ChosenAP' } });
        await fireEvent.input(screen.getByLabelText('AP Password'), {
            target: { value: 'ChosenPassword' }
        });
        if (disablePowerOff) {
            await fireEvent.input(screen.getByLabelText(/minutes after disconnect/i), {
                target: { value: '0' }
            });
        }
        await fireEvent.click(screen.getByRole('button', { name: /save settings/i }));
        await screen.findByText('Settings saved! New AP credentials apply the next time the AP starts.');
        const posts = fetchMock.mock.calls.filter(([url, init]) =>
            url === '/api/device/settings' && init?.method === 'POST');
        expect(posts).toHaveLength(1);
        expect(Object.fromEntries(posts[0][1].body)).toEqual({
            ap_ssid: 'ChosenAP',
            ap_password: 'ChosenPassword',
            autoPowerOffMinutes: disablePowerOff ? '0' : '30',
            apTimeoutMinutes: '15'
        });

        unmount();
    });

    it('shows WiFi status load errors in the page alert', async () => {
        installFetchMock(
            [
                {
                    method: 'GET',
                    match: '/api/device/settings',
                    respond: jsonResponse({ ap_ssid: 'V1' })
                },
                {
                    method: 'GET',
                    match: '/api/wifi/status',
                    respond: jsonResponse({ error: 'bad wifi' }, 500)
                },
                {
                    method: 'GET',
                    match: '/api/wifi/networks',
                    respond: jsonResponse({ slots: defaultSlots })
                },
                {
                    method: 'GET',
                    match: '/api/status',
                    respond: jsonResponse({ time: { valid: false } })
                }
            ],
            jsonResponse({})
        );
        const { unmount } = render(Page);

        await screen.findByText('Failed to load WiFi status');
        expect(screen.getByText('Settings')).toBeInTheDocument();

        unmount();
    });

    it('manual add dialog accepts label, SSID, password, and priority', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await openManualEditor();
        await fireEvent.input(screen.getByLabelText('Label'), { target: { value: 'Car hotspot' } });
        await fireEvent.input(screen.getByLabelText('SSID'), { target: { value: 'CarAP' } });
        await fireEvent.input(screen.getByLabelText('Password'), {
            target: { value: 'secret123' }
        });
        await fireEvent.input(screen.getByLabelText('Priority'), { target: { value: '0' } });
        await fireEvent.click(screen.getByRole('button', { name: /^Save$/i }));

        await screen.findByText('Saved CarAP');
        const bodies = jsonBodies(fetchMock, '/api/wifi/networks');
        expect(bodies.at(-1)).toMatchObject({
            ssid: 'CarAP',
            label: 'Car hotspot',
            password: 'secret123',
            priority: 0
        });

        unmount();
    });

    it('manual add keeps exact whitespace-only SSID bytes valid', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await openManualEditor();
        const ssid = screen.getByLabelText('SSID');
        const save = screen.getByRole('button', { name: /^Save$/i });
        expect(save).toBeDisabled();

        await fireEvent.input(ssid, { target: { value: 'OrdinaryNetwork' } });
        expect(save).toBeEnabled();
        await fireEvent.input(ssid, { target: { value: '   ' } });
        expect(save).toBeEnabled();
        await fireEvent.click(save);

        await waitFor(() => {
            expect(jsonBodies(fetchMock, '/api/wifi/networks').at(-1)?.ssid).toBe('   ');
        });

        unmount();
    });

    it('toggles saved-network password visibility in the manual editor', async () => {
        installDefaultFetch();
        const { unmount } = render(Page);

        await openManualEditor();
        const password = screen.getByLabelText('Password');
        expect(password).toHaveAttribute('type', 'password');

        await fireEvent.click(screen.getByRole('button', { name: /^Show$/i }));
        expect(password).toHaveAttribute('type', 'text');

        await fireEvent.click(screen.getByRole('button', { name: /^Hide$/i }));
        expect(password).toHaveAttribute('type', 'password');

        unmount();
    });

    it('edit dialog keeps an existing password when password is left blank', async () => {
        const fetchMock = installDefaultFetch();
        const { unmount } = render(Page);

        await screen.findByText('Garage');
        await fireEvent.click(screen.getAllByRole('button', { name: /^Edit$/i })[0]);
        await screen.findByText('Edit Saved Network');
        await fireEvent.input(screen.getByLabelText('Label'), {
            target: { value: 'Garage Updated' }
        });
        await fireEvent.click(screen.getByRole('button', { name: /^Save$/i }));

        await screen.findByText('Saved HomeWifi');
        const body = jsonBodies(fetchMock, '/api/wifi/networks').at(-1);
        expect(body).toMatchObject({
            index: 0,
            ssid: 'HomeWifi',
            label: 'Garage Updated',
            priority: 0
        });
        expect(body).not.toHaveProperty('password');

        unmount();
    });

    it('allows a label-only edit of an existing whitespace-only SSID', async () => {
        const slots = [
            {
                index: 0,
                ssid: '   ',
                label: 'Whitespace',
                priority: 0,
                hasPassword: true,
                lastConnectedAtSec: 0,
                configured: true
            }
        ];
        const fetchMock = installDefaultFetch([], { slots });
        const { unmount } = render(Page);

        await screen.findByText('Whitespace');
        await fireEvent.click(screen.getByRole('button', { name: /^Edit$/i }));
        await screen.findByText('Edit Saved Network');
        expect(screen.getByLabelText('SSID')).toHaveValue('   ');
        await fireEvent.input(screen.getByLabelText('Label'), {
            target: { value: 'Whitespace Updated' }
        });
        const save = screen.getByRole('button', { name: /^Save$/i });
        expect(save).toBeEnabled();
        await fireEvent.click(save);

        await waitFor(() => {
            expect(jsonBodies(fetchMock, '/api/wifi/networks').at(-1)).toMatchObject({
                index: 0,
                ssid: '   ',
                label: 'Whitespace Updated',
                priority: 0
            });
        });

        unmount();
    });

    it('deletes a saved network slot', async () => {
        const fetchMock = installDefaultFetch();
        vi.stubGlobal(
            'confirm',
            vi.fn(() => true)
        );
        const { unmount } = render(Page);

        await screen.findByText('Garage');
        await fireEvent.click(screen.getAllByRole('button', { name: /^Delete$/i })[0]);

        await screen.findByText('Deleted HomeWifi');
        expect(jsonBodies(fetchMock, '/api/wifi/networks/delete').at(-1)).toEqual({ index: 0 });

        unmount();
    });

    it.each(['success', 'HTTP error', 'transport error'])(
        'keeps both completed deletions after an older saved-network read returns %s',
        async (outcome) => {
            const oldRead = createDeferred();
            let reads = 0;
            let statusReads = 0;
            const emptySlots = defaultSlots.map((slot) => ({ ...slot, ssid: '', label: '', configured: false }));
            const fetchMock = installDefaultFetch([
                { method: 'GET', match: '/api/wifi/networks', respond: () => {
                    reads += 1;
                    if (reads === 2) return oldRead.promise;
                    return jsonResponse({ slots: reads === 1 ? defaultSlots : emptySlots });
                } },
                { method: 'GET', match: '/api/wifi/status', respond: () => {
                    statusReads += 1;
                    return jsonResponse({ enabled: true, state: 'disconnected' });
                } }
            ]);
            vi.stubGlobal('confirm', vi.fn(() => true));
            const { unmount } = render(Page);
            try {
                await screen.findByText('Garage');
                const deletes = screen.getAllByRole('button', { name: 'Delete' });
                await fireEvent.click(deletes[0]);
                await waitFor(() => expect(reads).toBe(2));
                expect(deletes[1]).toBeEnabled();
                await fireEvent.click(deletes[1]);
                await screen.findByText('Slot 2: Empty slot');
                // Status refreshes are independent of the pending list read.
                await waitFor(() => expect(statusReads).toBe(3));
                if (outcome === 'transport error') oldRead.reject(new Error('offline'));
                else oldRead.resolve(jsonResponse({ slots: [emptySlots[0], ...defaultSlots.slice(1)] }, outcome === 'HTTP error' ? 500 : 200));
                await new Promise((resolve) => setTimeout(resolve, 0));
                expect(statusReads).toBe(3);
                expect(screen.queryByText('Phone')).not.toBeInTheDocument();
                expect(screen.queryByText('Failed to load saved WiFi networks')).not.toBeInTheDocument();
                expect(jsonBodies(fetchMock, '/api/wifi/networks/delete')).toEqual([{ index: 0 }, { index: 1 }]);
            } finally { unmount(); }
        }
    );

    it('invalidates an old saved-network read before the next mutation finishes', async () => {
        const oldRead = createDeferred();
        const secondDelete = createDeferred();
        let reads = 0;
        let deletes = 0;
        let statusReads = 0;
        installDefaultFetch([
            { method: 'GET', match: '/api/wifi/networks', respond: () => {
                reads += 1;
                return reads === 2 ? oldRead.promise : jsonResponse({ slots: defaultSlots });
            } },
            { method: 'POST', match: '/api/wifi/networks/delete', respond: () => {
                deletes += 1;
                return deletes === 2 ? secondDelete.promise : jsonResponse({ success: true });
            } },
            { method: 'GET', match: '/api/wifi/status', respond: () => {
                statusReads += 1;
                return jsonResponse({ enabled: true, state: 'disconnected' });
            } }
        ]);
        vi.stubGlobal('confirm', vi.fn(() => true));
        const { unmount } = render(Page);
        try {
            await screen.findByText('Garage');
            const buttons = screen.getAllByRole('button', { name: 'Delete' });
            await fireEvent.click(buttons[0]);
            await waitFor(() => expect(reads).toBe(2));
            await fireEvent.click(buttons[1]);
            oldRead.resolve(jsonResponse({ slots: [] }));
            await waitFor(() => expect(statusReads).toBe(2));
            // The second operation owns the next authoritative read. An older
            // completion must not publish or unlock an editor in the meantime.
            expect(screen.getByText('Garage')).toBeInTheDocument();
            await openManualEditor();
            await fireEvent.input(screen.getByLabelText('SSID'), { target: { value: 'Independent draft' } });
            expect(screen.getByRole('button', { name: /^Save$/ })).toBeDisabled();
            secondDelete.resolve(jsonResponse({ success: false }, 500));
            await waitFor(() => expect(reads).toBe(3));
            await waitFor(() => expect(screen.getByRole('button', { name: /^Save$/ })).toBeEnabled());
            expect(screen.getByLabelText('SSID')).toHaveValue('Independent draft');
            expect(screen.getByText('Failed to delete WiFi network')).toBeInTheDocument();
        } finally { unmount(); }
    });

    it('reconciles an earlier-started deletion that commits after a newer deletion', async () => {
        const firstDelete = createDeferred();
        let reads = 0;
        let deletes = 0;
        const emptySlots = defaultSlots.map((slot) => ({ ...slot, ssid: '', label: '', configured: false }));
        installDefaultFetch([
            { method: 'POST', match: '/api/wifi/networks/delete', respond: () => {
                deletes += 1;
                return deletes === 1 ? firstDelete.promise : jsonResponse({ success: true });
            } },
            { method: 'GET', match: '/api/wifi/networks', respond: () => {
                reads += 1;
                const slots = reads === 1 ? defaultSlots : reads === 2 ? [defaultSlots[0], ...emptySlots.slice(1)] : emptySlots;
                return jsonResponse({ slots });
            } }
        ]);
        vi.stubGlobal('confirm', vi.fn(() => true));
        const { unmount } = render(Page);
        try {
            await screen.findByText('Garage');
            const deletes = screen.getAllByRole('button', { name: 'Delete' });
            await fireEvent.click(deletes[0]);
            await fireEvent.click(deletes[1]);
            await screen.findByText('Slot 2: Empty slot');
            expect(screen.getByText('Garage')).toBeInTheDocument();
            firstDelete.resolve(jsonResponse({ success: true }));
            await screen.findByText('Slot 1: Empty slot');
            expect(screen.queryByText('Garage')).not.toBeInTheDocument();
            expect(screen.queryByText('Phone')).not.toBeInTheDocument();
        } finally { unmount(); }
    });

    it('keeps the latest saved-network read error when an older success arrives, then recovers', async () => {
        const oldRead = createDeferred();
        let reads = 0;
        let statusReads = 0;
        const emptySlots = defaultSlots.map((slot) => ({ ...slot, ssid: '', label: '', configured: false }));
        installDefaultFetch([
            { method: 'GET', match: '/api/wifi/networks', respond: () => {
                reads += 1;
                if (reads === 2) return oldRead.promise;
                if (reads === 3) return jsonResponse({}, 500);
                return jsonResponse({ slots: reads === 1 ? defaultSlots : emptySlots });
            } },
            { method: 'GET', match: '/api/wifi/status', respond: () => {
                statusReads += 1;
                return jsonResponse({ enabled: true, state: 'disconnected' });
            } }
        ]);
        vi.stubGlobal('confirm', vi.fn(() => true));
        const { unmount } = render(Page);
        try {
            await screen.findByText('Garage');
            const deletes = screen.getAllByRole('button', { name: 'Delete' });
            await fireEvent.click(deletes[0]);
            await waitFor(() => expect(reads).toBe(2));
            await fireEvent.click(deletes[1]);
            await screen.findByText('Failed to load saved WiFi networks');
            await waitFor(() => expect(statusReads).toBe(3));
            oldRead.resolve(jsonResponse({ slots: [] }));
            await new Promise((resolve) => setTimeout(resolve, 0));
            expect(statusReads).toBe(3);
            expect(screen.getByText('Failed to load saved WiFi networks')).toBeInTheDocument();
            expect(screen.getByText('Garage')).toBeInTheDocument();
            // A later explicit operation reconciles both committed deletions.
            const card = screen.getByText('WiFi Client').closest('.surface-card');
            await fireEvent.click(card.querySelector('input[type="checkbox"]'));
            await screen.findByText('Slot 1: Empty slot');
            expect(screen.queryByText('Failed to load saved WiFi networks')).not.toBeInTheDocument();
        } finally { unmount(); }
    });

    it.each(['manual save', 'scan save', 'reorder', 'restore', 'toggle'])(
        'protects the saved-network refresh from %s against a stale sibling read',
        async (operation) => {
            const oldRead = createDeferred();
            let reads = 0;
            let statusReads = 0;
            const initialIsOrder = operation === 'manual save';
            installDefaultFetch([
                { method: 'GET', match: '/api/wifi/networks', respond: () => {
                    reads += 1;
                    return reads === 2 ? oldRead.promise : jsonResponse({ slots: defaultSlots });
                } },
                { method: 'GET', match: '/api/wifi/status', respond: () => {
                    statusReads += 1;
                    return jsonResponse({ enabled: true, state: 'disconnected' });
                } },
                { method: 'POST', match: '/api/wifi/scan', respond: jsonResponse({ scanning: false, networks: [{ ssid: 'Open network', secure: false, rssi: -30 }] }) },
                { method: 'POST', match: '/api/settings/restore', respond: jsonResponse({ success: true }) }
            ]);
            vi.stubGlobal('confirm', vi.fn(() => true));
            const { unmount } = render(Page);
            try {
                await screen.findByText('Garage');
                await fireEvent.click(initialIsOrder ? screen.getByRole('button', { name: 'Move PhoneHotspot up' }) : screen.getAllByRole('button', { name: 'Delete' })[0]);
                await waitFor(() => expect(reads).toBe(2));
                await performSavedWifiOperation(operation);
                await waitFor(() => expect(reads).toBe(3));
                // Wait until the latest response has published before the old
                // response reports a failure. Both callbacks enter real UI code.
                await waitFor(() => expect(screen.getByText('Saved Networks').parentElement.querySelector('.loading')).toBeNull());
                const expectedStatusReads = statusReads;
                oldRead.resolve(jsonResponse({}, 500));
                if (initialIsOrder) {
                    await waitFor(() => expect(screen.getByRole('button', { name: 'Move PhoneHotspot up' })).toBeEnabled());
                } else {
                    await new Promise((resolve) => setTimeout(resolve, 0));
                    expect(statusReads).toBe(expectedStatusReads);
                }
                expect(screen.queryByText('Failed to load saved WiFi networks')).not.toBeInTheDocument();
                expect(screen.getByText('Garage')).toBeInTheDocument();
            } finally { unmount(); }
        }
    );

    it.each(['HTTP error', 'transport error'])(
        'reconciles saved networks after an ambiguous deletion %s',
        async (outcome) => {
            let reads = 0;
            installDefaultFetch([
                { method: 'GET', match: '/api/wifi/networks', respond: () => {
                    reads += 1;
                    return jsonResponse({ slots: reads === 1 ? defaultSlots : [] });
                } },
                { method: 'POST', match: '/api/wifi/networks/delete', respond: () => {
                    if (outcome === 'transport error') throw new Error('reply lost');
                    return jsonResponse({}, 500);
                } }
            ]);
            vi.stubGlobal('confirm', vi.fn(() => true));
            const { unmount } = render(Page);
            try {
                await screen.findByText('Garage');
                await fireEvent.click(screen.getAllByRole('button', { name: 'Delete' })[0]);
                await screen.findByText('No saved WiFi networks yet. Add a car hotspot, phone hotspot, or garage AP so future maintenance sessions can auto-join.');
                expect(screen.getByText('Failed to delete WiFi network')).toBeInTheDocument();
                expect(screen.queryByText('Garage')).not.toBeInTheDocument();
                expect(reads).toBe(2);
            } finally { unmount(); }
        }
    );

    it('keeps a manual save newer than the initial saved-network load', async () => {
        const initialRead = createDeferred();
        let reads = 0;
        const fetchMock = installDefaultFetch([
            { method: 'GET', match: '/api/wifi/networks', respond: () => {
                reads += 1;
                return reads === 1 ? initialRead.promise : jsonResponse({ slots: [{ ...defaultSlots[0], label: 'New network' }] });
            } }
        ]);
        const { unmount } = render(Page);
        try {
            await openManualEditor();
            await waitFor(() => expect(reads).toBe(1));
            await fireEvent.input(screen.getByLabelText('SSID'), { target: { value: 'New network' } });
            await fireEvent.click(screen.getByRole('button', { name: /^Save$/ }));
            await screen.findByText('New network');
            initialRead.resolve(jsonResponse({ slots: defaultSlots }));
            // Drain the older fetch body and Svelte update without triggering a
            // third request that could hide a stale publication.
            await new Promise((resolve) => setTimeout(resolve, 0));
            expect(screen.getByText('New network')).toBeInTheDocument();
            expect(screen.queryByText('Garage')).not.toBeInTheDocument();
            expect(jsonBodies(fetchMock, '/api/wifi/networks')).toHaveLength(1);
            expect(reads).toBe(2);
        } finally { unmount(); }
    });

    it.each(['manual save', 'scan save', 'reorder', 'restore', 'toggle'])(
        'keeps a failed latest saved-network read visible after %s succeeds',
        async (operation) => {
            let reads = 0;
            installDefaultFetch([
                { method: 'GET', match: '/api/wifi/networks', respond: () => {
                    reads += 1;
                    return jsonResponse({ slots: defaultSlots }, reads === 1 ? 200 : 500);
                } },
                { method: 'POST', match: '/api/wifi/scan', respond: jsonResponse({ scanning: false, networks: [{ ssid: 'Open network', secure: false, rssi: -30 }] }) },
                { method: 'POST', match: '/api/settings/restore', respond: jsonResponse({ success: true }) }
            ]);
            vi.stubGlobal('confirm', vi.fn(() => true));
            const { unmount } = render(Page);
            try {
                await screen.findByText('Garage');
                await performSavedWifiOperation(operation);
                await screen.findByText('Failed to load saved WiFi networks');
                // Include the caller's continuation after its awaited refresh.
                await new Promise((resolve) => setTimeout(resolve, 0));
                expect(screen.getByText('Failed to load saved WiFi networks')).toBeInTheDocument();
                expect(reads).toBe(2);
            } finally { unmount(); }
        }
    );

    it.each([
        ['manual save', 'HTTP error'], ['manual save', 'transport error'],
        ['scan save', 'HTTP error'], ['scan save', 'transport error'],
        ['restore', 'HTTP error'], ['restore', 'transport error']
    ])('reconciles after %s has an ambiguous %s without discarding its draft', async (operation, outcome) => {
        let reads = 0;
        installDefaultFetch([
            { method: 'GET', match: '/api/wifi/networks', respond: () => {
                reads += 1;
                return jsonResponse({ slots: reads === 1 ? defaultSlots : [{ ...defaultSlots[0], label: 'Reconciled network' }] });
            } },
            { method: 'POST', match: '/api/wifi/scan', respond: jsonResponse({ scanning: false, networks: [{ ssid: 'Open network', secure: false, rssi: -30 }] }) },
            { method: 'POST', match: (url) => ['/api/wifi/networks', '/api/settings/restore'].includes(url), respond: () => {
                if (outcome === 'transport error') throw new Error('reply lost');
                return jsonResponse({ success: false }, 500);
            } }
        ]);
        vi.stubGlobal('confirm', vi.fn(() => true));
        const { unmount } = render(Page);
        try {
            await screen.findByText('Garage');
            await performSavedWifiOperation(operation);
            await screen.findByText('Reconciled network');
            expect(reads).toBe(2);
            if (operation === 'manual save') {
                expect(screen.getByLabelText('SSID')).toHaveValue('New network');
                await waitFor(() => expect(screen.getByRole('button', { name: /^Save$/ })).toBeEnabled());
            } else if (operation === 'scan save') {
                await waitFor(() => expect(screen.getByRole('button', { name: /^Save Network$/ })).toBeEnabled());
            } else {
                await waitFor(() => expect(screen.getByRole('button', { name: /restore from backup/i })).toBeEnabled());
            }
            expect(screen.queryByText('Settings restored and reloaded.')).not.toBeInTheDocument();
        } finally { unmount(); }
    });

    it.each(['read', 'mutation'])(
        'does not continue saved-network work after navigation during a pending %s',
        async (pending) => {
            const response = createDeferred();
            let reads = 0;
            const fetchMock = installDefaultFetch([
                { method: 'GET', match: '/api/wifi/networks', respond: () => {
                    reads += 1;
                    return pending === 'read' && reads === 2 ? response.promise : jsonResponse({ slots: defaultSlots });
                } },
                { method: 'POST', match: '/api/wifi/networks/delete', respond: () => pending === 'mutation' ? response.promise : jsonResponse({ success: true }) }
            ]);
            vi.stubGlobal('confirm', vi.fn(() => true));
            const { unmount } = render(Page);
            await screen.findByText('Garage');
            await fireEvent.click(screen.getAllByRole('button', { name: 'Delete' })[0]);
            if (pending === 'read') await waitFor(() => expect(reads).toBe(2));
            unmount();
            const calls = fetchMock.mock.calls.length;
            response.resolve(jsonResponse({ slots: [], success: true }));
            await new Promise((resolve) => setTimeout(resolve, 0));
            expect(fetchMock.mock.calls).toHaveLength(calls);
            expect(document.body).not.toHaveTextContent('Deleted HomeWifi');
        }
    );

    it.each(['manual save', 'scan save'])(
        'refreshes connection status when %s changes the active saved network',
        async (operation) => {
            const { fetchMock, state } = installWifiStatusMutationFetch(operation);
            const { unmount } = render(Page);
            try {
                await screen.findByText(/Connected to HomeWifi • Slot 1/);
                await performWifiStatusMutation(operation);
                await expectReconciledWifiStatus(operation);
                expect(state.statusReads).toBe(2);
                const body = jsonBodies(fetchMock, '/api/wifi/networks').at(-1);
                expect(body).toMatchObject(operation === 'manual save'
                    ? { index: 0, ssid: 'ReplacementWifi' }
                    : { ssid: 'HomeWifi', password: 'placeholder' });
            } finally { unmount(); }
        }
    );

    it.each(wifiStatusMutationPaths.flatMap((operation) =>
        ['success', 'HTTP error', 'transport error'].map((outcome) => [operation, outcome])
    ))('reconciles %s after %s without reusing a pre-mutation Test status', async (operation, outcome) => {
        const { oldStatus, state } = installWifiStatusMutationFetch(operation, { pendingStatus: true, outcome });
        const { unmount } = render(Page);
        try {
            await screen.findByText(/Connected to HomeWifi • Slot 1/);
            await fireEvent.click(screen.getAllByRole('button', { name: 'Test' })[0]);
            await waitFor(() => expect(state.statusReads).toBe(2));
            await performWifiStatusMutation(operation);
            await waitFor(() => expect(state.committed).toBe(true));
            oldStatus.resolve(jsonResponse(connectedWifiStatus));
            await expectReconciledWifiStatus(operation);
            expect(state.statusReads).toBe(3);
            // The Test consumer must not claim success from the obsolete reply.
            expect(screen.queryByText('Connected to HomeWifi')).not.toBeInTheDocument();
            if (outcome !== 'success') {
                const failureText = operation.includes('save')
                    ? 'Failed to save WiFi network'
                    : operation === 'delete' ? 'Failed to delete WiFi network'
                    : operation === 'disconnect' ? 'Failed to disconnect'
                    : outcome === 'transport error' ? 'Failed to read backup file' : 'Failed to restore backup';
                expect(screen.getByText(failureText)).toBeInTheDocument();
                if (operation === 'manual save') {
                    expect(screen.getByLabelText('SSID')).toHaveValue('ReplacementWifi');
                    await waitFor(() => expect(screen.getByRole('button', { name: /^Save$/ })).toBeEnabled());
                } else if (operation === 'scan save') {
                    expect(screen.getByLabelText('Password')).toHaveValue('placeholder');
                    await waitFor(() => expect(screen.getByRole('button', { name: /^Save Network$/ })).toBeEnabled());
                }
            }
        } finally { unmount(); }
    });

    it.each(wifiStatusMutationPaths)(
        'reads status after Test when %s has an older status request pending',
        async (operation) => {
            const testStatus = createDeferred();
            const { fetchMock, oldStatus, state } = installWifiStatusMutationFetch(operation, {
                pendingStatus: true
            });
            // Keep the existing mutation fixtures, but make the later Test own
            // the next status request after the held pre-Test response settles.
            const defaultFetch = fetchMock.getMockImplementation();
            let testStarted = false;
            fetchMock.mockImplementation((url, init) => {
                if (url === '/api/wifi/networks/test' && init?.method === 'POST') {
                    testStarted = true;
                    return Promise.resolve(jsonResponse({ success: true }));
                }
                if (url === '/api/wifi/status' && testStarted) {
                    state.statusReads += 1;
                    return testStatus.promise;
                }
                return defaultFetch(url, init);
            });
            const { unmount } = render(Page);
            try {
                await screen.findByText(/Connected to HomeWifi • Slot 1/);
                await performWifiStatusMutation(operation);
                await waitFor(() => expect(state.statusReads).toBe(2));
                const phone = screen.getByText('Phone').closest('.rounded-box');
                const testButton = [...phone.querySelectorAll('button')]
                    .find((button) => button.textContent.trim() === 'Test');
                expect(testButton).toBeEnabled();
                await fireEvent.click(testButton);
                await screen.findByText('Testing connection to PhoneHotspot...');
                expect(jsonBodies(fetchMock, '/api/wifi/networks/test')).toEqual([{ index: 1 }]);
                expect(state.statusReads).toBe(2);

                oldStatus.resolve(jsonResponse({
                    enabled: operation !== 'restore',
                    state: operation === 'restore' ? 'disabled' : 'disconnected'
                }));
                await waitFor(() => expect(state.statusReads).toBe(3));
                expect(screen.queryByText('Could not connect to PhoneHotspot')).not.toBeInTheDocument();
                expect(screen.getAllByRole('button', { name: 'Test' }).every((button) => button.disabled)).toBe(true);
                testStatus.resolve(jsonResponse({
                    enabled: true,
                    state: 'connected',
                    connectedSSID: 'PhoneHotspot',
                    connectedSlotIndex: 1
                }));
                await screen.findByText('Connected to PhoneHotspot');
                expect(wifiClientToggle()).toBeChecked();
                expect(screen.getAllByRole('button', { name: 'Test' }).every((button) => !button.disabled)).toBe(true);
                expect(state.statusReads).toBe(3);
            } finally {
                oldStatus.resolve(jsonResponse({ enabled: true, state: 'disconnected' }));
                testStatus.resolve(jsonResponse({ enabled: true, state: 'disconnected' }));
                unmount();
            }
        }
    );

    it.each(wifiStatusMutationPaths)(
        'finishes Test from current status when %s supersedes its pending reply',
        async (operation) => {
            vi.useFakeTimers();
            const { oldStatus, state } = installWifiStatusMutationFetch(operation, { pendingStatus: true });
            const { unmount } = render(Page);
            try {
                await screen.findByText(/Connected to HomeWifi • Slot 1/);
                await fireEvent.click(screen.getAllByRole('button', { name: 'Test' })[0]);
                await waitFor(() => expect(state.statusReads).toBe(2));
                await performWifiStatusMutation(operation);
                await waitFor(() => expect(state.committed).toBe(true));
                oldStatus.resolve(jsonResponse({
                    enabled: true,
                    state: 'connected',
                    connectedSSID: 'HomeWifi',
                    connectedSlotIndex: 0
                }));
                await expectReconciledWifiStatus(operation);
                expect(screen.queryByText('Connected to HomeWifi')).not.toBeInTheDocument();
                await vi.advanceTimersByTimeAsync(1000);
                await screen.findByText('Could not connect to HomeWifi');
                if (operation !== 'restore') {
                    expect(screen.getAllByRole('button', { name: 'Test' }).every((button) => !button.disabled)).toBe(true);
                }
                expect(state.statusReads).toBe(4);
            } finally { unmount(); }
        }
    );

    it('does not report the original network connected when its tested slot is replaced', async () => {
        vi.useFakeTimers();
        const testRequest = createDeferred();
        let savedSsid = 'HomeWifi';
        let testedSsid = null;
        let testStatusReads = 0;
        const fetchMock = installDefaultFetch([
            { method: 'POST', match: '/api/wifi/networks/test', respond: () => testRequest.promise },
            { method: 'POST', match: '/api/wifi/networks', respond: ({ init }) => {
                savedSsid = JSON.parse(init.body).ssid;
                return jsonResponse({ success: true, index: 0 });
            } },
            { method: 'GET', match: '/api/wifi/networks', respond: () => jsonResponse({
                    slots: defaultSlots.map((slot) => slot.index === 0
                        ? { ...slot, ssid: savedSsid }
                        : slot)
            }) },
            { method: 'GET', match: '/api/wifi/status', respond: () => {
                if (!testedSsid) return jsonResponse({ enabled: true, state: 'disconnected' });
                testStatusReads += 1;
                return jsonResponse(testStatusReads === 1
                    ? { enabled: true, state: 'connecting', savedSSID: testedSsid }
                    : { enabled: true, state: 'connected', connectedSSID: testedSsid, connectedSlotIndex: 0 });
            } }
        ]);
        const { unmount } = render(Page);
        try {
            await screen.findByText('Garage');
            await fireEvent.click(screen.getAllByRole('button', { name: 'Test' })[0]);
            await waitFor(() => expect(jsonBodies(fetchMock, '/api/wifi/networks/test'))
                .toEqual([{ index: 0 }]));
            expect(screen.getAllByRole('button', { name: 'Test' })[0]).toBeDisabled();

            // The UI permits editing while Test is pending. Model the save
            // reaching the device first; Test then reads that slot's current SSID.
            await performWifiStatusMutation('manual save');
            await screen.findByText('Saved ReplacementWifi');
            expect(jsonBodies(fetchMock, '/api/wifi/networks')).toEqual([
                expect.objectContaining({ index: 0, ssid: 'ReplacementWifi' })
            ]);
            testedSsid = savedSsid;
            testRequest.resolve(jsonResponse({ success: true, index: 0 }));
            await screen.findByText('Testing connection to HomeWifi...');
            await screen.findByText('Connecting to ReplacementWifi...');
            await vi.advanceTimersByTimeAsync(1000);

            expect(screen.getByText(/Connected to ReplacementWifi • Slot 1/)).toBeInTheDocument();
            expect(screen.queryByText('Connected to HomeWifi')).not.toBeInTheDocument();
            expect(screen.getByText(
                'Connection test reached ReplacementWifi, not HomeWifi'
            )).toBeInTheDocument();
            expect(screen.getAllByRole('button', { name: 'Test' })
                .every((button) => !button.disabled)).toBe(true);
        } finally { unmount(); }
    });

    it('keeps Test polling when a different saved slot is deleted', async () => {
        vi.useFakeTimers();
        const { fetchMock, oldStatus, state } = installWifiStatusMutationFetch('delete', { pendingStatus: true });
        const defaultFetch = fetchMock.getMockImplementation();
        fetchMock.mockImplementation((url, init) => {
            if (url === '/api/wifi/status' && state.committed) {
                state.statusReads += 1;
                return Promise.resolve(jsonResponse(state.statusReads === 3
                    ? { enabled: true, state: 'connecting', savedSSID: 'PhoneHotspot' }
                    : { enabled: true, state: 'connected', connectedSSID: 'PhoneHotspot', connectedSlotIndex: 1 }));
            }
            return defaultFetch(url, init);
        });
        const { unmount } = render(Page);
        try {
            await screen.findByText(/Connected to HomeWifi • Slot 1/);
            await fireEvent.click(screen.getAllByRole('button', { name: 'Test' })[1]);
            await waitFor(() => expect(state.statusReads).toBe(2));
            await performWifiStatusMutation('delete');
            await waitFor(() => expect(state.committed).toBe(true));
            oldStatus.resolve(jsonResponse({ enabled: true, state: 'failed' }));
            await screen.findByText(/Connecting to PhoneHotspot/);
            expect(screen.queryByText('Could not connect to PhoneHotspot')).not.toBeInTheDocument();
            await vi.advanceTimersByTimeAsync(1000);
            await screen.findByText('Connected to PhoneHotspot');
            expect(screen.getAllByRole('button', { name: 'Test' }).every((button) => !button.disabled)).toBe(true);
            expect(state.statusReads).toBe(4);
        } finally { unmount(); }
    });

    it.each(wifiStatusMutationPaths)(
        'does not launch WiFi status reconciliation when %s settles after unmount',
        async (operation) => {
            const pendingMutation = createDeferred();
            const { fetchMock, state } = installWifiStatusMutationFetch(operation, { pendingMutation });
            const { unmount } = render(Page);
            await screen.findByText(/Connected to HomeWifi • Slot 1/);
            await performWifiStatusMutation(operation);
            await waitFor(() => expect(state.mutations).toBe(1));
            unmount();
            const calls = fetchMock.mock.calls.length;
            pendingMutation.resolve(jsonResponse({ success: true }));
            await new Promise((resolve) => setTimeout(resolve, 0));
            expect(fetchMock.mock.calls).toHaveLength(calls);
            expect(document.body).not.toHaveTextContent('Disconnected from WiFi');
        }
    );

    it.each(Array.from({ length: 25 }, (_, gap) => gap))(
        'ignores obsolete Test success across status/mutation settlement gap %i',
        async (microtaskGap) => {
            const pendingMutation = createDeferred();
            const { oldStatus, state } = installWifiStatusMutationFetch('disconnect', {
                pendingStatus: true,
                pendingMutation
            });
            const { unmount } = render(Page);
            try {
                await screen.findByText(/Connected to HomeWifi • Slot 1/);
                await fireEvent.click(screen.getAllByRole('button', { name: 'Test' })[0]);
                await waitFor(() => expect(state.statusReads).toBe(2));
                await performWifiStatusMutation('disconnect');
                // Sweep both sides of the reader's return and the Test caller's
                // await continuation: a settled mutation invalidates both.
                oldStatus.resolve(jsonResponse(connectedWifiStatus));
                for (let i = 0; i < microtaskGap; i += 1) await Promise.resolve();
                state.committed = true;
                pendingMutation.resolve(jsonResponse({ success: true }));
                await expectReconciledWifiStatus('disconnect');
                expect(state.statusReads).toBe(3);
                expect(screen.queryByText('Connected to HomeWifi')).not.toBeInTheDocument();
            } finally { unmount(); }
        }
    );

    it.each([0, 1, 24])(
        'keeps restore status authoritative across enable-confirmation settlement gap %i',
        async (microtaskGap) => {
            const toggleStatus = createDeferred();
            const oldNetworks = createDeferred();
            const restore = createDeferred();
            let statusReads = 0;
            let networkReads = 0;
            installDefaultFetch([
                { method: 'GET', match: '/api/wifi/status', respond: () => {
                    statusReads += 1;
                    return statusReads === 2 ? toggleStatus.promise
                        : jsonResponse({ enabled: false, state: 'disabled' });
                } },
                { method: 'GET', match: '/api/wifi/networks', respond: () => {
                    networkReads += 1;
                    return networkReads === 2 ? oldNetworks.promise : jsonResponse({ slots: defaultSlots });
                } },
                { method: 'POST', match: '/api/settings/restore', respond: () => restore.promise }
            ]);
            vi.stubGlobal('confirm', vi.fn(() => true));
            const { unmount } = render(Page);
            try {
                await waitFor(() => expect(networkReads).toBe(1));
                const toggle = wifiClientToggle();
                await fireEvent.click(toggle);
                await waitFor(() => expect(statusReads).toBe(2));
                await startRestore(JSON.stringify({ _type: 'v1simple_backup', _version: 1, wifiClientEnabled: false }));
                toggleStatus.resolve(jsonResponse({ enabled: true, state: 'connecting', savedSSID: 'HomeWifi' }));
                await waitFor(() => expect(networkReads).toBe(2));
                oldNetworks.resolve(jsonResponse({ slots: defaultSlots }));
                // The enable caller awaits its list refresh after reading status.
                // A restore settling in that gap supersedes its confirmation too.
                for (let i = 0; i < microtaskGap; i += 1) await Promise.resolve();
                restore.resolve(jsonResponse({ success: true }));
                await waitFor(() => expect(statusReads).toBe(3));
                await waitFor(() => expect(networkReads).toBe(3));
                await waitFor(() => expect(toggle).not.toBeChecked());
                expect(screen.queryByText('WiFi client enabled')).not.toBeInTheDocument();
                expect(screen.getByText('Settings restored and reloaded.')).toBeInTheDocument();
            } finally { unmount(); }
        }
    );

    it('refreshes WiFi status again when an earlier deletion commits after a later deletion', async () => {
        const firstDeletion = createDeferred();
        let deleteCalls = 0;
        let statusReads = 0;
        let activeNetworkDeleted = false;
        installDefaultFetch([
            { method: 'GET', match: '/api/wifi/status', respond: () => {
                statusReads += 1;
                return jsonResponse(activeNetworkDeleted
                    ? { enabled: true, state: 'disconnected' }
                    : connectedWifiStatus);
            } },
            { method: 'POST', match: '/api/wifi/networks/delete', respond: () => {
                deleteCalls += 1;
                return deleteCalls === 1 ? firstDeletion.promise : jsonResponse({ success: true });
            } }
        ]);
        vi.stubGlobal('confirm', vi.fn(() => true));
        const { unmount } = render(Page);
        try {
            await screen.findByText(/Connected to HomeWifi • Slot 1/);
            const deletes = screen.getAllByRole('button', { name: 'Delete' });
            await fireEvent.click(deletes[0]);
            await fireEvent.click(deletes[1]);
            await waitFor(() => expect(statusReads).toBe(2));
            expect(screen.getByRole('button', { name: 'Disconnect' })).toBeInTheDocument();
            activeNetworkDeleted = true;
            firstDeletion.resolve(jsonResponse({ success: true }));
            await expectReconciledWifiStatus('delete');
            expect(statusReads).toBe(3);
        } finally { unmount(); }
    });

    it('polls a saved-network test through terminal connection success', async () => {
        vi.useFakeTimers();
        let statusCalls = 0;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    if (statusCalls === 1) {
                        return jsonResponse({ enabled: true, state: 'disconnected' });
                    }
                    if (statusCalls === 2) {
                        return jsonResponse({
                            enabled: true,
                            state: 'connecting',
                            savedSSID: 'HomeWifi'
                        });
                    }
                    return jsonResponse({
                        enabled: true,
                        state: 'connected',
                        connectedSSID: 'HomeWifi',
                        connectedSlotIndex: 0,
                        rssi: -48
                    });
                }
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Garage');
        await fireEvent.click(screen.getAllByRole('button', { name: /^Test$/i })[0]);

        await screen.findByText('Testing connection to HomeWifi...');
        expect(screen.getAllByRole('button', { name: /^Test$/i })[1]).toBeDisabled();
        await vi.advanceTimersByTimeAsync(1000);
        await screen.findByText('Connected to HomeWifi');
        expect(jsonBodies(fetchMock, '/api/wifi/networks/test').at(-1)).toEqual({ index: 0 });

        unmount();
    });

    it.each([
        ['SSID-only status', { connectedSSID: 'HomeWifi' }, true],
        ['slot-only status', { connectedSlotIndex: 0 }, true],
        ['another saved network', { connectedSSID: 'PhoneHotspot', connectedSlotIndex: 1 }, false]
    ])('preserves Test identity handling for %s', async (_label, connection, succeeds) => {
        let testing = false;
        installDefaultFetch([
            { method: 'POST', match: '/api/wifi/networks/test', respond: () => {
                testing = true;
                return jsonResponse({ success: true });
            } },
            { method: 'GET', match: '/api/wifi/status', respond: () => jsonResponse(testing
                ? { enabled: true, state: 'connected', ...connection }
                : { enabled: true, state: 'disconnected' }) }
        ]);
        const { unmount } = render(Page);
        try {
            await screen.findByText('Garage');
            await fireEvent.click(screen.getAllByRole('button', { name: 'Test' })[0]);
            if (succeeds) {
                await screen.findByText('Connected to HomeWifi');
            } else {
                await screen.findByText('Connection test reached PhoneHotspot, not HomeWifi');
                expect(screen.queryByText('Connected to HomeWifi')).not.toBeInTheDocument();
            }
            expect(screen.getAllByRole('button', { name: 'Test' })
                .every((button) => !button.disabled)).toBe(true);
        } finally { unmount(); }
    });

    it('reports a saved-network test terminal failure', async () => {
        let statusCalls = 0;
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    return jsonResponse({
                        enabled: true,
                        state: statusCalls === 1 ? 'disconnected' : 'failed'
                    });
                }
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Garage');
        await fireEvent.click(screen.getAllByRole('button', { name: /^Test$/i })[0]);

        await screen.findByText('Could not connect to HomeWifi');
        expect(screen.getAllByRole('button', { name: /^Test$/i })[0]).toBeEnabled();

        unmount();
    });

    it.each(['headers', 'body'])('caps stalled WiFi %s at a 20-second test deadline and permits retry', async (stall) => {
        vi.useFakeTimers();
        let statusCalls = 0;
        let stalled = true;
        const signals = [];
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: ({ init }) => {
                    statusCalls += 1;
                    if (statusCalls === 1) {
                        return jsonResponse({ enabled: true, state: 'disconnected' });
                    }
                    if (!stalled) {
                        return jsonResponse({
                            enabled: true,
                            state: 'connected',
                            connectedSSID: 'HomeWifi',
                            connectedSlotIndex: 0
                        });
                    }

                    signals.push(init.signal);
                    if (stall === 'body') {
                        return new Response(new ReadableStream({
                            start(controller) {
                                init.signal.addEventListener('abort', () =>
                                    controller.error(new Error('status body aborted')), { once: true });
                            }
                        }), { status: 200, headers: { 'Content-Type': 'application/json' } });
                    }

                    return new Promise((resolve, reject) => {
                        const rejectOnAbort = () => reject(new Error('status request aborted'));
                        if (init.signal?.aborted) {
                            rejectOnAbort();
                            return;
                        }
                        init.signal?.addEventListener('abort', rejectOnAbort, { once: true });
                    });
                }
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Garage');
        await fireEvent.click(screen.getAllByRole('button', { name: /^Test$/i })[0]);
        await screen.findByText('Testing connection to HomeWifi...');

        await vi.advanceTimersByTimeAsync(19_999);
        expect(
            screen.queryByText('Connection test timed out for HomeWifi')
        ).not.toBeInTheDocument();

        await vi.advanceTimersByTimeAsync(1);
        expect(screen.getByText('Connection test timed out for HomeWifi')).toBeInTheDocument();
        expect(screen.getAllByRole('button', { name: /^Test$/i })[0]).toBeEnabled();
        expect(statusCalls).toBeLessThanOrEqual(9);
        expect(signals.every((signal) => signal.aborted)).toBe(true);

        stalled = false;
        await fireEvent.click(screen.getAllByRole('button', { name: /^Test$/i })[0]);
        await screen.findByText('Connected to HomeWifi');
        expect(screen.getAllByRole('button', { name: /^Test$/i })[0]).toBeEnabled();

        unmount();
    });

    it.each(['headers', 'body'])('bounds the initial WiFi status %s and recovers on a later request', async (stall) => {
        vi.useFakeTimers();
        let statusCalls = 0;
        let initialSignal;
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: ({ init }) => {
                    if (++statusCalls > 1) {
                        return jsonResponse({ enabled: true, state: 'connected',
                            connectedSSID: 'HomeWifi', connectedSlotIndex: 0 });
                    }
                    initialSignal = init.signal;
                    if (stall === 'headers') {
                        return new Promise((resolve, reject) => {
                            init.signal.addEventListener('abort', () =>
                                reject(new Error('status headers aborted')), { once: true });
                        });
                    }
                    return new Response(new ReadableStream({
                        start(controller) {
                            init.signal.addEventListener('abort', () =>
                                controller.error(new Error('status body aborted')), { once: true });
                        }
                    }), { status: 200, headers: { 'Content-Type': 'application/json' } });
                }
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByLabelText('AP Name');
        await vi.advanceTimersByTimeAsync(5000);
        expect(initialSignal.aborted).toBe(true);
        expect(screen.getByText('Failed to load WiFi status')).toBeInTheDocument();
        const wifiCard = screen.getByText('WiFi Client').closest('.surface-card');
        await fireEvent.click(wifiCard.querySelector('input[type="checkbox"]'));
        await screen.findByText('Garage');
        expect(screen.getByText(/Connected to HomeWifi • Slot 1/)).toBeInTheDocument();
        expect(screen.queryByText('Failed to load WiFi status')).not.toBeInTheDocument();

        unmount();
    });

    it('reorders saved network priority with move controls', async () => {
        let networkReads = 0;
        const reorderedSlots = [
            { ...defaultSlots[0], priority: 1 },
            { ...defaultSlots[1], priority: 0 },
            ...defaultSlots.slice(2)
        ];
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/networks',
                respond: () =>
                    jsonResponse({ slots: networkReads++ === 0 ? defaultSlots : reorderedSlots })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Phone');
        await fireEvent.click(screen.getByRole('button', { name: /move phonehotspot up/i }));

        await screen.findByText('WiFi network priority updated');
        expect(jsonBodies(fetchMock, '/api/wifi/networks/priorities')).toEqual([
            {
                updates: [
                    { index: 1, priority: 0 },
                    { index: 0, priority: 1 }
                ]
            }
        ]);
        const phone = screen.getByText('Phone');
        const garage = screen.getByText('Garage');
        expect(
            phone.compareDocumentPosition(garage) & Node.DOCUMENT_POSITION_FOLLOWING
        ).toBeTruthy();

        unmount();
    });

    for (const [label, response] of [
        ['400 response', () => jsonResponse({ error: 'invalid update' }, 400)],
        ['500 response', () => jsonResponse({ error: 'persistence failed' }, 500)],
        ['network failure', () => Promise.reject(new Error('connection lost'))]
    ]) {
        it(`restores authoritative WiFi priority after ${label}`, async () => {
            let networkReads = 0;
            const fetchMock = installDefaultFetch([
                {
                    method: 'GET',
                    match: '/api/wifi/networks',
                    respond: () => {
                        networkReads += 1;
                        return jsonResponse({ slots: defaultSlots });
                    }
                },
                {
                    method: 'POST',
                    match: '/api/wifi/networks/priorities',
                    respond: response
                }
            ]);
            const { unmount } = render(Page);

            await screen.findByText('Phone');
            await fireEvent.click(screen.getByRole('button', { name: /move phonehotspot up/i }));

            await screen.findByText('Failed to reorder WiFi networks');
            await waitFor(() => expect(networkReads).toBeGreaterThanOrEqual(2));
            expect(jsonBodies(fetchMock, '/api/wifi/networks/priorities')).toEqual([
                {
                    updates: [
                        { index: 1, priority: 0 },
                        { index: 0, priority: 1 }
                    ]
                }
            ]);
            const garage = screen.getByText('Garage');
            const phone = screen.getByText('Phone');
            expect(
                garage.compareDocumentPosition(phone) & Node.DOCUMENT_POSITION_FOLLOWING
            ).toBeTruthy();
            expect((document.body.textContent.match(/Priority 1/g) || []).length).toBe(1);
            expect((document.body.textContent.match(/Priority 2/g) || []).length).toBe(1);

            unmount();
        });
    }

    it('pins the currently connected network with RSSI', async () => {
        installDefaultFetch([], {
            wifiStatus: {
                enabled: true,
                state: 'connected',
                connectedSSID: '',
                connectedSlotIndex: 1,
                ip: '192.168.1.50',
                rssi: -55
            }
        });
        const { unmount } = render(Page);

        const phone = await screen.findByText('Phone');
        const garage = await screen.findByText('Garage');
        expect(
            phone.compareDocumentPosition(garage) & Node.DOCUMENT_POSITION_FOLLOWING
        ).toBeTruthy();
        expect(screen.getByText('Connected')).toBeInTheDocument();
        expect(screen.getByText(/Slot 2 \(Phone\)/)).toBeInTheDocument();
        expect(screen.getAllByText('-55 dBm').length).toBeGreaterThan(0);

        unmount();
    });

    // Connection fields disappear when the STA link drops; stale values must
    // not badge the disconnected network during a new connection attempt.
    it('drops stale connection fields when wifi status omits them after a disconnect', async () => {
        let disconnected = false;
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/wifi/disconnect',
                respond: () => {
                    disconnected = true;
                    return jsonResponse({ success: true });
                }
            },
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () =>
                    disconnected
                        ? // Link down, now reconnecting elsewhere: no connection fields at all.
                          jsonResponse({
                              enabled: true,
                              state: 'connecting',
                              savedSSID: 'PhoneHotspot'
                          })
                        : jsonResponse({
                              enabled: true,
                              state: 'connected',
                              savedSSID: 'HomeWifi',
                              connectedSSID: 'HomeWifi',
                              connectedSlotIndex: 0,
                              ip: '192.168.1.50',
                              rssi: -48
                          })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText('Garage');
        expect(screen.getAllByText('-48 dBm').length).toBeGreaterThan(0);

        await fireEvent.click(await screen.findByRole('button', { name: /^Disconnect$/i }));
        await screen.findByText('Disconnected from WiFi');

        // Only the network actually being connected to may carry the badge. With the
        // stale connectedSSID retained, Garage picked one up too.
        const connecting = await screen.findAllByText('Connecting');
        expect(connecting).toHaveLength(1);
        expect(screen.getByText('Phone').closest('.rounded-box')).toContainElement(connecting[0]);
        expect(screen.queryByText('-48 dBm')).not.toBeInTheDocument();

        unmount();
    });

    it('saves a network picked from scan results', async () => {
        vi.useFakeTimers();
        const fetchMock = installDefaultFetch(apiFixtureMatchers('wifi_scan_success'));
        const { unmount } = render(Page);

        await openScanModal();
        await vi.advanceTimersByTimeAsync(1000);
        await vi.advanceTimersByTimeAsync(1000);
        await fireEvent.click(await screen.findByRole('button', { name: /BenchAP/i }));
        await fireEvent.input(await screen.findByLabelText('Password'), {
            target: { value: 'bench-secret' }
        });
        await fireEvent.click(screen.getByRole('button', { name: /^Save Network$/i }));

        await screen.findByText('Saved BenchAP');
        expect(jsonBodies(fetchMock, '/api/wifi/networks').at(-1)).toMatchObject({
            ssid: 'BenchAP',
            password: 'bench-secret',
            priority: 2
        });

        unmount();
    });

    it('clears the saved password when the same SSID is selected as an open network', async () => {
        let savedPassword = 'previous-network-secret';
        const fetchMock = installDefaultFetch([
            {
                method: 'POST',
                match: '/api/wifi/scan',
                respond: jsonResponse({
                    scanning: false,
                    networks: [{ ssid: 'HomeWifi', secure: false, rssi: -35 }]
                })
            },
            {
                method: 'POST',
                match: '/api/wifi/networks',
                respond: ({ init }) => {
                    const body = JSON.parse(init.body);
                    // Match the firmware contract: omission preserves a saved password.
                    if (Object.hasOwn(body, 'password')) savedPassword = body.password;
                    return jsonResponse({ success: true, index: 0 });
                }
            },
            {
                method: 'GET',
                match: '/api/wifi/networks',
                respond: () => jsonResponse({
                    slots: defaultSlots.map((slot) => slot.index === 0
                        ? { ...slot, hasPassword: savedPassword.length > 0 }
                        : slot)
                })
            }
        ]);
        const { unmount } = render(Page);

        await openScanModal();
        await fireEvent.click(await screen.findByRole('button', { name: /HomeWifi.*dBm/ }));
        await screen.findByText('This is an open network');
        expect(screen.queryByLabelText('Password')).not.toBeInTheDocument();
        await fireEvent.click(screen.getByRole('button', { name: /^Save Network$/ }));

        await screen.findByText('Saved HomeWifi');
        expect(jsonBodies(fetchMock, '/api/wifi/networks').at(-1)).toMatchObject({
            ssid: 'HomeWifi',
            password: ''
        });
        expect(savedPassword).toBe('');
        await waitFor(() => {
            expect(screen.getByText('Garage').closest('.rounded-box')).toHaveTextContent('open/no saved password');
        });

        unmount();
    });

    it('uses captured enable success and status-refetch responses', async () => {
        const fetchMock = installDefaultFetch(apiFixtureMatchers('wifi_enable_success'));
        const { unmount } = render(Page);

        await screen.findByText(/WiFi client is disabled/i);
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');
        expect(toggle).not.toBeChecked();

        await fireEvent.click(toggle);

        await screen.findByText('WiFi client enabled');
        expect(toggle).toBeChecked();
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === '/api/wifi/enable' && init?.method === 'POST'
            )
        ).toBe(true);

        unmount();
    });

    it('keeps the captured enable state disabled after a failed connection attempt', async () => {
        const fetchMock = installDefaultFetch(apiFixtureMatchers('wifi_enable_failure'));
        const first = render(Page);

        await screen.findByText(/WiFi client is disabled/i);
        const firstCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const firstToggle = firstCard.querySelector('input[type="checkbox"]');
        expect(firstToggle).not.toBeChecked();

        await fireEvent.click(firstToggle);

        await screen.findByText('Failed to change WiFi setting');
        expect(firstToggle).not.toBeChecked();
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/networks')).toHaveLength(
            2
        );

        first.unmount();
    });

    it('reconciles WiFi status after the enable request rejects', async () => {
        let statusCalls = 0;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    return statusCalls === 1
                        ? jsonResponse({ enabled: false, state: 'disabled' })
                        : jsonResponse({ enabled: true, state: 'connecting' });
                }
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: () => Promise.reject(new Error('request lost'))
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText(/WiFi client is disabled/i);
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');

        await fireEvent.click(toggle);

        await screen.findByText('Connection error');
        expect(statusCalls).toBe(2);
        expect(toggle).toBeChecked();
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);

        unmount();
    });

    it('does not publish a pre-mutation status when forced reconciliation fails', async () => {
        const activeStatus = createDeferred();
        let statusCalls = 0;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    return statusCalls === 1
                        ? activeStatus.promise
                        : Promise.reject(new Error('forced status unavailable'));
                }
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: jsonResponse({ success: false }, 500)
            }
        ]);
        const { unmount } = render(Page);

        await waitFor(() => expect(statusCalls).toBe(1));
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');
        await fireEvent.click(toggle);

        activeStatus.resolve(jsonResponse({ enabled: true, state: 'connecting' }));

        await waitFor(() => expect(statusCalls).toBe(2));
        await screen.findByText('Failed to change WiFi setting');
        expect(toggle).not.toBeChecked();
        expect(screen.queryByText(/Connecting to/)).not.toBeInTheDocument();
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);

        unmount();
    });

    it('queues a fresh status read behind an older in-flight request', async () => {
        const staleStatus = createDeferred();
        let statusCalls = 0;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    return statusCalls === 1
                        ? staleStatus.promise
                        : jsonResponse({ enabled: false, state: 'disabled' });
                }
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: jsonResponse({ success: false }, 500)
            }
        ]);
        const { unmount } = render(Page);

        await waitFor(() => expect(statusCalls).toBe(1));
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');
        await fireEvent.click(toggle);

        staleStatus.resolve(jsonResponse({ enabled: true, state: 'connecting' }));

        await waitFor(() => expect(statusCalls).toBe(2));
        await screen.findByText('Failed to change WiFi setting');
        expect(toggle).not.toBeChecked();
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);

        unmount();
    });

    it('does not report enable success when status confirmation fails', async () => {
        let statusCalls = 0;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    return statusCalls === 1
                        ? jsonResponse({ enabled: false, state: 'disabled' })
                        : Promise.reject(new Error('status unavailable'));
                }
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: jsonResponse({ success: true })
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText(/WiFi client is disabled/i);
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');

        await fireEvent.click(toggle);

        await screen.findByText('Failed to confirm WiFi setting');
        expect(statusCalls).toBe(2);
        expect(toggle).not.toBeChecked();
        expect(screen.queryByText('WiFi client enabled')).not.toBeInTheDocument();
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);

        unmount();
    });

    it('does not confirm disable or poison state from a non-boolean enabled field', async () => {
        let statusCalls = 0;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    return statusCalls === 1
                        ? jsonResponse({ enabled: true, state: 'connected' })
                        : jsonResponse({ enabled: null, state: 'disabled' });
                }
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: jsonResponse({ success: true })
            }
        ]);
        const { unmount } = render(Page);

        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');
        await waitFor(() => expect(toggle).toBeChecked());

        await fireEvent.click(toggle);

        await screen.findByText('Failed to confirm WiFi setting');
        expect(statusCalls).toBe(2);
        expect(toggle).toBeChecked();
        expect(screen.queryByText('WiFi client disabled')).not.toBeInTheDocument();
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);

        unmount();
    });

    it('restores the last authoritative toggle state when reconciliation fails', async () => {
        let statusCalls = 0;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    return statusCalls === 1
                        ? jsonResponse({ enabled: false, state: 'disabled' })
                        : Promise.reject(new Error('status unavailable'));
                }
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: jsonResponse({ success: false }, 500)
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText(/WiFi client is disabled/i);
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');

        await fireEvent.click(toggle);

        await screen.findByText('Failed to change WiFi setting');
        expect(statusCalls).toBe(2);
        expect(toggle).not.toBeChecked();
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);

        unmount();
    });

    it('disables the WiFi toggle while one mutation is pending', async () => {
        const enableResponse = createDeferred();
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: jsonResponse({ enabled: false, state: 'disabled' })
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: () => enableResponse.promise
            }
        ]);
        const { unmount } = render(Page);

        await screen.findByText(/WiFi client is disabled/i);
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');

        await fireEvent.click(toggle);
        const disabledWhilePending = toggle.disabled;
        const checkedWhilePending = toggle.checked;
        await fireEvent.click(toggle);
        const postCallsWhilePending = fetchMock.mock.calls.filter(
            ([url, init]) => url === '/api/wifi/enable' && init?.method === 'POST'
        ).length;

        enableResponse.resolve(jsonResponse({ success: false }, 500));
        await screen.findByText('Failed to change WiFi setting');

        expect(disabledWhilePending).toBe(true);
        expect(checkedWhilePending).toBe(false);
        expect(postCallsWhilePending).toBe(1);
        expect(toggle).not.toBeDisabled();

        unmount();
    });

    it('does not reconcile a deferred toggle after unmount', async () => {
        const enableResponse = createDeferred();
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: jsonResponse({ enabled: false, state: 'disabled' })
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: () => enableResponse.promise
            }
        ]);
        const view = render(Page);

        await screen.findByText(/WiFi client is disabled/i);
        await waitFor(() => {
            expect(
                fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/networks')
            ).toHaveLength(1);
        });
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');
        await fireEvent.click(toggle);

        const statusCallsBeforeUnmount = fetchMock.mock.calls.filter(
            ([url]) => url === '/api/wifi/status'
        ).length;
        const networkCallsBeforeUnmount = fetchMock.mock.calls.filter(
            ([url]) => url === '/api/wifi/networks'
        ).length;
        view.unmount();

        enableResponse.resolve(jsonResponse({ success: true }));
        await new Promise((resolve) => setTimeout(resolve, 0));

        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(
            statusCallsBeforeUnmount
        );
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/networks')).toHaveLength(
            networkCallsBeforeUnmount
        );
    });

    it('does not launch a queued status reconciliation after unmount', async () => {
        const activeStatus = createDeferred();
        let statusCalls = 0;
        const fetchMock = installDefaultFetch([
            {
                method: 'GET',
                match: '/api/wifi/status',
                respond: () => {
                    statusCalls += 1;
                    return activeStatus.promise;
                }
            },
            {
                method: 'POST',
                match: '/api/wifi/enable',
                respond: jsonResponse({ success: true })
            }
        ]);
        const view = render(Page);

        await waitFor(() => expect(statusCalls).toBe(1));
        const wifiCard = (await screen.findByText('WiFi Client')).closest('.surface-card');
        const toggle = wifiCard.querySelector('input[type="checkbox"]');
        await fireEvent.click(toggle);
        await waitFor(() => {
            expect(
                fetchMock.mock.calls.filter(
                    ([url, init]) => url === '/api/wifi/enable' && init?.method === 'POST'
                )
            ).toHaveLength(1);
        });

        view.unmount();
        activeStatus.resolve(jsonResponse({ enabled: false, state: 'disabled' }));
        await new Promise((resolve) => setTimeout(resolve, 0));

        expect(statusCalls).toBe(1);
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(1);
    });

    it('lazy-loads the WiFi scan modal on first open and reuses it after closing', async () => {
        const deferred = createDeferred();
        const wifiModalLoader = vi
            .spyOn(settingsLazyComponents, 'loadSettingsWifiModal')
            .mockReturnValue(deferred.promise);
        installDefaultFetch();
        const { unmount } = render(Page);

        expect(wifiModalLoader).not.toHaveBeenCalled();
        await fireEvent.click(
            (await screen.findAllByRole('button', { name: /pick from scan/i }))[0]
        );

        expect(wifiModalLoader).toHaveBeenCalledTimes(1);
        await screen.findByText('Loading WiFi modal...');

        deferred.resolve(await import('#lib/features/settings/SettingsWifiModal.svelte'));
        await screen.findByText('Pick WiFi Network to Save');
        await fireEvent.click(screen.getByRole('button', { name: /^Close$/i }));
        await waitFor(() => {
            expect(screen.queryByText('Pick WiFi Network to Save')).toBeNull();
        });

        await fireEvent.click(screen.getAllByRole('button', { name: /pick from scan/i })[0]);
        await screen.findByText('Pick WiFi Network to Save');
        expect(wifiModalLoader).toHaveBeenCalledTimes(1);

        unmount();
    });

    it('does not poll before the scan POST settles', async () => {
        vi.useFakeTimers();
        const scanPost = createDeferred();
        let getCalls = 0;
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/wifi/scan',
                respond: () => scanPost.promise
            },
            {
                method: 'GET',
                match: '/api/wifi/scan',
                respond: () => {
                    getCalls += 1;
                    return jsonResponse({
                        scanning: false,
                        networks: [{ ssid: 'DeferredAP', secure: true, rssi: -44 }]
                    });
                }
            }
        ]);
        const { unmount } = render(Page);

        await openScanModal();
        await vi.advanceTimersByTimeAsync(3000);
        expect(getCalls).toBe(0);

        scanPost.resolve(jsonResponse({ scanning: true, networks: [] }));
        await Promise.resolve();
        await vi.advanceTimersByTimeAsync(1000);

        expect(getCalls).toBe(1);
        await screen.findByRole('button', { name: /DeferredAP/i });

        await vi.advanceTimersByTimeAsync(2000);
        expect(getCalls).toBe(1);

        unmount();
    });

    it('ignores a stale scan response and polls the reopened scan', async () => {
        vi.useFakeTimers();
        let postCalls = 0;
        const reopenedPost = createDeferred();
        const staleGet = createDeferred();
        let getCalls = 0;
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/wifi/scan',
                respond: () => {
                    postCalls += 1;
                    return postCalls === 1
                        ? jsonResponse({ scanning: true, networks: [] })
                        : reopenedPost.promise;
                }
            },
            {
                method: 'GET',
                match: '/api/wifi/scan',
                respond: () => {
                    getCalls += 1;
                    if (getCalls === 1) return staleGet.promise;
                    return jsonResponse({
                        scanning: false,
                        networks: [{ ssid: 'ReopenedAP', secure: true, rssi: -42 }]
                    });
                }
            }
        ]);
        const { unmount } = render(Page);

        await openScanModal();
        await vi.advanceTimersByTimeAsync(1000);
        expect(getCalls).toBe(1);

        await fireEvent.click(screen.getByRole('button', { name: /^Close$/i }));
        await waitFor(() => {
            expect(screen.queryByText('Pick WiFi Network to Save')).not.toBeInTheDocument();
        });

        await openScanModal();
        staleGet.resolve(
            jsonResponse({
                scanning: false,
                networks: [{ ssid: 'OldAP', secure: true, rssi: -80 }]
            })
        );
        await Promise.resolve();
        await Promise.resolve();

        reopenedPost.resolve(jsonResponse({ scanning: true, networks: [] }));
        await Promise.resolve();
        await vi.advanceTimersByTimeAsync(1000);

        expect(getCalls).toBe(2);
        await screen.findByRole('button', { name: /ReopenedAP/i });
        expect(screen.queryByRole('button', { name: /OldAP/i })).not.toBeInTheDocument();

        unmount();
    });

    it('reports a non-success response when starting a WiFi scan', async () => {
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/wifi/scan',
                respond: jsonResponse({ success: false, message: 'scan unavailable' }, 503)
            }
        ]);
        const { unmount } = render(Page);

        const buttons = await screen.findAllByRole('button', { name: /pick from scan/i });
        await fireEvent.click(buttons[0]);
        await screen.findByText('Failed to start WiFi scan');
        expect(screen.queryByText('Scanning for networks...')).not.toBeInTheDocument();
        expect(screen.queryByText('Pick WiFi Network to Save')).not.toBeInTheDocument();

        unmount();
    });

    it('shows WiFi scan polling errors and clears them after a successful retry', async () => {
        vi.useFakeTimers();
        let postCalls = 0;
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/wifi/scan',
                respond: () => {
                    postCalls += 1;
                    return postCalls === 1
                        ? jsonResponse({ scanning: true, networks: [] })
                        : jsonResponse({
                              scanning: false,
                              networks: [{ ssid: 'RecoveredAP', secure: true, rssi: -38 }]
                          });
                }
            },
            {
                method: 'GET',
                match: '/api/wifi/scan',
                respond: jsonResponse({ error: 'scan failed' }, 500)
            }
        ]);
        const { unmount } = render(Page);

        await openScanModal();
        await vi.advanceTimersByTimeAsync(1000);

        await screen.findByText('Failed to update WiFi scan');
        expect(screen.queryByText('Pick WiFi Network to Save')).not.toBeInTheDocument();

        const retryButtons = await screen.findAllByRole('button', { name: /pick from scan/i });
        await fireEvent.click(retryButtons[0]);

        await screen.findByRole('button', { name: /RecoveredAP/i });
        expect(screen.queryByText('Failed to update WiFi scan')).not.toBeInTheDocument();

        unmount();
    });

    it('does not claim a disconnect succeeded when the API rejects it', async () => {
        installDefaultFetch(
            [
                {
                    method: 'POST',
                    match: '/api/wifi/disconnect',
                    respond: jsonResponse({ success: false, message: 'disconnect failed' }, 500)
                }
            ],
            {
                wifiStatus: {
                    enabled: true,
                    state: 'connected',
                    connectedSSID: 'HomeWifi',
                    connectedSlotIndex: 0,
                    ip: '192.168.1.50',
                    rssi: -50
                }
            }
        );
        const { unmount } = render(Page);

        await fireEvent.click(await screen.findByRole('button', { name: /^Disconnect$/i }));

        await screen.findByText('Failed to disconnect');
        expect(screen.queryByText('Disconnected from WiFi')).not.toBeInTheDocument();

        unmount();
    });

    it('shows success message on save success', async () => {
        const fetchMock = installDefaultFetch([
            {
                method: 'POST',
                match: '/api/device/settings',
                respond: jsonResponse({ success: true })
            }
        ]);
        const { unmount } = render(Page);

        const saveButton = await screen.findByRole('button', { name: /save settings/i });
        await fireEvent.click(saveButton);
        await screen.findByText(
            'Settings saved! New AP credentials apply the next time the AP starts.'
        );
        const postCall = fetchMock.mock.calls.find(
            ([url, init]) => url === '/api/device/settings' && init?.method === 'POST'
        );
        expect(postCall).toBeTruthy();
        expect(postCall[1].body.has('proxy_ble')).toBe(false);
        expect(postCall[1].body.has('proxy_name')).toBe(false);
        unmount();
    });

    it.each(['POST', 'readback'])(
        'locks settings controls during the pending %s without blocking WiFi editing',
        async (phase) => {
            const saveResponse = createDeferred();
            const readbackResponse = createDeferred();
            let settingsReads = 0;
            const fetchMock = installDefaultFetch([
                {
                    method: 'GET',
                    match: '/api/device/settings',
                    respond: () => {
                        settingsReads += 1;
                        return settingsReads === 1
                            ? jsonResponse({
                                  ap_ssid: 'V1',
                                  ap_password: 'InitialPassword',
                                  autoPowerOffMinutes: 30,
                                  apTimeoutMinutes: 15
                              })
                            : readbackResponse.promise;
                    }
                },
                {
                    method: 'POST',
                    match: '/api/device/settings',
                    respond: () => saveResponse.promise
                }
            ]);
            const { unmount } = render(Page);

            try {
                const name = await screen.findByLabelText('AP Name');
                const password = screen.getByLabelText('AP Password');
                const powerOff = screen.getByLabelText(/minutes after disconnect/i);
                const save = screen.getByRole('button', { name: /save settings/i });
                const controls = [name, password, powerOff, save];
                await screen.findByText('Garage');

                await fireEvent.input(name, { target: { value: 'SavedName' } });
                await fireEvent.input(password, { target: { value: 'SavedPassword' } });
                await fireEvent.input(powerOff, { target: { value: '10' } });
                await fireEvent.click(save);

                const posts = fetchMock.mock.calls.filter(
                    ([url, init]) => url === '/api/device/settings' && init?.method === 'POST'
                );
                expect(posts).toHaveLength(1);
                expect(Object.fromEntries(posts[0][1].body)).toEqual({
                    ap_ssid: 'SavedName',
                    ap_password: 'SavedPassword',
                    autoPowerOffMinutes: '10',
                    apTimeoutMinutes: '15'
                });

                if (phase === 'readback') {
                    saveResponse.resolve(jsonResponse({ success: true }));
                    await waitFor(() => expect(settingsReads).toBe(2));
                }

                for (const control of controls) expect(control).toBeDisabled();
                // Native interactions honor disabled fieldsets; fireEvent.input does not.
                name.focus();
                expect(name).not.toHaveFocus();

                await openManualEditor();
                const wifiName = screen.getByLabelText('SSID');
                expect(wifiName).toBeEnabled();
                await fireEvent.input(wifiName, { target: { value: 'IndependentWifiDraft' } });

                saveResponse.resolve(jsonResponse({ success: true }));
                await waitFor(() => expect(settingsReads).toBe(2));
                for (const control of controls) expect(control).toBeDisabled();
                readbackResponse.resolve(jsonResponse({
                    ap_ssid: 'SavedName',
                    ap_password: '********',
                    autoPowerOffMinutes: 10,
                    apTimeoutMinutes: 15
                }));

                await waitFor(() => expect(save).toBeEnabled());
                for (const control of controls) expect(control).toBeEnabled();
                expect(name).toHaveValue('SavedName');
                expect(password).toHaveValue('********');
                expect(powerOff).toHaveValue(10);
                expect(wifiName).toHaveValue('IndependentWifiDraft');
                await fireEvent.click(screen.getByRole('button', { name: /^Cancel$/i }));
                await fireEvent.input(name, { target: { value: 'NewerDraft' } });
                expect(name).toHaveValue('NewerDraft');
            } finally {
                saveResponse.resolve(jsonResponse({ success: true }));
                readbackResponse.resolve(jsonResponse({ ap_ssid: 'SavedName' }));
                unmount();
            }
        }
    );

    it('shows API error message on save failure', async () => {
        installDefaultFetch([
            {
                method: 'POST',
                match: '/api/device/settings',
                respond: jsonResponse({ success: false }, 500)
            }
        ]);
        const { unmount } = render(Page);

        const saveButton = await screen.findByRole('button', { name: /save settings/i });
        await fireEvent.click(saveButton);

        await screen.findByText('Failed to save settings');
        expect(saveButton).toBeEnabled();
        expect(screen.getByLabelText('AP Name')).toBeEnabled();
        expect(screen.getByLabelText(/minutes after disconnect/i)).toBeEnabled();
        unmount();
    });

    it('unlocks settings and keeps the submitted values when save readback fails', async () => {
        let settingsReads = 0;
        installDefaultFetch([
            {
                method: 'GET',
                match: '/api/device/settings',
                respond: () => {
                    settingsReads += 1;
                    return settingsReads === 1
                        ? jsonResponse({ ap_ssid: 'V1', autoPowerOffMinutes: 30 })
                        : jsonResponse({ error: 'readback unavailable' }, 500);
                }
            }
        ]);
        const { unmount } = render(Page);
        const name = await screen.findByLabelText('AP Name');
        await fireEvent.input(name, { target: { value: 'SavedName' } });
        const save = screen.getByRole('button', { name: /save settings/i });
        await fireEvent.click(save);

        await screen.findByText('Failed to load settings');
        await waitFor(() => expect(save).toBeEnabled());
        expect(name).toBeEnabled();
        expect(name).toHaveValue('SavedName');
        expect(screen.getByLabelText(/minutes after disconnect/i)).toBeEnabled();
        expect(screen.getByLabelText(/minutes after disconnect/i)).toHaveValue(30);
        unmount();
    });

    it('rejects invalid backup JSON before posting restore', async () => {
        const fetchMock = installDefaultFetch();
        vi.stubGlobal(
            'confirm',
            vi.fn(() => true)
        );
        const { unmount } = render(Page);

        await startRestore('{not-json');

        await screen.findByText('Selected file is not valid JSON.');
        expect(fetchMock.mock.calls.some(([url]) => url === '/api/settings/restore')).toBe(false);

        unmount();
    });

    it('rejects unrecognized backup types before posting restore', async () => {
        const fetchMock = installDefaultFetch();
        vi.stubGlobal(
            'confirm',
            vi.fn(() => true)
        );
        const { unmount } = render(Page);

        await startRestore(JSON.stringify({ _type: 'wrong_backup_type' }));

        await screen.findByText('Selected file is not a V1 Simple settings backup.');
        expect(fetchMock.mock.calls.some(([url]) => url === '/api/settings/restore')).toBe(false);

        unmount();
    });

    it('consumes captured restore success and refetches settings plus WiFi state', async () => {
        const fetchMock = installDefaultFetch(apiFixtureMatchers('settings_restore_success'));
        vi.stubGlobal(
            'confirm',
            vi.fn(() => true)
        );
        const { unmount } = render(Page);
        const payload = JSON.stringify({ _type: 'v1simple_backup', profiles: [] });

        await startRestore(payload);

        await screen.findByText('Settings restored and reloaded.');
        await waitFor(() => expect(screen.getByLabelText('AP Name')).toHaveValue('RestoredAP'));
        await screen.findByText(/SSID: RestoredWifi/);
        expect(
            fetchMock.mock.calls.some(
                ([url, init]) => url === '/api/settings/restore' && init?.body === payload
            )
        ).toBe(true);
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/device/settings')).toHaveLength(
            2
        );
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/status')).toHaveLength(2);
        expect(fetchMock.mock.calls.filter(([url]) => url === '/api/wifi/networks')).toHaveLength(
            2
        );

        unmount();
    });
});
