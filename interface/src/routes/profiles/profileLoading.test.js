import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { installFixtureFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import { createDefaultInTheBoxSettings } from '$lib/features/profiles/inTheBoxSettings';
import Page from './+page.svelte';

function savedProfile(name) {
    return {
        schemaVersion: 4,
        name,
        description: `${name} description`,
        detector: { userSettings: 'value', customFrequencies: { policy: 'unchanged' } },
        inTheBox: createDefaultInTheBoxSettings(),
        settings: { xBand: true, customFreqs: false }
    };
}

function pendingResponse(ok = true) {
    let resolveBody;
    let rejectBody;
    const body = new Promise((resolve, reject) => {
        resolveBody = resolve;
        rejectBody = reject;
    });
    const readBody = vi.fn(() => body);
    return {
        response: { ok, json: readBody, text: readBody },
        readBody,
        async finish(value, rejected = false) {
            await act(async () => {
                if (rejected) rejectBody(new Error('Disconnected'));
                else resolveBody(value);
                await body.catch(() => {});
            });
            // The body is an ordinary promise: flush the fetch finalizer and UI continuation.
            await act();
        }
    };
}

function installProfiles(profiles, overrides = []) {
    installFixtureFetchMock(['frontend_core_routes', 'v1_profile_routes'], [
        ...overrides,
        { method: 'GET', match: '/api/v1/profiles', respond: jsonResponse({
            schemaVersion: 4, detectorConfigurationOwner: 'profile', profiles
        }) },
        ...profiles.map((profile) => ({
            method: 'GET', match: `/api/v1/profile?name=${encodeURIComponent(profile.name)}`,
            respond: { ok: true, json: async () => profile }
        }))
    ]);
}

async function chooseProfile(name, action) {
    const row = (await screen.findByText(name, { exact: true })).closest('.surface-panel');
    await fireEvent.click(within(row).getByRole('button', { name: action, exact: true }));
}

describe('profile editor selection during a pending load', () => {
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

    it.each(['Edit', 'Copy'])('preserves a new draft when an earlier %s response arrives', async (action) => {
        const load = pendingResponse();
        const saved = savedProfile('Daily Drive');
        installProfiles([saved], [
            { method: 'GET', match: '/api/v1/profile?name=Daily%20Drive', respond: load.response }
        ]);
        const { unmount } = render(Page);
        try {
            await chooseProfile(saved.name, action);
            await waitFor(() => expect(load.readBody).toHaveBeenCalledOnce());
            await fireEvent.click(screen.getByRole('button', { name: 'New Profile', exact: true }));
            await fireEvent.input(screen.getByLabelText('Description'), {
                target: { value: 'My newer draft' }
            });
            await fireEvent.click(screen.getByRole('button', { name: /^In-the-Box/ }));
            await fireEvent.click(screen.getByRole('checkbox', { name: 'K Out-of-the-Box muting' }));
            await fireEvent.input(screen.getByRole('spinbutton', { name: 'K box lower MHz' }), {
                target: { value: '24060' }
            });

            await load.finish(saved);

            expect.soft(screen.getByLabelText('Description')).toHaveValue('My newer draft');
            expect.soft(screen.getByRole('checkbox', { name: 'K Out-of-the-Box muting' })).toBeChecked();
            expect.soft(screen.getByRole('spinbutton', { name: 'K box lower MHz' })).toHaveValue(24060);
            expect.soft(screen.queryByText('Editing profile: Daily Drive')).not.toBeInTheDocument();
            expect.soft(screen.queryByLabelText('Profile Name')).not.toBeInTheDocument();
        } finally {
            unmount();
        }
    });

    it.each([['Edit', 'Copy'], ['Copy', 'Edit']])(
        'retains the later %s selection when the previous %s body arrives last',
        async (latestAction, oldAction) => {
            const oldLoad = pendingResponse();
            const oldProfile = savedProfile('Alpha');
            const latestProfile = savedProfile('Beta');
            installProfiles([oldProfile, latestProfile], [
                { method: 'GET', match: '/api/v1/profile?name=Alpha', respond: oldLoad.response }
            ]);
            const { unmount } = render(Page);
            try {
                await chooseProfile('Alpha', oldAction);
                await waitFor(() => expect(oldLoad.readBody).toHaveBeenCalledOnce());
                await chooseProfile('Beta', latestAction);
                await waitFor(() => expect(screen.getByLabelText('Description')).toHaveValue('Beta description'));
                await oldLoad.finish(oldProfile);
                expect(screen.getByLabelText('Description')).toHaveValue('Beta description');
                if (latestAction === 'Edit') {
                    expect(screen.getByText('Editing profile: Beta')).toBeInTheDocument();
                    expect(screen.queryByLabelText('Profile Name')).not.toBeInTheDocument();
                } else {
                    expect(screen.getByLabelText('Profile Name')).toHaveValue('Beta copy');
                }
            } finally {
                unmount();
            }
        }
    );

    it.each([['Edit', false], ['Copy', true]])(
        'ignores an obsolete %s error after starting a new draft',
        async (action, rejected) => {
            const load = pendingResponse(false);
            const saved = savedProfile('Daily Drive');
            installProfiles([saved], [
                { method: 'GET', match: '/api/v1/profile?name=Daily%20Drive', respond: load.response }
            ]);
            const { unmount } = render(Page);
            try {
                await chooseProfile(saved.name, action);
                await waitFor(() => expect(load.readBody).toHaveBeenCalledOnce());
                await fireEvent.click(screen.getByRole('button', { name: 'New Profile', exact: true }));
                const draftMessage = screen.getByText('Choose your settings, then save this profile. Apply it to your V1 or assign it to Auto-Push.');
                await load.finish('Profile unavailable', rejected);
                expect(draftMessage).toBeInTheDocument();
                expect(screen.queryByText(/Failed to load:|Connection error/)).not.toBeInTheDocument();
            } finally {
                unmount();
            }
        }
    );

    it('allows a later Edit to complete after an earlier save closes its editor', async () => {
        const save = pendingResponse();
        const load = pendingResponse();
        const alpha = savedProfile('Alpha');
        const beta = savedProfile('Beta');
        installProfiles([alpha, beta], [
            { method: 'GET', match: '/api/v1/profile?name=Beta', respond: load.response },
            { method: 'POST', match: '/api/v1/profile', respond: () => save.readBody() }
        ]);
        const { unmount } = render(Page);
        try {
            await chooseProfile('Alpha', 'Edit');
            await screen.findByText('Editing profile: Alpha');
            await fireEvent.click(screen.getByRole('button', { name: 'Save Profile', exact: true }));
            await waitFor(() => expect(save.readBody).toHaveBeenCalledOnce());
            await fireEvent.click(screen.getByText(/Browse saved profiles/));
            await chooseProfile('Beta', 'Edit');
            await waitFor(() => expect(load.readBody).toHaveBeenCalledOnce());
            await save.finish({ ok: true });
            await screen.findByText('Profile "Alpha" saved');
            await load.finish(beta);
            expect(screen.getByText('Editing profile: Beta')).toBeInTheDocument();
            expect(screen.getByLabelText('Description')).toHaveValue('Beta description');
        } finally {
            unmount();
        }
    });
});
