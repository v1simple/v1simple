import { act, fireEvent, render, screen, within } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { installFixtureFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import { createDefaultInTheBoxSettings } from '#lib/features/profiles/inTheBoxSettings.js';
import Page from './+page.svelte';
import profileSaveErrors from '../../test/fixtures/profile-save-errors.json';

const existingName = 'Daily Drive';
const catalog = (profiles = [{ name: existingName }]) => ({ schemaVersion: 4, profiles });

function installPage(overrides = []) {
    return installFixtureFetchMock(['frontend_core_routes', 'v1_profile_routes'], [
        ...overrides,
        { method: 'GET', match: '/api/v1/profiles', respond: jsonResponse(catalog()) },
        { method: 'GET', match: '/api/v1/profile?', respond: jsonResponse({
            schemaVersion: 4, name: existingName, description: 'Original description',
            detector: { userSettings: 'value', customFrequencies: { policy: 'unchanged' } },
            inTheBox: createDefaultInTheBoxSettings(), settings: { xBand: true, customFreqs: false }
        }) }
    ]);
}

function mutations(fetchMock) {
    return fetchMock.mock.calls.filter(([, init]) => init?.method && init.method !== 'GET');
}

async function newDraft() {
    await fireEvent.click(await screen.findByRole('button', { name: 'New Profile', exact: true }));
    await fireEvent.input(screen.getByLabelText('Description'), { target: { value: 'Unsaved description' } });
    await fireEvent.click(screen.getByLabelText('X Band'));
    await fireEvent.click(screen.getByRole('button', { name: /^In-the-Box/ }));
    await fireEvent.click(screen.getByRole('checkbox', { name: 'K Out-of-the-Box muting' }));
    await fireEvent.click(screen.getByRole('button', { name: 'Save as Profile', exact: true }));
}

async function submitName(name) {
    const dialog = await screen.findByRole('dialog', { name: 'Save Profile' });
    await fireEvent.input(within(dialog).getByLabelText('Profile Name'), { target: { value: name } });
    await fireEvent.click(within(dialog).getByRole('button', { name: 'Save', exact: true }));
    return dialog;
}

function expectDraft(name) {
    const dialog = screen.getByRole('dialog', { name: 'Save Profile' });
    expect(within(dialog).getByLabelText('Profile Name')).toHaveValue(name);
    expect(within(dialog).getByLabelText('Description (optional)')).toHaveValue('Unsaved description');
    expect(screen.getByLabelText('Description')).toHaveValue('Unsaved description');
    expect(screen.getByRole('checkbox', { name: 'K Out-of-the-Box muting' })).toBeChecked();
    expect(within(dialog).getByRole('button', { name: 'Save', exact: true })).toBeEnabled();
    expect(screen.queryByText(/Profile ".*" saved/)).not.toBeInTheDocument();
}

describe('profile creation never replaces a saved profile', () => {
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

    it.each([existingName, ` ${existingName} `, 'daily drive'])(
        'rejects the known name %j before sending any mutation', async (name) => {
            const fetchMock = installPage();
            const { unmount } = render(Page);
            try {
                await screen.findByText(existingName, { exact: true });
                await newDraft();
                await submitName(name);
                await screen.findByText('Profile name collides with an existing profile');
                expectDraft(name);
                expect(mutations(fetchMock)).toEqual([]);
                expect(screen.getByText(existingName, { exact: true })).toBeInTheDocument();
            } finally {
                unmount();
            }
        }
    );

    it.each(['stale', 'failed', 'pending', 'partial', 'partial-failed'])(
        'uses create-only despite a %s catalog and retains a rejected draft', async (state) => {
            let releaseCatalog;
            const pendingCatalog = new Promise((resolve) => { releaseCatalog = resolve; });
            let pageReads = 0;
            const sent = [];
            const fetchMock = installPage([
                { method: 'GET', match: '/api/v1/profiles', respond: () => {
                    pageReads += 1;
                    if (state === 'stale') return jsonResponse(catalog([]));
                    if (state === 'failed') return jsonResponse({ error: 'unavailable' }, 500);
                    if (state === 'pending') return pendingCatalog;
                    if (pageReads === 1) return jsonResponse({
                        ...catalog([{ name: 'Other' }]), hasMore: true, nextCursor: 'Other'
                    });
                    return state === 'partial' ? pendingCatalog : jsonResponse({ error: 'unavailable' }, 500);
                } },
                { method: 'POST', match: '/api/v1/profile', respond: ({ init }) => {
                    sent.push(JSON.parse(init.body));
                    return sent.length === 1
                        ? jsonResponse({ error: 'Profile already exists' }, 409)
                        : jsonResponse({ success: true });
                } }
            ]);
            const { unmount } = render(Page);
            try {
                await newDraft();
                await submitName(existingName);
                await screen.findByText('Failed to save: Profile already exists');
                expectDraft(existingName);
                expect(sent).toHaveLength(1);
                expect(sent[0]).toMatchObject({
                    name: existingName, createOnly: true, description: 'Unsaved description',
                    settings: { xBand: false }, inTheBox: { bands: { k: { muteOutside: true } } }
                });
                expect(mutations(fetchMock).map(([url]) => url)).toEqual(['/api/v1/profile']);
                expect(screen.queryByText(existingName, { exact: true })).not.toBeInTheDocument();

                await submitName('Fresh Choice');
                await screen.findByText('Profile "Fresh Choice" saved');
                expect(sent).toHaveLength(2);
                expect(sent[1]).toEqual({ ...sent[0], name: 'Fresh Choice' });
                expect(mutations(fetchMock).map(([url]) => url)).toEqual([
                    '/api/v1/profile', '/api/v1/profile'
                ]);
            } finally {
                unmount();
                await act(async () => { releaseCatalog(jsonResponse(catalog([]))); });
            }
        }
    );

    it.each(profileSaveErrors)(
        'retains the complete creation draft for the firmware $name response', async (response) => {
            const sent = [];
            const fetchMock = installPage([
                { method: 'GET', match: '/api/v1/profiles', respond: jsonResponse(catalog([{ name: 'Other' }])) },
                { method: 'POST', match: '/api/v1/profile', respond: ({ init }) => {
                    sent.push(JSON.parse(init.body));
                    return sent.length === 1
                        ? jsonResponse(response.body, response.status)
                        : jsonResponse({ success: true });
                } }
            ]);
            const { unmount } = render(Page);
            try {
                await screen.findByText('Other', { exact: true });
                await newDraft();
                await submitName(existingName);
                await screen.findByText(`Failed to save: ${response.body.error}`, { exact: true, normalizer: (text) => text });
                expectDraft(existingName);
                expect(screen.getByLabelText('X Band')).not.toBeChecked();
                expect(sent).toHaveLength(1);
                expect(sent[0]).toMatchObject({
                    name: existingName, createOnly: true, description: 'Unsaved description',
                    settings: { xBand: false }, inTheBox: { bands: { k: { muteOutside: true } } }
                });
                expect(mutations(fetchMock).map(([url]) => url)).toEqual(['/api/v1/profile']);
                expect(screen.getByText('Other', { exact: true })).toBeInTheDocument();
                expect(screen.queryByText(existingName, { exact: true })).not.toBeInTheDocument();

                // A deliberate retry sends the entire preserved draft, including
                // detector policy and raw settings, under only the changed name.
                await submitName('Fresh Choice');
                await screen.findByText('Profile "Fresh Choice" saved');
                expect(sent).toHaveLength(2);
                expect(sent[1]).toEqual({ ...sent[0], name: 'Fresh Choice' });
                expect(mutations(fetchMock).map(([url]) => url)).toEqual([
                    '/api/v1/profile', '/api/v1/profile'
                ]);
                expect(screen.getByText('Other', { exact: true })).toBeInTheDocument();
            } finally {
                unmount();
            }
        }
    );

    it.each(['new', 'captured', 'copy', 'save under a name'])(
        'creates a separate profile through the %s naming dialog', async (entry) => {
            let saved;
            const fetchMock = installPage([{ method: 'POST', match: '/api/v1/profile', respond: ({ init }) => {
                saved = JSON.parse(init.body);
                return jsonResponse({ success: true });
            } }]);
            const { unmount } = render(Page);
            try {
                const row = (await screen.findByText(existingName, { exact: true })).closest('.surface-panel');
                if (entry === 'new') await newDraft();
                else if (entry === 'captured') {
                    await fireEvent.click(await screen.findByRole('button', { name: 'Start draft from captured settings' }));
                    await fireEvent.click(screen.getByRole('button', { name: 'Save as Profile', exact: true }));
                } else if (entry === 'copy') {
                    await fireEvent.click(within(row).getByRole('button', { name: 'Copy', exact: true }));
                } else {
                    await fireEvent.click(within(row).getByRole('button', { name: 'Edit', exact: true }));
                    await screen.findByText(`Editing profile: ${existingName}`);
                    await fireEvent.click(screen.getByRole('button', { name: 'Cancel', exact: true }));
                    await fireEvent.click(screen.getByRole('button', { name: 'Save under a name…', exact: true }));
                }
                await submitName('Separate Profile');
                await screen.findByText('Profile "Separate Profile" saved');
                expect(saved).toMatchObject({ name: 'Separate Profile', createOnly: true });
                expect(screen.getByText(existingName, { exact: true })).toBeInTheDocument();
                expect(screen.getByText('Separate Profile', { exact: true })).toBeInTheDocument();
                expect(mutations(fetchMock).map(([url]) => url)).toEqual(['/api/v1/profile']);
            } finally {
                unmount();
            }
        }
    );

    it.each(['rejected', 'disconnected'])('keeps a %s creation draft without retrying its write', async (failure) => {
        let saved;
        const fetchMock = installPage([{ method: 'POST', match: '/api/v1/profile', respond: ({ init }) => {
            saved = JSON.parse(init.body);
            if (failure === 'disconnected') throw new Error('Disconnected');
            return jsonResponse({ error: 'Profile storage unavailable' }, 500);
        } }]);
        const { unmount } = render(Page);
        try {
            await newDraft();
            await submitName('Separate Profile');
            await screen.findByText(failure === 'disconnected'
                ? 'Connection error' : 'Failed to save: Profile storage unavailable');
            expectDraft('Separate Profile');
            expect(saved).toMatchObject({ name: 'Separate Profile', createOnly: true, settings: { xBand: false } });
            expect(mutations(fetchMock).map(([url]) => url)).toEqual(['/api/v1/profile']);
            expect(screen.queryByText('Separate Profile', { exact: true })).not.toBeInTheDocument();
        } finally {
            unmount();
        }
    });
});
