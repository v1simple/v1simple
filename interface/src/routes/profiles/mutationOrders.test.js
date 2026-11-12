import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import Page from './+page.svelte';
import { createDefaultInTheBoxSettings } from '#lib/features/profiles/inTheBoxSettings.js';

function json(data, status = 200) {
    return new Response(JSON.stringify(data), { status, headers: { 'Content-Type': 'application/json' } });
}
function deferred() {
    let resolve;
    let reject;
    const promise = new Promise((done, fail) => { resolve = done; reject = fail; });
    return { promise, resolve, reject };
}
function installCatalog({ commitDelete = true, readCatalog = (data) => json(data) } = {}) {
    let durable = {
        name: 'Daily', description: 'original', schemaVersion: 4,
        settings: { xBand: true, kBand: true, kaBand: true, laser: true },
        detector: { userSettings: 'value', mode: { policy: 'unchanged' }, display: 'unchanged',
            volume: { policy: 'unchanged' }, customFrequencies: { policy: 'unchanged' } },
        inTheBox: createDefaultInTheBoxSettings()
    };
    const pending = [];
    let catalogReads = 0;
    vi.stubGlobal('fetch', vi.fn(async (url, init = {}) => {
        if (url === '/api/v1/profiles') {
            return readCatalog({ schemaVersion: 4, profiles: durable ? [durable] : [] }, ++catalogReads);
        }
        if (url === '/api/v1/snapshot') return json({ available: true, address: 'AA:BB:CC:DD:EE:FF',
            settings: { bytes: [255,255,255,255,255,255] }, capabilities: { versionKnown: true, gen2: true },
            observations: { userBytes: { available: true, value: [255,255,255,255,255,255] } } });
        if (url === '/api/v1/profile?name=Daily') return json(durable);
        if (init.method === 'POST' && ['/api/v1/profile', '/api/v1/profile/delete'].includes(url)) {
            const response = deferred();
            const payload = JSON.parse(init.body);
            // Persistence order is independent of response arrival, as on the device.
            if (!url.endsWith('/delete')) durable = payload;
            else if (commitDelete) durable = null;
            pending.push({ url, payload, ...response });
            return response.promise;
        }
        throw new Error(`Unexpected ${init.method || 'GET'} ${url}`);
    }));
    return { pending, durable: () => durable };
}
function browseDelete(name = 'Daily') {
    document.querySelector('.surface-card details').open = true;
    return within(screen.getByText(name, { selector: '.font-medium' }).closest('.surface-panel'))
        .getByRole('button', { name: 'Delete' });
}
async function editDaily() {
    render(Page);
    await fireEvent.click(within((await screen.findByText('Daily')).closest('.surface-panel'))
        .getByRole('button', { name: 'Edit' }));
    await screen.findByText('Editing profile: Daily');
    await fireEvent.input(screen.getByLabelText('Description'), { target: { value: 'updated' } });
}
async function saveControl(path, name = 'Daily') {
    if (path === 'editor') return screen.getByRole('button', { name: 'Save Profile' });
    await fireEvent.click(screen.getByRole('button', { name: 'New Profile' }));
    await fireEvent.click(screen.getByRole('button', { name: 'Save as Profile' }));
    await fireEvent.input(screen.getByLabelText('Profile Name'), { target: { value: name } });
    return within(screen.getByText('Save Profile').closest('.modal-box')).getByRole('button', { name: 'Save' });
}

beforeEach(() => {
    vi.stubGlobal('confirm', vi.fn(() => true));
    const storage = new Map();
    Object.defineProperty(window, 'localStorage', { configurable: true, value: {
        getItem: (key) => storage.get(key) ?? null,
        setItem: (key, value) => storage.set(key, String(value)),
        removeItem: (key) => storage.delete(key)
    } });
});
afterEach(() => { vi.restoreAllMocks(); vi.unstubAllGlobals(); });

