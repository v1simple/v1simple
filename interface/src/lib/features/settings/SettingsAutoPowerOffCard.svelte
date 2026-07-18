<script>
    import CardSectionHead from '$lib/components/CardSectionHead.svelte';

    let { settings } = $props();
</script>

<div class="surface-card">
    <div class="card-body space-y-4">
        <CardSectionHead
            title="Auto Shutdown"
            subtitle="Standard hardware: battery operation requests a hard latch cut; attached external power enters deep sleep. Car-install firmware does not use this control."
        />
        <div class="field-control">
            <label class="label" for="auto-power-off">
                <span class="field-label">Minutes after disconnect (0 = disabled)</span>
            </label>
            <input
                id="auto-power-off"
                type="number"
                class="input w-24"
                bind:value={settings.autoPowerOffMinutes}
                min="0"
                max="60"
                placeholder="0"
            />
            <div class="label">
                <span class="field-hint">
                    {#if settings.autoPowerOffMinutes > 0}
                        Device will shut down {settings.autoPowerOffMinutes} minute{settings.autoPowerOffMinutes !==
                        1
                            ? 's'
                            : ''} after V1 disconnects or ALP goes silent
                    {:else}
                        Auto shutdown is disabled
                    {/if}
                </span>
            </div>
        </div>

        <div class="divider my-1"></div>

        <div class="field-control">
            <div class="label gap-4">
                <div>
                    <label class="field-label cursor-pointer" for="power-off-sd-log"
                        >Record Shutdown Evidence</label
                    >
                    <p class="copy-caption-soft mt-1">
                        Records power source, latch/deep-sleep outcome, and the next boot reason to
                        <code>/poweroff.log</code> on the SD card. Review it from the
                        <a class="link link-primary" href="/logs">Logs page</a>.
                    </p>
                </div>
                <input
                    id="power-off-sd-log"
                    type="checkbox"
                    class="toggle toggle-primary shrink-0"
                    bind:checked={settings.powerOffSdLog}
                />
            </div>
        </div>
    </div>
</div>
