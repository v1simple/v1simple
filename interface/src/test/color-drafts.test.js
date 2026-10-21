import { act, cleanup, fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import Colors from '../lib/features/colors/ColorsPage.svelte';
import AutoPush from '../routes/autopush/+page.svelte';
import ColorControl from '../lib/components/ColorControl.svelte';
import { cloneDefaultColors } from '../lib/utils/colors.js';
import { installFixtureFetchMock, jsonResponse } from './fetch-mock.js';
import fixtures from './fixtures/wifi-api.json';

function deferred() {
    let resolve;
    const promise = new Promise((finish) => { resolve = finish; });
    return { promise, resolve };
}

async function finish(request, response) {
    await act(async () => { request.resolve(response); await request.promise; });
    await act();
}

function hexInput(swatchName) {
    return screen.getByRole('button', { name: swatchName }).parentElement.querySelector('input');
}

beforeEach(() => {
    vi.stubGlobal('confirm', vi.fn(() => true));
    const storage = new Map();
    Object.defineProperty(window, 'localStorage', {
        configurable: true,
        value: {
            getItem: (key) => storage.get(key) ?? null,
            setItem: (key, value) => storage.set(key, String(value)),
            removeItem: (key) => storage.delete(key)
        }
    });
});

afterEach(() => {
    cleanup();
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
});

describe('raw color drafts during canonical readback', () => {
    it.each([
        ['save', '001F'], ['reset', '001F'], ['save', '#00'], ['reset', '#00']
    ])('preserves Display %s draft %s through a delayed GET and saves it only on a later submission', async (action, rawDraft) => {
        const post = deferred();
        const readback = deferred();
        const posts = [];
        let reads = 0;
        let canonical = { ...cloneDefaultColors(), bar1: 0xf800, brightness: 99 };
        installFixtureFetchMock(['display_colors_success', 'quiet_settings_success'], [
            {
                method: 'GET', match: '/api/display/settings', respond: () => {
                    reads += 1;
                    if (reads === 1) return jsonResponse(cloneDefaultColors());
                    return reads === 2 ? readback.promise : jsonResponse(canonical);
                }
            },
            {
                method: 'POST',
                match: (url) => ['/api/display/settings', '/api/display/settings/reset'].includes(url),
                respond: ({ init }) => {
                    posts.push(init.body);
                    if (posts.length === 1) return post.promise;
                    canonical = { ...canonical, bar1: Number(init.body.get('bar1')) };
                    return jsonResponse({ success: true });
                }
            }
        ]);
        render(Colors);
        await screen.findByRole('slider', { name: 'Screen brightness' });
        await fireEvent.click(screen.getByRole('button', { name: 'Alert colors', exact: true }));
        const input = hexInput('Signal bar segment 1 color');
        await fireEvent.click(screen.getByRole('button', {
            name: action === 'save' ? 'Save display settings' : 'Reset Defaults'
        }));
        expect(input).toBeEnabled();
        input.focus();
        await fireEvent.input(input, { target: { value: rawDraft } });
        await finish(post, jsonResponse({ success: true }));
        await waitFor(() => expect(reads).toBe(2));
        expect(input).toBeInTheDocument();
        expect(input).toHaveFocus();
        expect(input).toHaveValue(rawDraft);
        await finish(readback, jsonResponse(canonical));
        await screen.findByText(action === 'save' ? 'Colors saved! Previewing on display...' : 'Colors reset to defaults!');
        expect(hexInput('Signal bar segment 1 color')).toBe(input);
        expect(input).toHaveFocus();
        expect(input).toHaveValue(rawDraft);
        expect(posts).toHaveLength(1);
        if (action === 'save') expect(posts[0].get('bar1')).toBe('2016');
        expect(screen.getByRole('slider', { name: 'Screen brightness', hidden: true })).toHaveValue('99');

        await fireEvent.input(input, { target: { value: '#0000FF' } });
        expect(input).toHaveValue('#0000FF');
        await fireEvent.change(input);
        await fireEvent.click(screen.getByRole('button', { name: 'Save display settings' }));
        await waitFor(() => expect(posts).toHaveLength(2));
        expect(posts[1].get('bar1')).toBe('31');
    });

    it.each(['001F', '#00'])('preserves Auto-Push raw draft %s after a slot save and readback', async (rawDraft) => {
        const post = deferred();
        const readback = deferred();
        const stored = structuredClone(fixtures.scenarios.autopush_routes['GET /api/autopush/slots'][0].body);
        stored.schemaVersion = 4;
        const posts = [];
        let reads = 0;
        installFixtureFetchMock(['frontend_core_routes', 'autopush_routes', 'v1_profile_routes'], [
            {
                method: 'GET', match: '/api/autopush/slots', respond: () => {
                    reads += 1;
                    return reads === 2 ? readback.promise : jsonResponse(stored);
                }
            },
            {
                method: 'POST', match: '/api/autopush/slot', respond: ({ init }) => {
                    posts.push(init.body);
                    stored.slots[0].color = Number(init.body.get('color'));
                    return posts.length === 1 ? post.promise : jsonResponse({ success: true });
                }
            }
        ]);
        render(AutoPush);
        await screen.findByText('Highway');
        await fireEvent.click(screen.getAllByRole('button', { name: 'Edit' })[0]);
        await fireEvent.input(screen.getByLabelText('Slot name'), { target: { value: 'Submitted' } });
        const input = hexInput('Choose Submitted color');
        const submittedColor = parseInt(input.value, 16);
        await fireEvent.click(screen.getByRole('button', { name: /^save slot$/i }));
        expect(input).toBeEnabled();
        input.focus();
        await fireEvent.input(input, { target: { value: rawDraft } });
        await finish(post, jsonResponse({ success: true }));
        await waitFor(() => expect(reads).toBe(2));
        await finish(readback, jsonResponse(stored));
        await waitFor(() => expect(screen.getByRole('button', { name: /^save slot$/i })).toBeEnabled());
        expect(hexInput('Choose Submitted color')).toBe(input);
        expect(input).toHaveFocus();
        expect(input).toHaveValue(rawDraft);
        expect(posts).toHaveLength(1);
        expect(Number(posts[0].get('color'))).toBe(submittedColor);

        await fireEvent.input(input, { target: { value: '#0000FF' } });
        expect(input).toHaveValue('#0000FF');
        await fireEvent.change(input);
        await fireEvent.click(screen.getByRole('button', { name: /^save slot$/i }));
        await waitFor(() => expect(posts).toHaveLength(2));
        expect(posts[1].get('color')).toBe('31');
        await waitFor(() => expect(screen.queryByLabelText('Slot name')).not.toBeInTheDocument());
    });
});

it('keeps partial raw color text across external value changes and commits only on change', async () => {
    const onHexChange = vi.fn();
    const onHexInput = vi.fn();
    const { rerender } = render(ColorControl, {
        id: 'test-color', label: 'Test color', value: 0x07e0, onHexChange, onHexInput
    });
    const input = hexInput('Test color');
    for (const raw of ['#', '#0', '#00', '#0000', '#0000FF']) {
        await fireEvent.input(input, { target: { value: raw } });
        expect(input).toHaveValue(raw);
    }
    expect(onHexChange).not.toHaveBeenCalled();
    expect(onHexInput).toHaveBeenCalledTimes(5);
    await rerender({ value: 0xf800 });
    expect(input).toHaveValue('#0000FF');
    await fireEvent.change(input);
    expect(onHexChange).toHaveBeenCalledExactlyOnceWith('#0000FF');
    await rerender({ value: 0x001f });
    expect(input).toHaveValue('001F');
});
