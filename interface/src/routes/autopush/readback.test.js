import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { beforeEach, describe, expect, it } from 'vitest';

import { installFixtureFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import fixtures from '../../test/fixtures/wifi-api.json';
import Page from './+page.svelte';

const slotsUrl = '/api/autopush/slots';
const saveUrl = '/api/autopush/slot';

function baseline() {
    const slots = structuredClone(fixtures.scenarios.autopush_routes['GET /api/autopush/slots'][0].body);
    slots.schemaVersion = 4;
    slots.slots[0].color = 0x400a;
    return slots;
}

function saveToDevice(stored, fields) {
    Object.assign(stored.slots[Number(fields.slot)], {
        name: fields.name, color: Number(fields.color), profile: fields.profile,
        alertPersist: Number(fields.alertPersist), priorityArrowOnly: fields.priorityArrowOnly === 'true',
        volumeConfigured: fields.volumeConfigured === 'true',
        volume: Number(fields.volume ?? 5), muteVolume: Number(fields.muteVol ?? 0),
        darkModeConfigured: fields.darkModeConfigured === 'true', darkMode: fields.darkMode === 'true'
    });
}

function installDevice(routes) {
    return installFixtureFetchMock(['frontend_core_routes', 'autopush_routes', 'v1_profile_routes'], routes);
}

describe('slot saved-state and editor authority', () => {
    beforeEach(() => {
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

    for (const reply of ['accepted', 'lost']) {
        it.each(['healthy', 'http', 'network', 'delayed'])(
            `${reply} reply and %s readback cannot revert persistence on a later name-only save`,
            async (readback) => {
                const stored = baseline();
                const posts = [];
                let reads = 0;
                let releaseRead;
                const pendingRead = new Promise((resolve) => { releaseRead = resolve; });
                const fetchMock = installDevice([
                    { method: 'GET', match: slotsUrl, respond: () => {
                        reads += 1;
                        if (reads === 2) {
                            if (readback === 'http') return jsonResponse({}, 503);
                            if (readback === 'network') throw new Error('read lost');
                            if (readback === 'delayed') return pendingRead;
                        }
                        return jsonResponse(stored);
                    } },
                    { method: 'POST', match: saveUrl, respond: ({ init }) => {
                        const fields = Object.fromEntries(init.body);
                        posts.push(fields);
                        saveToDevice(stored, fields);
                        if (posts.length === 1 && reply === 'lost') throw new Error('reply lost after commit');
                        return jsonResponse({ success: true });
                    } }
                ]);
                const { unmount } = render(Page);
                try {
                    await screen.findByText('Highway');
                    const firstCard = screen.getByRole('heading', { name: 'Default' }).closest('.surface-card');
                    await fireEvent.click(within(firstCard).getByRole('button', { name: 'Edit' }));
                    await fireEvent.input(screen.getByLabelText('Alert persistence value (seconds)'), { target: { value: '5' } });
                    await fireEvent.click(screen.getByRole('button', { name: 'Save slot' }));
                    await waitFor(() => expect(reads).toBe(2));
                    expect(stored.slots[0].alertPersist).toBe(5);

                    if (readback !== 'healthy') {
                        if (readback !== 'delayed') await screen.findByText('Failed to load slots');
                        const save = screen.getByRole('button', { name: 'Save slot' });
                        expect(save).toBeDisabled();
                        await act(() => save.click());
                        expect(posts).toHaveLength(1);
                        await fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
                        // An ACK cannot reopen an editor from the superseded list.
                        for (const button of screen.getAllByRole('button', { name: 'Edit' })) expect(button).toBeDisabled();
                        for (const button of screen.getAllByRole('button', { name: 'Set as default' })) expect(button).toBeDisabled();
                        for (const button of screen.getAllByRole('button', { name: 'Apply to V1' })) expect(button).toBeDisabled();
                        if (readback === 'delayed') {
                            expect(screen.getByRole('button', { name: 'Reload saved slots' })).toBeDisabled();
                            await act(() => releaseRead(jsonResponse(stored)));
                        } else {
                            await fireEvent.click(screen.getByRole('button', { name: 'Reload saved slots' }));
                        }
                    } else if (reply === 'lost') {
                        await screen.findByText('Save response was lost. Saved slots reloaded.');
                        await fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
                    }
                    await waitFor(() => expect(within(firstCard).getByRole('button', { name: 'Edit' })).toBeEnabled());
                    expect(posts).toHaveLength(1);
                    await fireEvent.click(within(firstCard).getByRole('button', { name: 'Edit' }));
                    expect(screen.getByLabelText('Alert persistence value (seconds)')).toHaveValue(5);
                    await fireEvent.input(screen.getByLabelText('Slot name'), { target: { value: 'Renamed' } });
                    await fireEvent.click(screen.getByRole('button', { name: 'Save slot' }));
                    await waitFor(() => expect(posts).toHaveLength(2));
                    expect(posts[1]).toMatchObject({ slot: '0', name: 'RENAMED', alertPersist: '5', profile: 'Road Trip' });
                    expect(stored.slots[0].alertPersist).toBe(5);
                    expect(fetchMock.mock.calls.filter(([url, init]) => init?.method === 'POST' && url !== saveUrl)).toHaveLength(0);
                } finally { unmount(); }
            }
        );
    }

    it.each([0, 1])('rebases untouched fields and keeps later edits in slot %s opened before an earlier ACK', async (nextSlot) => {
        const stored = baseline();
        Object.assign(stored.slots[0], {
            volumeConfigured: true, volume: 6, muteVolume: 2,
            darkModeConfigured: true, darkMode: true, priorityArrowOnly: true
        });
        const posts = [];
        let finishSave;
        const pendingSave = new Promise((resolve) => { finishSave = resolve; });
        installDevice([
            { method: 'GET', match: slotsUrl, respond: () => jsonResponse(stored) },
            { method: 'GET', match: '/api/v1/profile?name=', respond: jsonResponse({ detector: { volume: { policy: 'temporary' } } }) },
            { method: 'POST', match: saveUrl, respond: ({ init }) => {
                const fields = Object.fromEntries(init.body);
                posts.push(fields);
                saveToDevice(stored, fields);
                return posts.length === 1 ? pendingSave : jsonResponse({ success: true });
            } }
        ]);
        const { unmount } = render(Page);
        try {
            await screen.findByText('Highway');
            const firstCard = screen.getByRole('heading', { name: 'Default' }).closest('.surface-card');
            const nextCard = screen.getByRole('heading', { name: nextSlot === 0 ? 'Default' : 'Highway' }).closest('.surface-card');
            await fireEvent.click(within(firstCard).getByRole('button', { name: 'Edit' }));
            await screen.findByText(/assigned profile’s Temporary policy/);
            await fireEvent.input(screen.getByLabelText('Alert persistence value (seconds)'), { target: { value: '5' } });
            await fireEvent.click(screen.getByRole('button', { name: 'Save slot' }));
            await waitFor(() => expect(posts).toHaveLength(1));
            await fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
            await fireEvent.click(within(nextCard).getByRole('button', { name: 'Edit' }));
            await fireEvent.input(screen.getByLabelText('Slot name'), { target: { value: 'Later draft' } });
            await act(() => finishSave(jsonResponse({ success: true })));
            await waitFor(() => expect(screen.getByRole('button', { name: 'Save slot' })).toBeEnabled());
            expect(screen.getByLabelText('Slot name')).toHaveValue('Later draft');
            expect(screen.getByLabelText('Alert persistence value (seconds)')).toHaveValue(nextSlot === 0 ? 5 : 2);
            if (nextSlot === 0) await screen.findByText(/assigned profile’s Temporary policy/);
            await fireEvent.click(screen.getByRole('button', { name: 'Save slot' }));
            await waitFor(() => expect(posts).toHaveLength(2));
            expect(posts[1]).toMatchObject({ slot: String(nextSlot), name: 'LATER DRAFT', alertPersist: nextSlot === 0 ? '5' : '2' });
            if (nextSlot === 0) expect(posts[1]).toMatchObject({
                volumeConfigured: 'true', volume: '6', muteVol: '2',
                darkModeConfigured: 'true', darkMode: 'true', priorityArrowOnly: 'true', profile: 'Road Trip'
            });
            else expect(posts[1].profile).toBe('Quiet Commute');
            expect(stored.slots[0].alertPersist).toBe(5);
        } finally { unmount(); }
    });

    it('keeps edits made after submission through failed readback and a read-only retry', async () => {
        const stored = baseline();
        const posts = [];
        let reads = 0;
        let finishSave;
        const pendingSave = new Promise((resolve) => { finishSave = resolve; });
        installDevice([
            { method: 'GET', match: slotsUrl, respond: () => ++reads === 2 ? jsonResponse({}, 503) : jsonResponse(stored) },
            { method: 'POST', match: saveUrl, respond: ({ init }) => {
                const fields = Object.fromEntries(init.body);
                posts.push(fields);
                saveToDevice(stored, fields);
                return posts.length === 1 ? pendingSave : jsonResponse({ success: true });
            } }
        ]);
        const { unmount } = render(Page);
        try {
            await screen.findByText('Highway');
            await fireEvent.click(screen.getAllByRole('button', { name: 'Edit' })[0]);
            await fireEvent.input(screen.getByLabelText('Alert persistence value (seconds)'), { target: { value: '5' } });
            await fireEvent.click(screen.getByRole('button', { name: 'Save slot' }));
            await fireEvent.input(screen.getByLabelText('Slot name'), { target: { value: 'Later draft' } });
            await fireEvent.input(screen.getByLabelText('Alert persistence value (seconds)'), { target: { value: '3' } });
            await act(() => finishSave(jsonResponse({ success: true })));
            await screen.findByText('Failed to load slots');
            await fireEvent.click(screen.getByRole('button', { name: 'Reload saved slots' }));
            await waitFor(() => expect(screen.getByRole('button', { name: 'Save slot' })).toBeEnabled());
            expect(posts).toHaveLength(1);
            expect(screen.getByLabelText('Slot name')).toHaveValue('Later draft');
            expect(screen.getByLabelText('Alert persistence value (seconds)')).toHaveValue(3);
            await fireEvent.click(screen.getByRole('button', { name: 'Save slot' }));
            await waitFor(() => expect(posts).toHaveLength(2));
            expect(posts[1]).toMatchObject({ name: 'LATER DRAFT', alertPersist: '3' });
        } finally { unmount(); }
    });

    it('reloads an uncertain default selection without repeating it', async () => {
        const stored = baseline();
        let reads = 0;
        const fetchMock = installDevice([
            { method: 'GET', match: slotsUrl, respond: () => ++reads === 2 ? jsonResponse({}, 503) : jsonResponse(stored) },
            { method: 'POST', match: '/api/autopush/activate', respond: () => {
                stored.activeSlot = 0;
                throw new Error('reply lost after commit');
            } }
        ]);
        const { unmount } = render(Page);
        try {
            await screen.findByText('Highway');
            const firstCard = screen.getByRole('heading', { name: 'Default' }).closest('.surface-card');
            await fireEvent.click(within(firstCard).getByRole('button', { name: 'Set as default' }));
            await screen.findByText('Failed to load slots');
            expect(within(firstCard).getByRole('button', { name: 'Set as default' })).toBeDisabled();
            await fireEvent.click(screen.getByRole('button', { name: 'Reload saved slots' }));
            await waitFor(() => expect(within(firstCard).getByText('Global default')).toBeInTheDocument());
            expect(fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST')).toHaveLength(1);
        } finally { unmount(); }
    });

    it('does not mistake an uncommitted lost save for accepted settings', async () => {
        const stored = baseline();
        const posts = [];
        installDevice([
            { method: 'GET', match: slotsUrl, respond: () => jsonResponse(stored) },
            { method: 'POST', match: saveUrl, respond: ({ init }) => {
                posts.push(Object.fromEntries(init.body));
                if (posts.length === 1) throw new Error('request lost before commit');
                saveToDevice(stored, posts[1]);
                return jsonResponse({ success: true });
            } }
        ]);
        const { unmount } = render(Page);
        try {
            await screen.findByText('Highway');
            await fireEvent.click(screen.getAllByRole('button', { name: 'Edit' })[0]);
            await fireEvent.input(screen.getByLabelText('Alert persistence value (seconds)'), { target: { value: '5' } });
            await fireEvent.click(screen.getByRole('button', { name: 'Save slot' }));
            await screen.findByText('Save response was lost. Saved slots reloaded.');
            expect(posts).toHaveLength(1);
            expect(screen.getByLabelText('Alert persistence value (seconds)')).toHaveValue(1);
            await fireEvent.input(screen.getByLabelText('Slot name'), { target: { value: 'Renamed' } });
            await fireEvent.click(screen.getByRole('button', { name: 'Save slot' }));
            await waitFor(() => expect(posts).toHaveLength(2));
            expect(posts[1]).toMatchObject({ name: 'RENAMED', alertPersist: '1' });
        } finally { unmount(); }
    });

    it('does not continue save readback after the editor has unmounted', async () => {
        let rejectWrite;
        const pendingWrite = new Promise((_, reject) => { rejectWrite = reject; });
        const fetchMock = installDevice([
            { method: 'GET', match: slotsUrl, respond: jsonResponse(baseline()) },
            { method: 'POST', match: saveUrl, respond: () => pendingWrite }
        ]);
        const { unmount } = render(Page);
        await screen.findByText('Highway');
        await fireEvent.click(screen.getAllByRole('button', { name: 'Edit' })[0]);
        await fireEvent.click(screen.getByRole('button', { name: 'Save slot' }));
        unmount();
        await act(() => rejectWrite(new Error('reply lost')));
        expect(fetchMock.mock.calls.filter(([url]) => url === slotsUrl)).toHaveLength(1);
        expect(fetchMock.mock.calls.filter(([url]) => url === saveUrl)).toHaveLength(1);
    });
});
