<script>
    import { IN_THE_BOX_BANDS, IN_THE_BOX_BOXES, inTheBoxSettingsError } from '$lib/features/profiles/inTheBoxSettings';

    let { settings = $bindable(), editingSettings = false } = $props();
    let error = $derived(inTheBoxSettingsError(settings));
</script>

<div class="space-y-4">
    <div class="space-y-2">
        <h3 class="font-semibold">In-the-Box</h3>
        <p class="copy-caption">V1Simple must be connected to use these profile options. Both actions start off. Muting affects the whole detector; boxes never change scanning or hide alerts.</p>
        <p class="copy-caption">Out-of-the-Box muting requires every current radar alert to qualify outside its enabled boxes. Other alerts, unknown frequencies, and laser prevent it. Photo radar can fall outside these boxes. V1Simple releases its mute when the condition ends.</p>
        <p class="copy-caption">In-the-Box unmuting releases a mute when a new alert arrives inside an enabled box. A later mute from V1Simple takes precedence for that alert.</p>
    </div>
    {#each IN_THE_BOX_BANDS as band}
        <div class="surface-panel space-y-3">
            <h4 class="font-semibold">{band.label} band</h4>
            <div class="grid gap-3 sm:grid-cols-2">
                <label class="flex items-center justify-between gap-3 text-sm">
                    <span>Out-of-the-Box muting</span>
                    <input type="checkbox" class="toggle toggle-primary toggle-sm"
                        aria-label={`${band.label} Out-of-the-Box muting`}
                        bind:checked={settings.bands[band.key].muteOutside} disabled={!editingSettings} />
                </label>
                <label class="flex items-center justify-between gap-3 text-sm">
                    <span>In-the-Box unmuting</span>
                    <input type="checkbox" class="toggle toggle-primary toggle-sm"
                        aria-label={`${band.label} In-the-Box unmuting`}
                        bind:checked={settings.bands[band.key].unmuteInside} disabled={!editingSettings} />
                </label>
            </div>
            {#if settings.bands[band.key].muteOutside && band.boxes.every((key) => !settings.boxes[key].enabled)}
                <p class="copy-caption">With no boxes enabled, every {band.label} alert qualifies for outside muting.</p>
            {/if}
            {#each band.boxes as key}
                {@const box = IN_THE_BOX_BOXES[key]}
                <div class="grid items-end gap-3 border-t border-base-300 pt-3 sm:grid-cols-3">
                    <label class="flex items-center justify-between gap-3 pb-2 text-sm">
                        <span>{box.label} box</span>
                        <input type="checkbox" class="toggle toggle-primary toggle-sm"
                            aria-label={`Enable ${box.label} box`}
                            bind:checked={settings.boxes[key].enabled} disabled={!editingSettings} />
                    </label>
                    <label class="field-control">
                        <span class="field-label text-xs">Lower MHz</span>
                        <input type="number" class="input input-sm w-full" min={box.min} max={box.max} step="1"
                            aria-label={`${box.label} box lower MHz`}
                            bind:value={settings.boxes[key].lowerMHz} disabled={!editingSettings} />
                    </label>
                    <label class="field-control">
                        <span class="field-label text-xs">Upper MHz</span>
                        <input type="number" class="input input-sm w-full" min={box.min} max={box.max} step="1"
                            aria-label={`${box.label} box upper MHz`}
                            bind:value={settings.boxes[key].upperMHz} disabled={!editingSettings} />
                    </label>
                </div>
            {/each}
        </div>
    {/each}
    {#if error}<p class="surface-alert alert-warning" role="alert">{error}</p>{/if}
</div>
