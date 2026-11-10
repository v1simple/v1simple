import { act, fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';

import { cloneDefaultColors, COLOR_PARAM_ORDER } from '#lib/utils/colors.js';
import { installFixtureFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

const settingsUrl = '/api/display/settings';
const resetUrl = '/api/display/settings/reset';

describe('display settings write/readback authority', () => {
    beforeEach(() => { global.confirm = vi.fn(() => true); });

    for (const operation of ['reset', 'save']) {
        for (const reply of ['accepted', 'lost']) {
            it.each(['healthy', 'http', 'network', 'delayed'])(
                `${operation}, ${reply} reply, %s readback: keeps current saved colors and later edits`,
                async (readback) => {
                    let stored = { ...cloneDefaultColors(), bogey: 31, brightness: 200 };
                    const posts = [];
                    let reads = 0;
                    let releaseRead;
                    const pendingRead = new Promise((resolve) => { releaseRead = resolve; });
                    const fetchMock = installFixtureFetchMock(['quiet_settings_success'], [
                        { method: 'GET', match: settingsUrl, respond: () => {
                            reads += 1;
                            if (reads === 2) {
                                if (readback === 'http') return jsonResponse({}, 503);
                                if (readback === 'network') throw new Error('read lost');
                                if (readback === 'delayed') return pendingRead;
                            }
                            return jsonResponse(stored);
                        } },
                        { method: 'POST', match: (url) => [settingsUrl, resetUrl].includes(url), respond: ({ url, init }) => {
                            posts.push({ url, fields: Object.fromEntries(init.body) });
                            if (url === resetUrl) stored = { ...cloneDefaultColors(), brightness: stored.brightness };
                            else stored = Object.fromEntries([...init.body].map(([key, value]) => [
                                key, value === 'true' ? true : value === 'false' ? false : Number(value)
                            ]));
                            if (posts.length === 1 && reply === 'lost') throw new Error('reply lost after commit');
                            return jsonResponse({ success: true });
                        } }
                    ]);
                    const { unmount } = render(Page);
                    try {
                        const save = await screen.findByRole('button', { name: 'Save display settings' });
                        if (operation === 'save') {
                            await fireEvent.click(screen.getByRole('button', { name: 'Alert colors' }));
                            const bogey = screen.getByRole('button', { name: 'Bogey counter color' })
                                .closest('.field-control').querySelector('input[type="text"]');
                            await fireEvent.change(bogey, { target: { value: '07E0' } });
                            await fireEvent.click(screen.getByRole('button', { name: 'Screen' }));
                        }
                        await fireEvent.click(operation === 'save' ? save : screen.getByRole('button', { name: 'Reset Defaults' }));
                        await waitFor(() => expect(reads).toBe(2));
                        const expectedBogey = operation === 'reset' ? cloneDefaultColors().bogey : 2016;
                        expect(stored.bogey).toBe(expectedBogey);
                        expect(stored.brightness).toBe(200); // Reset leaves brightness alone.

                        if (readback !== 'healthy') {
                            if (readback !== 'delayed') await screen.findByText('Failed to load colors');
                            expect(save).toBeDisabled();
                            expect(screen.getByRole('button', { name: 'Reset Defaults' })).toBeDisabled();
                            expect(screen.getByRole('button', { name: 'Preview' })).toBeDisabled();
                            await act(() => save.click());
                            expect(posts).toHaveLength(1);
                            // Editing remains intentional draft input, never an automatic retry.
                            await fireEvent.input(screen.getByRole('slider', { name: 'Screen brightness' }), { target: { value: '100' } });
                            if (readback === 'delayed') {
                                expect(screen.getByRole('button', { name: 'Reload saved display settings' })).toBeDisabled();
                                await act(() => releaseRead(jsonResponse(stored)));
                            } else {
                                await fireEvent.click(screen.getByRole('button', { name: 'Reload saved display settings' }));
                            }
                            await waitFor(() => expect(save).toBeEnabled());
                            expect(screen.getByRole('slider', { name: 'Screen brightness' })).toHaveValue('100');
                        } else {
                            await waitFor(() => expect(save).toBeEnabled());
                            await fireEvent.input(screen.getByRole('slider', { name: 'Screen brightness' }), { target: { value: '100' } });
                        }

                        expect(posts).toHaveLength(1); // Recovery only reads.
                        await fireEvent.click(save);
                        await waitFor(() => expect(posts).toHaveLength(2));
                        expect(posts[1].url).toBe(settingsUrl);
                        expect(Object.keys(posts[1].fields).sort()).toEqual([...COLOR_PARAM_ORDER].sort());
                        expect(posts[1].fields.bogey).toBe(String(expectedBogey));
                        expect(posts[1].fields.brightness).toBe('100');
                        expect(stored.bogey).toBe(expectedBogey);
                        expect(fetchMock.mock.calls.filter(([url, init]) =>
                            init?.method === 'POST' && ![settingsUrl, resetUrl].includes(url))).toHaveLength(0);
                    } finally { unmount(); }
                }
            );
        }
    }

    it('does not read or retry a lost reset response after navigation', async () => {
        let rejectWrite;
        const pendingWrite = new Promise((_, reject) => { rejectWrite = reject; });
        const fetchMock = installFixtureFetchMock(['display_colors_success', 'quiet_settings_success'], [
            { method: 'POST', match: resetUrl, respond: () => pendingWrite }
        ]);
        const { unmount } = render(Page);
        await fireEvent.click(await screen.findByRole('button', { name: 'Reset Defaults' }));
        unmount();
        await act(() => rejectWrite(new Error('response lost')));
        expect(fetchMock.mock.calls.filter(([url, init]) => url === settingsUrl && (!init?.method || init.method === 'GET'))).toHaveLength(1);
        expect(fetchMock.mock.calls.filter(([url]) => url === resetUrl)).toHaveLength(1);
    });

    it.each(['reset', 'save'])('reads actual settings when an uncertain %s never committed', async (operation) => {
        const stored = { ...cloneDefaultColors(), bogey: 31, brightness: 200 };
        const fetchMock = installFixtureFetchMock(['quiet_settings_success'], [
            { method: 'GET', match: settingsUrl, respond: jsonResponse(stored) },
            { method: 'POST', match: (url) => [settingsUrl, resetUrl].includes(url), respond: () => {
                throw new Error('request lost before commit');
            } }
        ]);
        const { unmount } = render(Page);
        try {
            const save = await screen.findByRole('button', { name: 'Save display settings' });
            if (operation === 'save') await fireEvent.input(screen.getByRole('slider', { name: 'Screen brightness' }), { target: { value: '100' } });
            await fireEvent.click(operation === 'save' ? save : screen.getByRole('button', { name: 'Reset Defaults' }));
            await screen.findByText(`${operation === 'save' ? 'Save' : 'Reset'} response was lost. Current display settings reloaded.`);
            expect(screen.getByRole('slider', { name: 'Screen brightness' })).toHaveValue('200');
            expect(save).toBeEnabled();
            expect(fetchMock.mock.calls.filter(([, init]) => init?.method === 'POST')).toHaveLength(1);
        } finally { unmount(); }
    });
});
