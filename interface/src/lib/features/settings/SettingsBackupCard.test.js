import { render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

import SettingsBackupCard from './SettingsBackupCard.svelte';

function status(overrides = {}) {
    return {
        internal_save: 'unknown', last_backup_attempt: 'unknown',
        internal_save_pending: false, storage_ready: true, sd_mounted: true,
        recovery_pending: false, restore_pending: false, backup_queued: false,
        backup_writing: false, backup_retry_pending: false, has_written_snapshot: false,
        last_write_matches_current: false, current_revision: 2, last_written_revision: null,
        ...overrides
    };
}

function response(data, ok = true) {
    return { ok, status: ok ? 200 : 503, json: async () => data };
}

function deferred() {
    let resolve;
    const promise = new Promise((done) => { resolve = done; });
    return { promise, resolve };
}

afterEach(() => {
    vi.useRealTimers();
    vi.unstubAllGlobals();
});

describe('owner-visible recovery observations', () => {
    it.each([
        [status(), 'No completed write observed since device startup.'],
        [status({ sd_mounted: false }), 'Normal use without an SD card is supported'],
        [status({ backup_queued: true, last_write_matches_current: true }), 'Backup queued. It has not completed yet.'],
        [status({ backup_writing: true, last_write_matches_current: true }), 'Backup in progress.'],
        [status({ backup_retry_pending: true }), 'Backup waiting to retry.'],
        [status({ last_backup_attempt: 'failed', has_written_snapshot: true, last_write_matches_current: true }), 'Last backup attempt did not complete.'],
        [status({ restore_pending: true, last_backup_attempt: 'preserved', last_write_matches_current: true }), 'SD recovery remains unverified.'],
        [status({ recovery_pending: true }), 'Settings recovery is pending.'],
        [status({ has_written_snapshot: true }), 'Last observed write does not confirm the current settings.']
    ])('distinguishes incomplete and unsupported evidence %#', async (data, expected) => {
        vi.stubGlobal('fetch', vi.fn(async () => response(data)));
        render(SettingsBackupCard);
        await screen.findByText((text) => text.includes(expected));
        expect(screen.queryByText(/Last observed write matches the current/)).not.toBeInTheDocument();
    });

    it('describes a matching receipt as a boot observation with a media limitation', async () => {
        vi.stubGlobal('fetch', vi.fn(async () => response(status({
            internal_save: 'succeeded', last_backup_attempt: 'written',
            has_written_snapshot: true, last_write_matches_current: true, last_written_revision: 2
        }))));
        render(SettingsBackupCard);
        await screen.findByText(/Last observed write matches the current settings and profiles/);
        expect(screen.getByText(/Card contents are not continuously checked/)).toBeInTheDocument();
        expect(screen.getByText(/Last internal save completed/)).toBeInTheDocument();
    });

    it('does not invent an existing backup from unresolved recovery', async () => {
        vi.stubGlobal('fetch', vi.fn(async () => response(status({ recovery_pending: true }))));
        render(SettingsBackupCard);
        await screen.findByText(/Current backup not confirmed while recovery is unresolved/);
        expect(screen.queryByText(/preserved an existing recovery copy/)).not.toBeInTheDocument();
    });

    it('discards a response from before a mutation and refreshes when it settles', async () => {
        const old = deferred();
        const fetch = vi.fn()
            .mockImplementationOnce(() => old.promise)
            .mockResolvedValue(response(status({ backup_queued: true })));
        vi.stubGlobal('fetch', fetch);
        const view = render(SettingsBackupCard, { mutationBusy: false });
        await waitFor(() => expect(fetch).toHaveBeenCalledTimes(1));
        await view.rerender({ mutationBusy: true });
        expect(screen.getByText(/Updating settings/)).toBeInTheDocument();
        await view.rerender({ mutationBusy: false });
        old.resolve(response(status({ last_backup_attempt: 'written', has_written_snapshot: true, last_write_matches_current: true })));
        await screen.findByText(/Backup queued. It has not completed yet/);
        expect(fetch).toHaveBeenCalledTimes(2);
        expect(screen.queryByText(/Last observed write matches/)).not.toBeInTheDocument();
    });

    it('replaces prior success with unavailable when the next poll fails', async () => {
        vi.useFakeTimers();
        const fetch = vi.fn()
            .mockResolvedValueOnce(response(status({ last_backup_attempt: 'written', has_written_snapshot: true, last_write_matches_current: true })))
            .mockRejectedValue(new Error('offline'));
        vi.stubGlobal('fetch', fetch);
        render(SettingsBackupCard);
        await vi.advanceTimersByTimeAsync(1);
        expect(screen.getByText(/Last observed write matches/)).toBeInTheDocument();
        await vi.advanceTimersByTimeAsync(3000);
        expect(screen.getByText(/Backup status unavailable/)).toBeInTheDocument();
        expect(screen.queryByText(/Last observed write matches/)).not.toBeInTheDocument();
    });

    it.each([response({}, true), response(null, false)])('treats a missing or malformed owner as unavailable %#', async (result) => {
        vi.stubGlobal('fetch', vi.fn(async () => result));
        render(SettingsBackupCard);
        await screen.findByText(/Backup status unavailable/);
        expect(screen.getByRole('button', { name: 'Download Backup' })).toBeEnabled();
    });

    it('does not restart polling or accept a response after unmount', async () => {
        vi.useFakeTimers();
        const pending = deferred();
        const fetch = vi.fn(() => pending.promise);
        vi.stubGlobal('fetch', fetch);
        const view = render(SettingsBackupCard);
        await vi.advanceTimersByTimeAsync(1);
        view.unmount();
        pending.resolve(response(status()));
        await vi.advanceTimersByTimeAsync(10_000);
        expect(fetch).toHaveBeenCalledTimes(1);
    });
});