describe('profile mutation ordering', () => {
    it.each(['editor', 'dialog'])('holds Delete until the %s Save response and preserves later edits', async (path) => {
        const state = installCatalog();
        await editDaily();
        const savedName = path === 'dialog' ? 'New Daily' : 'Daily';
        await fireEvent.click(await saveControl(path, savedName));
        await waitFor(() => expect(state.pending).toHaveLength(1));
        if (path === 'dialog') {
            await fireEvent.click(within(document.querySelector('.modal-box')).getByRole('button', { name: 'Cancel' }));
        }
        const deleting = browseDelete();
        expect(deleting).toBeDisabled();
        await fireEvent.click(deleting);
        expect(state.pending).toHaveLength(1);
        await fireEvent.input(screen.getByLabelText('Description'), { target: { value: 'later unsent edit' } });
        state.pending[0].resolve(json({ success: true }));
        await screen.findByText(`Profile "${savedName}" saved`);
        expect(screen.getByLabelText('Description')).toHaveValue('later unsent edit');
        expect(browseDelete(savedName)).toBeEnabled();
        await fireEvent.click(browseDelete(savedName));
        await waitFor(() => expect(state.pending).toHaveLength(2));
        state.pending[1].resolve(json({ success: true }));
        await screen.findByText('Profile deleted');
        expect(state.durable()).toBeNull();
        expect(screen.queryByText(savedName, { selector: '.font-medium' })).not.toBeInTheDocument();
        expect(screen.queryByRole('button', { name: 'Apply saved profile to V1' })).not.toBeInTheDocument();
    });

    it.each(['editor', 'dialog'])('holds %s Save until Delete completes, then allows intentional recreation', async (path) => {
        const state = installCatalog();
        await editDaily();
        await fireEvent.click(browseDelete());
        await waitFor(() => expect(state.pending).toHaveLength(1));
        const save = await saveControl(path);
        expect(save).toBeDisabled();
        await fireEvent.click(save);
        expect(state.pending).toHaveLength(1);
        state.pending[0].resolve(json({ success: true }));
        await screen.findByText('Profile deleted');
        expect(save).toBeEnabled();
        await fireEvent.click(save);
        await waitFor(() => expect(state.pending).toHaveLength(2));
        state.pending[1].resolve(json({ success: true }));
        await screen.findByText('Profile "Daily" saved');
        expect(state.durable().name).toBe('Daily');
        expect(screen.getByText('Daily', { selector: '.font-medium' })).toBeInTheDocument();
        expect(screen.getByRole('button', { name: 'Apply saved profile to V1' })).toBeEnabled();
    });

    it('blocks a second Delete while its first response is pending', async () => {
        const state = installCatalog();
        await editDaily();
        const button = browseDelete();
        await fireEvent.click(button);
        await waitFor(() => expect(state.pending).toHaveLength(1));
        expect(button).toBeDisabled();
        await fireEvent.click(button);
        expect(state.pending).toHaveLength(1);
        state.pending[0].resolve(json({ success: true }));
        await screen.findByText('Profile deleted');
    });

    it.each(['save', 'delete'])('releases mutation controls after a failed %s response', async (operation) => {
        const state = installCatalog({ commitDelete: false });
        await editDaily();
        await fireEvent.click(operation === 'save' ? await saveControl('editor') : browseDelete());
        await waitFor(() => expect(state.pending).toHaveLength(1));
        state.pending[0].resolve(json({ error: 'Storage unavailable' }, 503));
        await screen.findByText(/Failed to (save|delete)/);
        expect(screen.getByRole('button', { name: 'Save Profile' })).toBeEnabled();
        expect(browseDelete()).toBeEnabled();
        expect(screen.getByLabelText('Description')).toHaveValue('updated');
    });
    it.each(['save', 'delete'])('releases mutation controls after a %s connection failure', async (operation) => {
        const state = installCatalog({ commitDelete: false });
        await editDaily();
        await fireEvent.click(operation === 'save' ? await saveControl('editor') : browseDelete());
        await waitFor(() => expect(state.pending).toHaveLength(1));
        state.pending[0].reject(new Error('Connection lost'));
        await screen.findByText(operation === 'save' ? 'Connection error' : 'Failed to delete');
        expect(screen.getByRole('button', { name: 'Save Profile' })).toBeEnabled();
        expect(browseDelete()).toBeEnabled();
        expect(screen.getByLabelText('Description')).toHaveValue('updated');
    });

    it.each(['lost response', '404'])('reconciles a committed Delete after a %s without repeating it', async (outcome) => {
        const readback = deferred();
        const state = installCatalog({
            readCatalog: (data, count) => count === 2 ? readback.promise : json(data)
        });
        await editDaily();
        await fireEvent.click(browseDelete());
        await waitFor(() => expect(state.pending).toHaveLength(1));
        expect(state.durable()).toBeNull();
        const apply = screen.getByRole('button', { name: 'Apply saved profile to V1' });
        expect(apply).toBeDisabled();
        if (outcome === '404') state.pending[0].resolve(json({ error: 'Profile not found' }, 404));
        else state.pending[0].reject(new Error('Delete reply lost after commit'));
        await screen.findByText('Awaiting readback');
        expect(browseDelete()).toBeDisabled();
        expect(apply).toBeDisabled();
        expect(screen.getByRole('button', { name: 'Save Profile' })).toBeDisabled();
        readback.resolve(json({ schemaVersion: 4, profiles: [] }));
        await screen.findByText('Profile "Daily" is no longer saved');
        expect(screen.queryByText('Daily', { selector: '.font-medium' })).not.toBeInTheDocument();
        expect(screen.queryByRole('button', { name: 'Apply saved profile to V1' })).not.toBeInTheDocument();
        expect(screen.getByLabelText('Description')).toHaveValue('updated');
        expect(screen.getByRole('button', { name: 'Save Profile' })).toBeEnabled();
        expect(state.pending).toHaveLength(1);
        expect(fetch.mock.calls.filter(([url]) => url === '/api/v1/profiles')).toHaveLength(2);
    });

    it('keeps an unresolved Delete unavailable and retries only the catalog read', async () => {
        const state = installCatalog({
            readCatalog: (data, count) => count === 2 ? json({}, 503) : json(data)
        });
        await editDaily();
        await fireEvent.click(browseDelete());
        await waitFor(() => expect(state.pending).toHaveLength(1));
        state.pending[0].reject(new Error('Delete reply lost after commit'));
        const retry = await screen.findByRole('button', { name: 'Reload saved profiles' });
        expect(screen.getByRole('button', { name: 'Apply saved profile to V1' })).toBeDisabled();
        expect(browseDelete()).toBeDisabled();
        expect(screen.getByRole('button', { name: 'Save Profile' })).toBeDisabled();
        await fireEvent.input(screen.getByLabelText('Description'), { target: { value: 'newer draft' } });
        await fireEvent.click(retry);
        await screen.findByText('Profile "Daily" is no longer saved');
        expect(screen.queryByText('Daily', { selector: '.font-medium' })).not.toBeInTheDocument();
        expect(screen.getByLabelText('Description')).toHaveValue('newer draft');
        expect(state.pending).toHaveLength(1);
        expect(fetch.mock.calls.filter(([url]) => url === '/api/v1/profiles')).toHaveLength(3);
    });

});
