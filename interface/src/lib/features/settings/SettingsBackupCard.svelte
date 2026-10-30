<script>
    import { onMount } from 'svelte';
    import CardSectionHead from '#lib/components/CardSectionHead.svelte';
    import { createPoll, fetchJsonWithTimeout } from '#lib/utils/poll.js';

    let {
        backingUpNow = false,
        onbackupNowToSd,
        ondownloadBackup,
        restoreFile = null,
        restoring = false,
        restoreDisabled = false,
        mutationBusy = false,
        onfileSelect,
        onrestoreBackup
    } = $props();

    let mounted = $state(false);
    let recovery = $state(null);
    let statusUnavailable = $state(false);
    let requestGeneration = 0;
    let requestInFlight = false;
    let refreshRequested = false;

    function validStatus(value) {
        const flags = ['internal_save_pending', 'storage_ready', 'sd_mounted', 'recovery_pending',
            'restore_pending', 'backup_queued', 'backup_writing', 'backup_retry_pending',
            'has_written_snapshot', 'last_write_matches_current'];
        return value && flags.every((key) => typeof value[key] === 'boolean') &&
            ['unknown', 'succeeded', 'failed'].includes(value.internal_save) &&
            ['unknown', 'written', 'preserved', 'failed', 'superseded', 'unavailable']
                .includes(value.last_backup_attempt);
    }

    async function refreshStatus() {
        if (!mounted || mutationBusy) return;
        if (requestInFlight) {
            refreshRequested = true;
            return;
        }
        requestInFlight = true;
        const generation = requestGeneration;
        try {
            const response = await fetchJsonWithTimeout('/api/settings/recovery', {}, 2500);
            if (!mounted || mutationBusy || generation !== requestGeneration) return;
            recovery = response.ok && validStatus(response.data) ? response.data : null;
            statusUnavailable = !recovery;
        } catch {
            if (mounted && !mutationBusy && generation === requestGeneration) {
                recovery = null;
                statusUnavailable = true;
            }
        } finally {
            requestInFlight = false;
            if (refreshRequested) {
                refreshRequested = false;
                void refreshStatus();
            }
        }
    }

    $effect(() => {
        // Mutation begins and ends each invalidate an older response. Never
        // let a pre-save poll restore a success message after a newer change.
        const busy = mutationBusy;
        if (!mounted) return;
        requestGeneration += 1;
        recovery = null;
        statusUnavailable = false;
        if (!busy) void refreshStatus();
    });

    onMount(() => {
        mounted = true;
        const poll = createPoll(refreshStatus, 3000);
        poll.start();
        return () => {
            mounted = false;
            requestGeneration += 1;
            poll.stop();
        };
    });
</script>

<div class="surface-card">
    <div class="card-body space-y-4">
        <CardSectionHead
            title="Backup & Restore"
            subtitle="Download your settings or restore from a backup file."
        />

        <div class="space-y-2 text-sm" aria-live="polite" aria-label="Saved settings and backup status">
            {#if mutationBusy}
                <p>Updating settings. Backup status will refresh when this finishes.</p>
            {:else if !recovery}
                <p class="copy-muted">{statusUnavailable ? 'Backup status unavailable. You can still use the controls below.' : 'Checking saved settings and backup status…'}</p>
            {:else}
                <p><strong>Internal settings:</strong>
                    {#if recovery.internal_save_pending}
                        Save pending. Keep the device powered while it finishes.
                    {:else if recovery.internal_save === 'failed'}
                        Last save failed. Retry Save Settings before relying on this change.
                    {:else if recovery.internal_save === 'succeeded'}
                        Last internal save completed.
                    {:else}
                        No save observed since device startup. Existing saved settings may still be present.
                    {/if}
                </p>
                <p><strong>SD backup:</strong>
                    {#if !recovery.sd_mounted}
                        No mounted SD backup is available. Normal use without an SD card is supported; Download Backup keeps a copy on this computer.
                    {:else if recovery.last_backup_attempt === 'preserved'}
                        Last attempt preserved an existing recovery copy. It is not confirmation that the current settings were written.
                    {:else if recovery.recovery_pending || recovery.restore_pending}
                        Current backup not confirmed while recovery is unresolved. Keep any prior backups.
                    {:else if recovery.backup_writing}
                        Backup in progress. Wait for its result before disconnecting power.
                    {:else if recovery.last_backup_attempt === 'failed' || recovery.last_backup_attempt === 'unavailable'}
                        Last backup attempt did not complete. Keep existing backups; check the SD card and retry Backup to SD Now.
                    {:else if recovery.backup_retry_pending}
                        Backup waiting to retry. A new completed copy has not been confirmed.
                    {:else if recovery.backup_queued}
                        Backup queued. It has not completed yet.
                    {:else if recovery.has_written_snapshot && recovery.last_write_matches_current && recovery.last_backup_attempt === 'written'}
                        Last observed write matches the current settings and profiles.
                    {:else if recovery.has_written_snapshot}
                        Last observed write does not confirm the current settings. Use Backup to SD Now for a new copy.
                    {:else}
                        No completed write observed since device startup. Use Backup to SD Now to create a current copy.
                    {/if}
                </p>
                <p><strong>Recovery:</strong>
                    {#if recovery.recovery_pending}
                        Settings recovery is pending. Keep the existing SD card and backups; let recovery finish before replacing data.
                    {:else if recovery.restore_pending}
                        SD recovery remains unverified. Keep any prior backup; normal use without an SD card is supported.
                    {:else}
                        No settings recovery is currently reported.
                    {/if}
                </p>
                <p class="copy-muted text-xs">These observations start at device startup. Card contents are not continuously checked; a mounted card or an earlier write is not a media-health guarantee.</p>
            {/if}
        </div>

        <div class="flex flex-col gap-3">
            <button
                class="btn btn-primary btn-sm"
                onclick={onbackupNowToSd}
                disabled={backingUpNow}
            >
                {#if backingUpNow}
                    <span class="loading loading-sm loading-spinner"></span>
                {/if}
                Backup to SD Now
            </button>

            <div class="divider my-0">OR</div>

            <button class="btn btn-outline btn-sm" onclick={() => ondownloadBackup?.()}>
                Download Backup
            </button>

            <p class="copy-muted text-xs">
                Downloaded backups omit WiFi passwords. Use a local SD backup for full device
                recovery.
            </p>

            <div class="divider my-0">OR</div>

            <input
                type="file"
                accept=".json,application/json"
                class="file-input w-full file-input-sm"
                onchange={onfileSelect}
            />

            <button
                class="btn btn-sm btn-warning"
                onclick={onrestoreBackup}
                disabled={!restoreFile || restoring || restoreDisabled}
            >
                {#if restoring}
                    <span class="loading loading-sm loading-spinner"></span>
                {/if}
                Restore from Backup
            </button>
        </div>
    </div>
</div>
