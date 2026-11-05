import { afterEach, describe, expect, it, vi } from 'vitest';
import { createProfileAuthoring } from './profileAuthoring.svelte.js';

const document = {
    schemaVersion: 4, name: 'Daily', description: 'Original',
    settings: { bytes: [255, 255, 255, 255, 255, 255], xBand: true },
    detector: { userSettings: 'value', display: 'off', mode: { policy: 'value', value: 2 },
        volume: { policy: 'temporary', main: 7, muted: 2, feedback: 'always', disconnect: 'keep_current' },
        customFrequencies: { policy: 'value', definitions: [{ index: 0, lowerMHz: 23910, upperMHz: 24250 }] } },
    inTheBox: { bands: {
        x: { muteOutside: true, unmuteInside: false },
        k: { muteOutside: false, unmuteInside: false },
        ka: { muteOutside: false, unmuteInside: false },
        ku: { muteOutside: false, unmuteInside: false }
    }, boxes: { x: { enabled: true, lowerMHz: 10501, upperMHz: 10549 } } }
};
function pendingSave() {
    let resolve;
    let reject;
    const request = new Promise((done, fail) => { resolve = done; reject = fail; });
    vi.stubGlobal('fetch', vi.fn(() => request));
    const onSaved = vi.fn();
    const onMessage = vi.fn();
    const owner = createProfileAuthoring({ onSaved, onMessage });
    owner.beginEdit(document);
    return { owner, resolve, reject, onSaved, onMessage };
}
function submit(state, creating) {
    if (creating) {
        state.owner.openNamingDialog();
        state.owner.namingDialog.name = 'Unique';
        return state.owner.saveNewName([]);
    }
    return state.owner.saveEdit();
}
afterEach(() => { vi.unstubAllGlobals(); });

