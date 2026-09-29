import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { installFixtureFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import { createDefaultInTheBoxSettings } from '$lib/features/profiles/inTheBoxSettings';
import { DETECTOR_OPERATION_COMPONENTS, DETECTOR_OPERATION_STORAGE_KEY } from '$lib/features/profiles/detectorOperation';
import Page from './+page.svelte';

function profileWithBoxes() {
    const inTheBox = createDefaultInTheBoxSettings();
    inTheBox.bands.ka.unmuteInside = true;
    inTheBox.boxes.kaMid.enabled = false;
    inTheBox.boxes.k.lowerMHz = 24060;
    return {
        schemaVersion: 4, name: 'Boxes', description: 'Existing box choices', inTheBox,
        detector: { userSettings: 'unchanged', customFrequencies: { policy: 'unchanged' } },
        settings: { customFreqs: false }
    };
}

function installPage(overrides = [], profile = profileWithBoxes()) {
    return installFixtureFetchMock(['frontend_core_routes', 'v1_profile_routes'], [
        ...overrides,
        { method: 'GET', match: '/api/v1/profiles', respond: jsonResponse({
            schemaVersion: 4, detectorConfigurationOwner: 'profile', profiles: [{ name: 'Boxes' }]
        }) },
        { method: 'GET', match: '/api/v1/profile?', respond: jsonResponse(profile) }
    ]);
}

async function openBoxes() {
    await fireEvent.click(await screen.findByRole('button', { name: /^In-the-Box/ }));
}

async function editSaved() {
    const row = (await screen.findByText('Boxes', { exact: true })).closest('.surface-panel');
    await fireEvent.click(within(row).getByRole('button', { name: 'Edit', exact: true }));
    await screen.findByText('Editing profile: Boxes');
    await openBoxes();
}

async function beginSaveAs(name) {
    await fireEvent.click(screen.getByRole('button', { name: 'Save as Profile' }));
    const modal = screen.getByText('Save Profile', { exact: true }).closest('.modal-box');
    await fireEvent.input(within(modal).getByLabelText('Profile Name'), { target: { value: name } });
    await fireEvent.click(within(modal).getByRole('button', { name: 'Save', exact: true }));
    return modal;
}

describe('In-the-Box profile editor', () => {
    beforeEach(() => {
        global.confirm = vi.fn(() => true);
        const values = new Map();
        Object.defineProperty(window, 'localStorage', {
            configurable: true,
            value: {
                getItem: (key) => values.get(key) ?? null,
                setItem: (key, value) => values.set(key, String(value)),
                removeItem: (key) => values.delete(key)
            }
        });
    });
    afterEach(() => vi.restoreAllMocks());

    it('finds real app controls, preserves hidden edits, and saves them apart from detector ranges', async () => {
        let saved;
        installPage([{ method: 'POST', match: '/api/v1/profile', respond: ({ init }) => {
            saved = JSON.parse(init.body);
            return jsonResponse({ success: true });
        } }]);
        const { unmount } = render(Page);
        await fireEvent.click(await screen.findByRole('button', { name: 'New Profile' }));
        await fireEvent.input(screen.getByRole('searchbox'), { target: { value: 'Out-of-the-Box' } });
        const actions = screen.getAllByRole('checkbox').filter((input) => /muting|unmuting/.test(input.getAttribute('aria-label') || ''));
        expect(actions).toHaveLength(8);
        for (const action of actions) expect(action).not.toBeChecked();
        expect(screen.getAllByRole('checkbox', { name: /^Enable .* box$/ })).toHaveLength(6);
        expect(screen.getByText(/Muting affects the whole detector/)).toBeVisible();
        await fireEvent.click(screen.getByRole('checkbox', { name: 'K Out-of-the-Box muting' }));
        await fireEvent.click(screen.getByRole('checkbox', { name: 'Ka In-the-Box unmuting' }));
        await fireEvent.click(screen.getByRole('checkbox', { name: 'Enable Ka high box' }));
        await fireEvent.input(screen.getByRole('spinbutton', { name: 'K box lower MHz' }), { target: { value: '24060' } });
        await fireEvent.input(screen.getByRole('spinbutton', { name: 'K box upper MHz' }), { target: { value: '24060' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Clear search' }));
        await fireEvent.click(screen.getByRole('button', { name: /^Lights, mode & volume/ }));
        expect(screen.queryByRole('checkbox', { name: 'K Out-of-the-Box muting' })).not.toBeInTheDocument();
        await beginSaveAs('App boxes');
        await screen.findByText('Profile "App boxes" saved');
        expect(saved.schemaVersion).toBe(4);
        expect(saved.inTheBox.bands.k.muteOutside).toBe(true);
        expect(saved.inTheBox.bands.ka.unmuteInside).toBe(true);
        expect(saved.inTheBox.boxes.kaHigh.enabled).toBe(false);
        expect(saved.inTheBox.boxes.k).toEqual({ enabled: true, lowerMHz: 24060, upperMHz: 24060 });
        expect(saved.settings.customFreqs).toBe(false);
        expect(saved.detector.customFrequencies.definitions).toEqual([
            { index: 0, lowerMHz: 23910, upperMHz: 24250 },
            { index: 1, lowerMHz: 33400, upperMHz: 36002 }
        ]);
        await openBoxes();
        expect(screen.getByRole('checkbox', { name: 'K Out-of-the-Box muting' })).toBeDisabled();
        expect(screen.getByRole('checkbox', { name: 'K Out-of-the-Box muting' })).toBeChecked();
        unmount();
    });

    it('copies all box choices and saves under a new profile identity', async () => {
        let saved;
        const source = profileWithBoxes();
        installPage([{ method: 'POST', match: '/api/v1/profile', respond: ({ init }) => {
            saved = JSON.parse(init.body);
            return jsonResponse({ success: true });
        } }], source);
        const { unmount } = render(Page);
        const row = (await screen.findByText('Boxes', { exact: true })).closest('.surface-panel');
        await fireEvent.click(within(row).getByRole('button', { name: 'Copy' }));
        const modal = (await screen.findByText('Save Profile', { exact: true })).closest('.modal-box');
        await fireEvent.click(within(modal).getByRole('button', { name: 'Save', exact: true }));
        await screen.findByText('Profile "Boxes copy" saved');
        expect(saved.createOnly).toBe(true);
        expect(saved.inTheBox).toEqual(source.inTheBox);
        expect(saved.detector.customFrequencies).toEqual({ policy: 'unchanged' });
        unmount();
    });

    it('loads a legacy profile with app actions off and resets an edited profile to those defaults', async () => {
        let saved;
        const legacy = profileWithBoxes();
        delete legacy.inTheBox;
        legacy.schemaVersion = 3;
        installPage([{ method: 'POST', match: '/api/v1/profile', respond: ({ init }) => {
            saved = JSON.parse(init.body);
            return jsonResponse({ success: true });
        } }], legacy);
        const { unmount } = render(Page);
        await editSaved();
        expect(screen.getByRole('checkbox', { name: 'Ka In-the-Box unmuting' })).not.toBeChecked();
        await fireEvent.click(screen.getByRole('checkbox', { name: 'K Out-of-the-Box muting' }));
        await fireEvent.input(screen.getByRole('spinbutton', { name: 'K box lower MHz' }), { target: { value: '23900' } });
        await fireEvent.click(screen.getByText('Reset this draft', { exact: true }));
        await fireEvent.click(screen.getByRole('button', { name: 'Reset this draft to local defaults' }));
        expect(screen.getByRole('checkbox', { name: 'K Out-of-the-Box muting' })).not.toBeChecked();
        expect(screen.getByRole('spinbutton', { name: 'K box lower MHz' })).toHaveValue(24050);
        await fireEvent.click(screen.getByRole('button', { name: 'Save Profile', exact: true }));
        await screen.findByText('Profile "Boxes" saved');
        expect(saved.inTheBox).toEqual(createDefaultInTheBoxSettings());
        unmount();
    });

    it('starts captured detector drafts with app actions off instead of inheriting the edited profile', async () => {
        installPage();
        const { unmount } = render(Page);
        await editSaved();
        expect(screen.getByRole('checkbox', { name: 'Ka In-the-Box unmuting' })).toBeChecked();
        await fireEvent.click(screen.getByRole('button', { name: 'Start draft from captured settings' }));
        await openBoxes();
        expect(screen.getByRole('checkbox', { name: 'Ka In-the-Box unmuting' })).not.toBeChecked();
        expect(screen.getByRole('spinbutton', { name: 'K box lower MHz' })).toHaveValue(24050);
        expect(screen.getByText(/detector cannot report these app settings/)).toBeVisible();
        unmount();
    });

    it('rejects invalid box edges before sending a profile', async () => {
        const fetchMock = installPage();
        const { unmount } = render(Page);
        await editSaved();
        await fireEvent.input(screen.getByRole('spinbutton', { name: 'X box lower MHz' }), { target: { value: '10499' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Save Profile', exact: true }));
        expect(screen.getAllByText(/X box needs whole MHz values from 10500 to 10550/).length).toBeGreaterThan(0);
        expect(fetchMock.mock.calls.some(([url, init]) => url === '/api/v1/profile' && init?.method === 'POST')).toBe(false);
        unmount();
    });

    it.each([
        ['K', ['K']],
        ['Ka', ['Ka low', 'Ka middle', 'Ka high']]
    ])('explains outside muting with every %s box disabled only when muting is enabled', async (band, boxes) => {
        installPage();
        const { unmount } = render(Page);
        await fireEvent.click(await screen.findByRole('button', { name: 'New Profile' }));
        await openBoxes();
        const caption = `With no boxes enabled, every ${band} alert qualifies for outside muting.`;
        expect(screen.queryByText(caption)).not.toBeInTheDocument();
        for (const box of boxes) {
            await fireEvent.click(screen.getByRole('checkbox', { name: `Enable ${box} box` }));
        }
        expect(screen.queryByText(caption)).not.toBeInTheDocument();
        const mute = screen.getByRole('checkbox', { name: `${band} Out-of-the-Box muting` });
        await fireEvent.click(mute);
        expect(screen.getByText(caption)).toBeVisible();
        await fireEvent.click(screen.getByRole('checkbox', { name: `Enable ${boxes[0]} box` }));
        expect(screen.queryByText(caption)).not.toBeInTheDocument();
        await fireEvent.click(screen.getByRole('checkbox', { name: `Enable ${boxes[0]} box` }));
        expect(screen.getByText(caption)).toBeVisible();
        await fireEvent.click(mute);
        expect(screen.queryByText(caption)).not.toBeInTheDocument();
        unmount();
    });

    it('explains a partial apply when app settings could not be committed', async () => {
        window.localStorage.setItem(DETECTOR_OPERATION_STORAGE_KEY, '42');
        const components = Object.fromEntries(DETECTOR_OPERATION_COMPONENTS.map((name) => [name, {
            requested: false, sent: false, verified: false, outcome: 'not_requested', reason: 'none'
        }]));
        installPage([{ method: 'GET', match: '/api/autopush/status?operationId=42', respond: jsonResponse({
            operationId: 42, kind: 'apply_profile', source: 'maintenance_ui', profileName: 'Boxes',
            targetAddress: 'AA:BB:CC:DD:EE:FF', returnToMaintenance: true,
            state: 'partial', result: 'partial', terminal: true,
            reason: 'in_the_box_persist_failed', components
        }) }]);
        const { unmount } = render(Page);
        expect(await screen.findByText(/could not confirm saving the In-the-Box choices/)).toBeVisible();
        expect(screen.queryByText(/malformed or mismatched status/)).not.toBeInTheDocument();
        expect(window.localStorage.getItem(DETECTOR_OPERATION_STORAGE_KEY)).toBeNull();
        unmount();
    });

    it.each(['existing', 'new'])('preserves box edits made during a pending %s profile save', async (kind) => {
        let saved;
        let completeSave;
        installPage([{ method: 'POST', match: '/api/v1/profile', respond: ({ init }) => {
            saved = JSON.parse(init.body);
            return new Promise((resolve) => { completeSave = resolve; });
        } }]);
        const { unmount } = render(Page);
        let name = 'Boxes';
        if (kind === 'existing') {
            await editSaved();
            await fireEvent.click(screen.getByRole('button', { name: 'Save Profile', exact: true }));
        } else {
            await fireEvent.click(await screen.findByRole('button', { name: 'New Profile' }));
            await openBoxes();
            name = 'New boxes';
            const modal = await beginSaveAs(name);
            await fireEvent.click(within(modal).getByRole('button', { name: 'Cancel', exact: true }));
        }
        await waitFor(() => expect(completeSave).toBeTypeOf('function'));
        await fireEvent.input(screen.getByRole('spinbutton', { name: 'K box lower MHz' }), { target: { value: '23999' } });
        await fireEvent.click(screen.getByRole('checkbox', { name: 'X Out-of-the-Box muting' }));
        completeSave(jsonResponse({ success: true }));
        await screen.findByText(`Profile "${name}" saved`);
        expect(saved.inTheBox.boxes.k.lowerMHz).toBe(kind === 'existing' ? 24060 : 24050);
        expect(saved.inTheBox.bands.x.muteOutside).toBe(false);
        expect(screen.getByRole('spinbutton', { name: 'K box lower MHz' })).toHaveValue(23999);
        expect(screen.getByRole('checkbox', { name: 'X Out-of-the-Box muting' })).toBeChecked();
        expect(screen.getByRole('checkbox', { name: 'X Out-of-the-Box muting' })).toBeEnabled();
        expect(screen.getByRole('button', { name: 'Save Profile', exact: true })).toBeEnabled();
        unmount();
    });
});
