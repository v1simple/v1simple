import { afterEach, describe, expect, it, vi } from 'vitest';
import { nativeDialog } from './nativeDialog.js';

function createDialog() {
    const dialog = document.createElement('dialog');
    const input = document.createElement('input');
    dialog.append(input);
    document.body.append(dialog);
    return { dialog, input };
}

function createOpener() {
    const opener = document.createElement('button');
    document.body.append(opener);
    opener.focus();
    return opener;
}

describe('native dialog teardown focus', () => {
    afterEach(() => {
        document.body.replaceChildren();
        vi.restoreAllMocks();
    });

    it('returns focus when Svelte removes the dialog before action teardown', () => {
        const opener = createOpener();
        const { dialog, input } = createDialog();
        const lifecycle = nativeDialog(dialog);
        // jsdom does not perform showModal focus; model the browser observation.
        input.focus();
        dialog.remove();
        expect(document.activeElement).toBe(document.body);
        lifecycle.destroy();
        expect(opener).toHaveFocus();
    });

    it('does not steal focus from another open dialog during teardown', () => {
        createOpener();
        const first = createDialog();
        const lifecycle = nativeDialog(first.dialog);
        first.input.focus();
        const next = createDialog();
        next.dialog.showModal();
        next.input.focus();
        first.dialog.remove();
        lifecycle.destroy();
        expect(next.input).toHaveFocus();
    });

    it('does not try to restore an opener removed with its page', () => {
        const opener = createOpener();
        const { dialog, input } = createDialog();
        const lifecycle = nativeDialog(dialog);
        input.focus();
        opener.remove();
        const focus = vi.spyOn(opener, 'focus');
        dialog.remove();
        lifecycle.destroy();
        expect(focus).not.toHaveBeenCalled();
    });

    it('keeps the original opener across a loading dialog replacement', () => {
        const opener = createOpener();
        const loading = createDialog();
        const loadingLifecycle = nativeDialog(loading.dialog);
        loading.input.focus();
        loading.dialog.remove();
        loadingLifecycle.destroy();
        const loaded = createDialog();
        const loadedLifecycle = nativeDialog(loaded.dialog);
        loaded.input.focus();
        loaded.dialog.remove();
        loadedLifecycle.destroy();
        expect(opener).toHaveFocus();
    });
});
