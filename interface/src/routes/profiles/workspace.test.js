import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { installFixtureFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import { createDefaultProfileSettings } from '$lib/features/profiles/profileSettingsAdapter';
import { PROFILE_SETTING_GROUPS } from '$lib/features/profiles/profileSettingsCatalog';
import Page from './+page.svelte';
import wifiApiFixtures from '../../test/fixtures/wifi-api.json';

async function newProfile(overrides = []) {
    installFixtureFetchMock(['frontend_core_routes', 'v1_profile_routes'], [
        ...overrides,
        { method: 'GET', match: '/api/v1/profiles', respond: jsonResponse({
            ...wifiApiFixtures.scenarios.v1_profile_routes['GET /api/v1/profiles'][0].body,
            schemaVersion: 4
        }) }
    ]);
    const view = render(Page);
    await fireEvent.click(await screen.findByRole('button', { name: 'New Profile' }));
    return { ...view, editor: document.getElementById('profile-editor') };
}
async function saveDraft(name) {
    await fireEvent.click(screen.getByRole('button', { name: 'Save as Profile' }));
    const modal = screen.getByText('Save Profile').closest('.modal-box');
    await fireEvent.input(within(modal).getByLabelText('Profile Name'), { target: { value: name } });
    await fireEvent.click(within(modal).getByRole('button', { name: 'Save', exact: true }));
    await screen.findByText(`Profile "${name}" saved`);
}

describe('profile settings workspace', () => {
    beforeEach(() => { global.confirm = vi.fn(() => true); });
    afterEach(() => vi.restoreAllMocks());

    it('exposes all 30 named user settings and preserves edits across navigation and search', async () => {
        let saved;
        const { unmount, editor } = await newProfile([{
            method: 'POST', match: '/api/v1/profile', respond: ({ init }) => {
                saved = JSON.parse(init.body);
                return jsonResponse({ success: true });
            }
        }]);
        expect(within(editor).getByRole('combobox', { name: 'Detector lights' })).toBeVisible();
        expect(within(editor).queryByRole('checkbox', { name: 'Rear Laser' })).not.toBeInTheDocument();
        await fireEvent.click(within(editor).getByRole('button', { name: /Bands & sensitivity/ }));
        const rearLaser = within(editor).getByRole('checkbox', { name: 'Rear Laser' });
        await fireEvent.click(rearLaser);
        expect(rearLaser).not.toBeChecked();
        await fireEvent.input(within(editor).getByRole('searchbox'), { target: { value: 'Gatso' } });
        expect(within(editor).getAllByRole('checkbox')).toHaveLength(1);
        await fireEvent.click(within(editor).getByRole('checkbox', { name: 'Gatso RT4' }));
        await fireEvent.click(within(editor).getByRole('button', { name: 'Show all settings' }));
        expect(within(editor).getAllByRole('checkbox')).toHaveLength(40);
        const keys = PROFILE_SETTING_GROUPS.flatMap((group) => group.fields.map((field) => field.key));
        expect([...keys, 'customFreqs'].sort()).toEqual(Object.keys(createDefaultProfileSettings()).sort());
        expect(new Set(keys).size).toBe(29);
        for (const field of PROFILE_SETTING_GROUPS.flatMap((group) => group.fields)) {
            expect(within(editor).getByRole(field.options ? 'combobox' : 'checkbox', {
                name: field.ariaLabel || field.label, exact: true
            })).toBeVisible();
        }
        expect(within(editor).getByRole('checkbox', { name: 'Rear Laser' })).toBe(rearLaser);
        await saveDraft('Complete controls');
        expect(saved.settings.laserRear).toBe(false);
        expect(saved.settings.gatsoRT4).toBe(true);
        unmount();
    });

    it('shows firmware requirements without preventing offline authoring', async () => {
        let resolveSnapshot;
        const snapshotResponse = new Promise((resolve) => { resolveSnapshot = resolve; });
        const { unmount, editor } = await newProfile([{
            method: 'GET', match: '/api/v1/snapshot', respond: () => snapshotResponse
        }]);
        await fireEvent.input(within(editor).getByRole('searchbox'), { target: { value: 'Gatso' } });
        const gatso = within(editor).getByRole('checkbox', { name: 'Gatso RT4' });
        const row = gatso.closest('.setting-row');
        expect(gatso).toBeEnabled();
        await fireEvent.click(gatso);
        expect(gatso).toBeChecked();
        expect(within(row).queryByText(/Requires V1 4.1039/)).not.toBeInTheDocument();

        // The editor is usable before the independent detector snapshot finishes.
        resolveSnapshot(jsonResponse({
            available: true, address: 'AA:BB:CC:DD:EE:FF', firmware: { value: 41038 },
            capabilities: { versionKnown: true, gen2: true }
        }));
        expect(await within(row).findByText(/Requires V1 4.1039/)).toBeVisible();
        expect(gatso).toBeEnabled();
        expect(gatso).toBeChecked();
        unmount();
    });

    it('offers keeping detector ranges independently of enabling detection', async () => {
        let saved;
        const { unmount, editor } = await newProfile([{
            method: 'POST', match: '/api/v1/profile', respond: ({ init }) => {
                saved = JSON.parse(init.body);
                return jsonResponse({ success: true });
            }
        }]);
        await fireEvent.click(within(editor).getByRole('button', { name: /Frequency ranges/ }));
        await fireEvent.change(within(editor).getByRole('combobox', { name: 'Range replacement' }), { target: { value: 'unchanged' } });
        expect(within(editor).getByRole('checkbox', { name: 'Enable Custom Frequencies' })).not.toBeChecked();
        await fireEvent.click(within(editor).getByRole('checkbox', { name: 'Enable Custom Frequencies' }));
        expect(within(editor).getByRole('combobox', { name: 'Range replacement' })).toHaveValue('unchanged');
        expect(within(editor).getByText(/To enable custom-frequency detection/)).toBeVisible();
        await fireEvent.change(within(editor).getByRole('combobox', { name: 'Apply detection settings' }), { target: { value: 'unchanged' } });
        await saveDraft('Keep detector ranges');
        expect(saved.detector.customFrequencies).toEqual({ policy: 'unchanged' });
        expect(saved.detector.userSettings).toBe('unchanged');
        expect(saved.settings.customFreqs).toBe(true);
        unmount();
    });

    it('explains the Euro and Advanced Logic conflict at the controls', async () => {
        const { unmount, editor } = await newProfile();
        await fireEvent.change(within(editor).getByRole('combobox', { name: 'Operating mode' }), { target: { value: '3' } });
        await fireEvent.click(within(editor).getByRole('button', { name: /Filters & startup/ }));
        await fireEvent.click(within(editor).getByRole('checkbox', { name: 'Euro Mode' }));
        expect(within(editor).getByText(/Euro Mode cannot be applied with Advanced Logic/)).toBeVisible();
        unmount();
    });
});