describe('profile authoring submitted contents and completion policies', () => {
    // Literal outcomes characterized against the old page's actual preparation
    // and comparison expressions. The two policies are intentionally distinct.
    it.each([
        ['unchanged', () => {}, true, true],
        ['equal raw array replacement', (s) => { s.baseBytes = [...s.baseBytes]; }, true, false],
        ['in-place raw byte mutation', (s) => { s.baseBytes[0] = 0; }, false, true],
        ['visible setting mutation', (s) => { s.x = false; }, false, false],
        ['non-wire scalar mutation', (s) => { s.localMarker = 2; }, true, false]
    ])('preserves create/edit comparison for %s', async (_, mutate, createCloses, editCloses) => {
        for (const creating of [true, false]) {
            const state = pendingSave();
            state.owner.draft.settings.localMarker = 1;
            const promise = submit(state, creating);
            const transmitted = JSON.parse(fetch.mock.calls[0][1].body);
            expect(transmitted.settings.baseBytes).toEqual([255, 255, 255, 255, 255, 255]);
            expect(transmitted.createOnly).toBe(creating ? true : undefined);
            mutate(state.owner.draft.settings);
            state.resolve({ ok: true });
            await promise;
            expect(state.owner.draft === null).toBe(creating ? createCloses : editCloses);
            expect(state.owner.acceptedProfile.settings.baseBytes).toEqual([255, 255, 255, 255, 255, 255]);
            expect(state.onSaved).toHaveBeenCalledExactlyOnceWith(creating ? 'Unique' : 'Daily', 'Original', true);
        }
    });

    it.each(['new', 'captured', 'copy', 'edit', 'reedit', 'reset', 'cancel'])('publishes the catalog without replacing a later %s session', async (action) => {
        const state = pendingSave();
        const promise = submit(state, true);
        if (action === 'new') state.owner.beginNew();
        if (action === 'captured') state.owner.beginCaptured({ settings: { bytes: [1, 2, 3, 4, 5, 6] } });
        if (action === 'copy') state.owner.beginCopy({ ...document, name: 'Other' }, []);
        if (action === 'edit') state.owner.beginEdit({ ...document, name: 'Other' });
        if (action === 'reedit') state.owner.reedit();
        if (action === 'reset') state.owner.resetEditableDomains();
        if (action === 'cancel') state.owner.cancel();
        const live = { source: state.owner.acceptedProfile, draft: state.owner.draft, dialog: state.owner.namingDialog };
        state.resolve({ ok: true });
        await promise;
        expect(state.onSaved).toHaveBeenCalledExactlyOnceWith('Unique', 'Original', true);
        expect(state.owner.acceptedProfile).toBe(live.source);
        expect(state.owner.draft).toBe(live.draft);
        expect(state.owner.namingDialog).toBe(live.dialog);
    });

    it('does not close a reopened dialog with identical text after the old dialog saves', async () => {
        const state = pendingSave();
        const promise = submit(state, true);
        state.owner.closeNamingDialog();
        state.owner.openNamingDialog();
        state.owner.namingDialog.name = 'Unique';
        const dialog = state.owner.namingDialog;
        state.resolve({ ok: true });
        await promise;
        expect(state.owner.namingDialog).toBe(dialog);
        expect(state.owner.draft).not.toBeNull();
    });

    it('resets all editable domains without replacing source, description or naming values', () => {
        const { owner } = pendingSave();
        owner.openNamingDialog();
        owner.namingDialog.name = 'Unique';
        const source = owner.acceptedProfile;
        const dialog = owner.namingDialog;
        owner.resetEditableDomains();
        expect(owner.acceptedProfile).toBe(source);
        expect(owner.namingDialog).toBe(dialog);
        expect(owner.draft.description).toBe('Original');
        expect(owner.draft.settings).not.toHaveProperty('baseBytes');
        expect(owner.draft.settings.x).toBe(true);
        expect(owner.draft.detector.display).toBe('unchanged');
        expect(owner.draft.detector.customFrequencyDefinitions).toEqual([
            { index: 0, lowerMHz: 23910, upperMHz: 24250 }, { index: 1, lowerMHz: 33400, upperMHz: 36002 }
        ]);
        expect(Object.values(owner.draft.inTheBox.bands).every((band) => !band.muteOutside && !band.unmuteInside)).toBe(true);
    });

    it.each(['reopen', 'reset'])('advances an Edit baseline while preserving later %s changes in every domain', async (transition) => {
        const state = pendingSave();
        const { owner } = state;
        const source = owner.acceptedProfile;
        owner.draft.settings.x = false;
        owner.draft.detector.display = 'on';
        owner.draft.inTheBox.bands.k.muteOutside = true;
        owner.draft.description = 'Submitted description';
        const promise = owner.saveEdit();
        const transmitted = JSON.parse(fetch.mock.calls[0][1].body);
        if (transition === 'reopen') {
            owner.cancel();
            owner.reedit();
            expect(owner.draft.settings.x).toBe(false);
            expect(owner.draft.detector.display).toBe('on');
            expect(owner.draft.inTheBox.bands.k.muteOutside).toBe(true);
            expect(owner.draft.description).toBe('Submitted description');
            owner.draft.settings.k = true;
            owner.draft.settings.baseBytes[0] = 0;
            owner.draft.detector.customFrequencyDefinitions[0].lowerMHz = 23920;
            owner.draft.inTheBox.boxes.x.lowerMHz = 10502;
            owner.draft.description = 'Later unsent description';
        } else owner.resetEditableDomains();
        const draft = owner.draft;
        const later = { settings: JSON.stringify(draft.settings), detector: JSON.stringify(draft.detector),
            inTheBox: JSON.stringify(draft.inTheBox), description: draft.description };
        expect(owner.acceptedProfile).toBe(source);
        expect(owner.acceptedProfile.settings.x).toBe(true);
        state.resolve({ ok: true });
        await promise;
        expect(owner.draft).toBe(draft);
        expect(owner.draft.source).toBe(owner.acceptedProfile);
        expect(owner.acceptedProfile.settings.x).toBe(false);
        expect(owner.acceptedProfile.settings.baseBytes).toEqual(transmitted.settings.baseBytes);
        expect(owner.acceptedProfile.detector.display).toBe('on');
        expect(owner.acceptedProfile.detector.customFrequencyDefinitions[0].lowerMHz).toBe(23910);
        expect(owner.acceptedProfile.inTheBox.bands.k.muteOutside).toBe(true);
        expect(owner.acceptedProfile.inTheBox.boxes.x.lowerMHz).toBe(10501);
        expect(owner.acceptedProfile.description).toBe('Submitted description');
        expect({ settings: JSON.stringify(draft.settings), detector: JSON.stringify(draft.detector),
            inTheBox: JSON.stringify(draft.inTheBox), description: draft.description }).toEqual(later);
        expect(fetch).toHaveBeenCalledOnce();
    });

    it.each(['new', 'captured', 'copy', 'other profile'])('does not replace a later %s selection after an Edit ACK', async (selection) => {
        const state = pendingSave();
        state.owner.draft.settings.x = false;
        const promise = state.owner.saveEdit();
        if (selection === 'new') state.owner.beginNew();
        if (selection === 'captured') state.owner.beginCaptured({ settings: { bytes: [1, 2, 3, 4, 5, 6] } });
        if (selection === 'copy') state.owner.beginCopy(document, []);
        if (selection === 'other profile') state.owner.beginEdit({ ...document, name: 'Other' });
        const live = { source: state.owner.acceptedProfile, draft: state.owner.draft, dialog: state.owner.namingDialog };
        state.resolve({ ok: true });
        await promise;
        expect(state.owner.acceptedProfile).toBe(live.source);
        expect(state.owner.draft).toBe(live.draft);
        expect(state.owner.namingDialog).toBe(live.dialog);
        expect(state.onSaved).toHaveBeenCalledExactlyOnceWith('Daily', 'Original', true);
    });

    it.each(['rejected', 'lost response'])('keeps a reopened Edit retryable without accepting the submission on %s', async (failure) => {
        const state = pendingSave();
        const source = state.owner.acceptedProfile;
        state.owner.draft.settings.x = false;
        const promise = state.owner.saveEdit();
        state.owner.cancel();
        state.owner.reedit();
        state.owner.draft.description = 'Later unsent description';
        if (failure === 'rejected') state.resolve({ ok: false, text: async () => 'Storage unavailable' });
        else state.reject(new Error('Lost acknowledgement'));
        await promise;
        expect(state.owner.acceptedProfile).toBe(source);
        expect(state.owner.acceptedProfile.settings.x).toBe(true);
        expect(state.owner.draft.settings.x).toBe(false);
        expect(state.owner.draft.description).toBe('Later unsent description');
        expect(state.owner.savingProfile).toBeNull();
        expect(state.onSaved).not.toHaveBeenCalled();
        expect(fetch).toHaveBeenCalledOnce();
    });

    it.each(['missing response', 'unreadable rejection'])('keeps the entire draft without retry on %s', async (failure) => {
        const state = pendingSave();
        const draft = state.owner.draft;
        const before = JSON.stringify(draft);
        const promise = submit(state, true);
        if (failure === 'missing response') state.reject(new Error('Lost acknowledgement'));
        else state.resolve({ ok: false, text: async () => { throw new Error('Lost error body'); } });
        await promise;
        expect(fetch).toHaveBeenCalledTimes(1);
        expect(state.owner.draft).toBe(draft);
        expect(JSON.stringify(draft)).toBe(before);
        expect(state.owner.namingDialog.name).toBe('Unique');
        expect(state.owner.savingProfile).toBeNull();
        expect(state.onSaved).not.toHaveBeenCalled();
    });

    it('accepts successful headers without depending on an optional success body', async () => {
        const state = pendingSave();
        const promise = submit(state, false);
        const json = vi.fn(async () => { throw new Error('unreadable optional body'); });
        state.resolve({ ok: true, json });
        await promise;
        expect(json).not.toHaveBeenCalled();
        expect(state.owner.draft).toBeNull();
        expect(state.onSaved).toHaveBeenCalledOnce();
    });

    it('retires publication on disposal without resending an outstanding save', async () => {
        const state = pendingSave();
        const promise = submit(state, false);
        state.owner.dispose();
        const messages = state.onMessage.mock.calls.length;
        state.resolve({ ok: true });
        await promise;
        expect(state.onSaved).not.toHaveBeenCalled();
        expect(state.onMessage).toHaveBeenCalledTimes(messages);
        expect(fetch).toHaveBeenCalledOnce();
    });
});
