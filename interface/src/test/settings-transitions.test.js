import { act, cleanup, fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import Settings from '../lib/features/settings/SettingsPage.svelte';
import Colors from '../lib/features/colors/ColorsPage.svelte';
import Audio from '../routes/audio/+page.svelte';
import AutoPush from '../routes/autopush/+page.svelte';
import Profiles from '../lib/features/profiles/ProfilesPage.svelte';
import Gps from '../lib/features/settings/SettingsGpsCard.svelte';
import Obd from '../lib/features/settings/SettingsObdCard.svelte';
import { retainRuntimeStatus, runtimeStatus } from '../lib/stores/runtimeStatus.svelte.js';
import { installFetchMock, installFixtureFetchMock, jsonResponse } from './fetch-mock.js';
import { cloneDefaultColors } from '../lib/utils/colors.js';
import fixtures from './fixtures/wifi-api.json';
const deferred = () => { let resolve; const promise = new Promise(r => resolve = r); return { promise, resolve }; };
const settle = async (d, r) => { await act(async () => { d.resolve(r); await d.promise; }); await act(); };
const slots = [{ index: 0, ssid: 'HomeWifi', label: 'Home', priority: 0, hasPassword: true, configured: true, lastConnectedAtSec: 0 }];
const wifi = (state) => state === 'connected' ? { enabled: true, state, connectedSSID: 'HomeWifi', connectedSlotIndex: 0, ip: '192.168.1.2', rssi: -50 } : { enabled: state !== 'disabled', state, savedSSID: 'HomeWifi' };
function settingsFetch(overrides = []) {
    return installFetchMock([...overrides,
        { method: 'GET', match: '/api/device/settings', respond: jsonResponse({ ap_ssid: 'V1', autoPowerOffMinutes: 30 }) },
        { method: 'GET', match: '/api/wifi/status', respond: jsonResponse(wifi('disconnected')) },
        { method: 'GET', match: '/api/wifi/networks', respond: jsonResponse({ slots }) },
        { method: 'GET', match: '/api/status', respond: jsonResponse({ maintenanceBoot: true, time: { valid: false } }) },
        { method: 'POST', match: '/api/wifi/networks', respond: jsonResponse({ success: true }) },
        { method: 'POST', match: '/api/wifi/enable', respond: jsonResponse({ success: true }) }
    ], ({ method, url }) => { throw new Error(`Unexpected ${method} ${url}`); });
}
beforeEach(() => { vi.stubGlobal('confirm', vi.fn(() => true)); const m = new Map(); Object.defineProperty(window, 'localStorage', { configurable: true, value: { getItem: k => m.get(k) ?? null, setItem: (k, v) => m.set(k, String(v)), removeItem: k => m.delete(k) } }); });
afterEach(() => { cleanup(); vi.useRealTimers(); vi.restoreAllMocks(); vi.unstubAllGlobals(); });
describe('automatic WiFi status refresh boundary', () => {
    it.each(['mount connecting', 'enable scanning', 'enable connecting', 'link loss', 'reconnect'])('refreshes mounted card after %s and stops polling on unmount', async (kind) => {
        vi.useFakeTimers();
        let state = kind.startsWith('enable') ? 'disabled' : kind === 'link loss' ? 'connected' : kind === 'reconnect' ? 'disconnected' : 'connecting';
        let reads = 0;
        let sharedReads = 0;
        settingsFetch([
            { method: 'GET', match: '/api/wifi/status', respond: () => { reads++; return jsonResponse(wifi(state)); } },
            { method: 'POST', match: '/api/wifi/enable', respond: () => { state = kind === 'enable scanning' ? 'disconnected' : 'connecting'; return jsonResponse({ success: true }); } },
            { method: 'GET', match: '/api/status', respond: () => { sharedReads++; return jsonResponse({ maintenanceBoot: true, wifi: { sta_connected: state === 'connected', sta_ip: state === 'connected' ? '192.168.1.2' : '', ap_active: true }, device: { hostname: 'v1' } }); } }
        ]);
        const release = retainRuntimeStatus({ needsStatus: true });
        const first = render(Settings);
        let firstMounted = true;
        try {
            if (kind.startsWith('enable'))
                await screen.findByText(/WiFi client is disabled/);
            else
                await screen.findByText('Home');
            if (kind.startsWith('enable')) {
                await fireEvent.click(screen.getByText('WiFi Client').closest('.surface-card').querySelector('input'));
                await screen.findByText('WiFi client enabled');
            }
            const readCount = reads;
            state = kind === 'link loss' ? 'disconnected' : 'connected';
            await act(async () => { await vi.advanceTimersByTimeAsync(30000); });
            expect(sharedReads).toBeGreaterThan(3);
            let shared;
            const unsub = runtimeStatus.subscribe(v => shared = v);
            unsub();
            expect(shared.wifi.sta_connected).toBe(state === 'connected');
            expect(reads).toBeGreaterThan(readCount);
            if (state === 'connected') {
                expect(screen.getByRole('button', { name: 'Disconnect' })).toBeInTheDocument();
                expect(screen.getByText(/Connected to HomeWifi •/)).toBeInTheDocument();
            }
            else {
                expect(screen.queryByRole('button', { name: 'Disconnect' })).not.toBeInTheDocument();
                expect(screen.queryByText(/Connected to HomeWifi •/)).not.toBeInTheDocument();
            }
            first.unmount();
            firstMounted = false;
            const finalReads = reads;
            await act(async () => { await vi.advanceTimersByTimeAsync(6000); });
            expect(reads).toBe(finalReads);
        }
        finally {
            if (firstMounted)
                first.unmount();
            release();
        }
    });
    it('control: explicit Test polls the same asynchronous connection to completion', async () => {
        vi.useFakeTimers();
        let state = 'disconnected';
        let reads = 0;
        settingsFetch([{ method: 'GET', match: '/api/wifi/status', respond: () => { reads++; return jsonResponse(wifi(state)); } },
            { method: 'POST', match: '/api/wifi/networks/test', respond: () => { state = 'connecting'; return jsonResponse({ success: true }); } }]);
        render(Settings);
        await screen.findByText('Home');
        await fireEvent.click(screen.getByRole('button', { name: 'Test' }));
        await screen.findByText('Testing connection to HomeWifi...');
        state = 'connected';
        await act(async () => { await vi.advanceTimersByTimeAsync(1000); });
        expect(screen.getByText('Connected to HomeWifi')).toBeInTheDocument();
        expect(reads).toBe(3);
    });
});
describe('mounted WiFi refresh lifecycle', () => {
    it('deduplicates a delayed background read, ignores it after unmount, and recovers on remount', async () => {
        vi.useFakeTimers();
        const pending = deferred();
        let reads = 0;
        settingsFetch([{ method: 'GET', match: '/api/wifi/status', respond: () => {
                    reads++;
                    return reads === 2 ? pending.promise : jsonResponse(wifi('disconnected'));
                } }]);
        const first = render(Settings);
        await screen.findByText('Home');
        await act(async () => { await vi.advanceTimersByTimeAsync(6000); });
        expect(reads).toBe(2);
        first.unmount();
        const second = render(Settings);
        await screen.findByText('Home');
        expect(reads).toBe(3);
        await settle(pending, jsonResponse(wifi('connected')));
        expect(screen.queryByRole('button', { name: 'Disconnect' })).not.toBeInTheDocument();
        second.unmount();
        await act(async () => { await vi.advanceTimersByTimeAsync(6000); });
        expect(reads).toBe(3);
    });
});
describe('same-session editable draft during pending save', () => {
    it.each(['save', 'reset'])('display %s preserves a later brightness edit through canonical readback', async (action) => {
        const pending = deferred();
        let stored = cloneDefaultColors();
        let posted;
        installFixtureFetchMock(['display_colors_success', 'quiet_settings_success'], [
            { method: 'GET', match: '/api/display/settings', respond: () => jsonResponse(stored) },
            { method: 'POST', match: action === 'save' ? '/api/display/settings' : '/api/display/settings/reset', respond: ({ init }) => { posted = init.body; if (action === 'save')
                    stored = { ...stored, brightness: Number(init.body.get('brightness')) }; stored.hideBatteryIcon = true; return pending.promise; } }
        ]);
        render(Colors);
        const slider = await screen.findByRole('slider', { name: 'Screen brightness' });
        await fireEvent.input(slider, { target: { value: '100' } });
        await fireEvent.click(screen.getByRole('button', { name: action === 'save' ? 'Save display settings' : 'Reset Defaults' }));
        expect(slider).toBeEnabled();
        await fireEvent.input(slider, { target: { value: '120' } });
        expect(slider).toHaveValue('120');
        await settle(pending, jsonResponse({ success: true }));
        await screen.findByText(action === 'save' ? 'Colors saved! Previewing on display...' : 'Colors reset to defaults!');
        const fresh = await screen.findByRole('slider', { name: 'Screen brightness' });
        expect(fresh).toHaveValue('120');
        await fireEvent.click(screen.getByRole('button', { name: 'Status' }));
        expect(screen.getByRole('checkbox', { name: /^Show battery level/ })).not.toBeChecked();
        if (action === 'save')
            expect(posted.get('brightness')).toBe('100');
    });
    it('audio preserves a later volume edit through canonical readback', async () => {
        const pending = deferred();
        let stored = { voiceVolume: 50 };
        let sent;
        installFixtureFetchMock('audio_settings_success', [
            { method: 'GET', match: '/api/audio/settings', respond: () => jsonResponse(stored) },
            { method: 'POST', match: '/api/audio/settings', respond: ({ init }) => { sent = init.body; stored = { voiceVolume: Number(sent.get('voiceVolume')), voiceDirectionEnabled: false }; return pending.promise; } }
        ]);
        render(Audio);
        const input = await screen.findByRole('slider');
        await fireEvent.input(input, { target: { value: '60' } });
        await fireEvent.click(screen.getByRole('button', { name: /save audio settings/i }));
        expect(input).toBeEnabled();
        await fireEvent.input(input, { target: { value: '70' } });
        expect(input).toHaveValue('70');
        await fireEvent.click(screen.getByRole('checkbox', { name: /announce bogey count/i }));
        await settle(pending, jsonResponse({ success: true }));
        expect(await screen.findByRole('slider')).toHaveValue('70');
        expect(sent.get('voiceVolume')).toBe('60');
        expect(screen.getByRole('checkbox', { name: /announce bogey count/i })).not.toBeChecked();
        expect(screen.getByRole('checkbox', { name: /include direction/i })).not.toBeChecked();
    });
    it.each(['name', 'color picker'])('Auto-Push preserves a later unsent %s draft in the same editor', async (field) => {
        const pending = deferred();
        const stored = structuredClone(fixtures.scenarios.autopush_routes['GET /api/autopush/slots'][0].body);
        stored.schemaVersion = 4;
        let sent;
        installFixtureFetchMock(['frontend_core_routes', 'autopush_routes', 'v1_profile_routes'], [
            { method: 'GET', match: '/api/autopush/slots', respond: () => jsonResponse(stored) },
            { method: 'POST', match: '/api/autopush/slot', respond: ({ init }) => { sent = init.body; stored.slots[0].name = sent.get('name'); return pending.promise; } }
        ]);
        render(AutoPush);
        await screen.findByText('Highway');
        await fireEvent.click(screen.getAllByRole('button', { name: 'Edit' })[0]);
        const name = screen.getByLabelText('Slot name');
        await fireEvent.input(name, { target: { value: 'Submitted' } });
        await fireEvent.click(screen.getByRole('button', { name: /^save slot$/i }));
        expect(name).toBeEnabled();
        if (field === 'name') {
            await fireEvent.input(name, { target: { value: 'Later' } });
        } else {
            await fireEvent.click(screen.getByRole('button', { name: 'Choose Submitted color' }));
            await fireEvent.input(screen.getByRole('slider', { name: /^Red/ }), { target: { value: '80' } });
        }
        await settle(pending, jsonResponse({ success: true }));
        await screen.findByRole('heading', { name: 'SUBMITTED' });
        expect(screen.getByLabelText('Slot name')).toHaveValue(field === 'name' ? 'Later' : 'Submitted');
        if (field === 'color picker') {
            expect(screen.getByRole('slider', { name: /^Red/ })).toHaveValue('80');
            await fireEvent.click(screen.getByRole('button', { name: 'Apply', exact: true }));
            expect(screen.getByLabelText('Slot name')).toBeInTheDocument();
        }
        expect(sent.get('name')).toBe('SUBMITTED');
    });
    it.each(['SSID', 'Label', 'Password', 'Priority', 'scan'])('WiFi %s save preserves a later unsent draft', async (kind) => {
        const pending = deferred();
        let sent;
        settingsFetch([
            { method: 'POST', match: u => u === '/api/wifi/networks', respond: ({ init }) => { sent = JSON.parse(init.body); return pending.promise; } },
            { method: 'POST', match: '/api/wifi/scan', respond: jsonResponse({ scanning: false, networks: [{ ssid: 'GarageAP', rssi: -35, secure: true }] }) }
        ]);
        render(Settings);
        await screen.findByText('Home');
        let input;
        if (kind !== 'scan') {
            await fireEvent.click(screen.getAllByRole('button', { name: 'Enter manually' })[0]);
            await fireEvent.input(screen.getByLabelText('SSID'), { target: { value: 'SubmittedAP' } });
            input = screen.getByLabelText(kind);
            await fireEvent.click(screen.getByRole('button', { name: 'Save' }));
        }
        else {
            await fireEvent.click(screen.getAllByRole('button', { name: 'Pick from scan' })[0]);
            await fireEvent.click(await screen.findByRole('button', { name: /GarageAP/ }));
            input = screen.getByLabelText('Password');
            await fireEvent.input(input, { target: { value: 'submitted-pass' } });
            await fireEvent.click(screen.getByRole('button', { name: 'Save Network' }));
        }
        expect(input).toBeEnabled();
        const later = kind === 'Priority' ? '3' : 'later-value';
        await fireEvent.input(input, { target: { value: later } });
        await settle(pending, jsonResponse({ success: true }));
        await screen.findByText(`Saved ${sent.ssid}`);
        expect(screen.getByDisplayValue(later)).toBeInTheDocument();
        expect(sent[kind !== 'scan' ? 'ssid' : 'password']).toBe(kind !== 'scan' ? 'SubmittedAP' : 'submitted-pass');
    });
});
describe('peripheral configuration concurrent readback', () => {
    it('GPS blocks overlapping edits until failed-save reconciliation finishes', async () => {
        const stale = deferred();
        let reads = 0, posts = 0, stored = { gpsEnabled: true, gpsBaud: 9600 };
        installFetchMock([
            { method: 'GET', match: '/api/gps/config', respond: () => { reads++; return reads === 2 ? stale.promise : jsonResponse(stored); } },
            { method: 'POST', match: '/api/gps/config', respond: ({ init }) => { posts++; if (posts === 1)
                    return jsonResponse({}, 500); stored = { ...stored, ...JSON.parse(init.body) }; return jsonResponse({ success: true }); } }
        ]);
        render(Gps);
        const baud = await screen.findByRole('combobox');
        await fireEvent.change(baud, { target: { value: '38400' } });
        await waitFor(() => expect(reads).toBe(2));
        expect(baud).toBeDisabled();
        expect(screen.getByRole('checkbox')).toBeDisabled();
        await settle(stale, jsonResponse({ gpsEnabled: true, gpsBaud: 9600 }));
        await waitFor(() => expect(baud).toBeEnabled());
        expect(baud).toHaveValue('9600');
        await fireEvent.change(baud, { target: { value: '115200' } });
        await waitFor(() => expect(posts).toBe(2));
        expect(stored.gpsBaud).toBe(115200);
        expect(baud).toHaveValue('115200');
    });
    it('OBD locks RSSI and timing until canonical readback finishes', async () => {
        const stale = deferred();
        let reads = 0, stored = { enabled: true, minRssi: -80 };
        installFetchMock([
            { method: 'GET', match: '/api/obd/config', respond: () => { reads++; return reads === 2 ? stale.promise : jsonResponse(stored); } },
            { method: 'GET', match: '/api/obd/devices', respond: jsonResponse({ devices: [] }) },
            { method: 'POST', match: '/api/obd/config', respond: ({ init }) => { stored = { ...stored, ...JSON.parse(init.body) }; return jsonResponse({ success: true }); } }
        ]);
        render(Obd);
        const rssi = await screen.findByLabelText('Min RSSI (dBm)');
        await fireEvent.input(rssi, { target: { value: '-70' } });
        await fireEvent.change(rssi);
        await waitFor(() => expect(reads).toBe(2));
        expect(rssi).toBeDisabled();
        expect(screen.getByLabelText('Enable OBD')).toBeDisabled();
        expect(screen.getByLabelText('OBD Scan Window (ms)')).toBeDisabled();
        await settle(stale, jsonResponse({ enabled: true, minRssi: -70 }));
        await waitFor(() => expect(rssi).toBeEnabled());
        expect(rssi).toHaveValue(-70);
        await fireEvent.input(rssi, { target: { value: '-60' } });
        await fireEvent.change(rssi);
        await waitFor(() => expect(reads).toBe(3));
        expect(stored.minRssi).toBe(-60);
        expect(rssi).toHaveValue(-60);
    });
});
describe('profile catalog ownership', () => {
    it.each(['initial', 'paginated', 'failed'])('an older %s catalog cannot remove a newly saved profile', async (kind) => {
        const stale = deferred();
        let sent;
        let reads = 0;
        installFixtureFetchMock(['frontend_core_routes', 'v1_profile_routes'], [
            { method: 'GET', match: url => url.startsWith('/api/v1/profiles'), respond: () => {
                    reads++;
                    if (kind === 'paginated' && reads === 1)
                        return jsonResponse({ schemaVersion: 4, profiles: [{ name: 'Existing' }], hasMore: true, nextCursor: 'Existing' });
                    if (!sent)
                        return stale.promise;
                    return jsonResponse({ schemaVersion: 4, profiles: [{ name: 'Existing' }, { name: sent.name, description: sent.description }] });
                } },
            { method: 'POST', match: '/api/v1/profile', respond: ({ init }) => { sent = JSON.parse(init.body); return jsonResponse({ success: true }); } }
        ]);
        render(Profiles);
        if (kind === 'paginated')
            await waitFor(() => expect(reads).toBe(2));
        await fireEvent.click(await screen.findByRole('button', { name: 'New Profile' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Save as Profile' }));
        const modal = screen.getByText('Save Profile', { exact: true }).closest('.modal-box');
        await fireEvent.input(screen.getByLabelText('Profile Name'), { target: { value: 'Saved during loading' } });
        await fireEvent.click(within(modal).getByRole('button', { name: 'Save', exact: true }));
        await screen.findByText('Profile "Saved during loading" saved');
        expect(sent.name).toBe('Saved during loading');
        await settle(stale, jsonResponse({ schemaVersion: 4, profiles: [] }, kind === 'failed' ? 500 : 200));
        await screen.findByText('Browse saved profiles (2)');
        expect(screen.getByRole('button', { name: /apply saved profile to v1/i })).toBeInTheDocument();
        expect(screen.queryByText(/No saved profiles/)).not.toBeInTheDocument();
        expect(screen.queryByText('Failed to load profiles')).not.toBeInTheDocument();
        expect(reads).toBe(kind === 'paginated' ? 3 : 2);
    });
});
