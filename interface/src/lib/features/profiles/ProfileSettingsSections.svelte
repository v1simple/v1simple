<script>
    import StatusAlert from '$lib/components/StatusAlert.svelte';
    import { customFrequencyBand } from '$lib/features/profiles/profileSettingsAdapter';

    let {
        editingSettings,
        currentProfile,
        editedSettings = $bindable(null),
        editedDetector = $bindable(null),
        frequencyError = null,
        oncustomFrequenciesChange,
        onaddCustomFrequencyRange,
        onremoveCustomFrequencyRange
    } = $props();

    let settings = $derived(editingSettings ? editedSettings : currentProfile.settings);
    let detector = $derived(editingSettings ? editedDetector : currentProfile.detector);

    function bandDefinitionCount(band) {
        if (!Array.isArray(detector?.customFrequencyDefinitions)) return 0;
        return detector.customFrequencyDefinitions
            .filter((definition) => customFrequencyBand(definition) === band).length;
    }

    function relinquishesBandOwnership(definition) {
        const band = customFrequencyBand(definition);
        return band !== null && bandDefinitionCount(band) === 1;
    }
</script>

<div class="space-y-3">
    <details class="surface-collapse">
        <summary class="collapse-title min-h-0 py-3 text-sm font-semibold">Bands</summary>
        <div class="collapse-content">
            <div class="grid grid-cols-1 gap-2 pt-2 text-sm sm:grid-cols-2">
                <label class="flex items-center justify-between">
                    <span>Laser</span>
                    <input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.laser} disabled={!editingSettings} />
                </label>
                <label class="flex items-center justify-between">
                    <span>Rear Laser</span>
                    <input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.laserRear} disabled={!editingSettings} />
                </label>
                <label class="flex items-center justify-between">
                    <span>X Band</span>
                    <input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.x} disabled={!editingSettings} />
                </label>
                <label class="flex items-center justify-between">
                    <span>Ka Band</span>
                    <input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.ka} disabled={!editingSettings} />
                </label>
                <label class="flex items-center justify-between">
                    <span>K Band</span>
                    <input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.k} disabled={!editingSettings} />
                </label>
                <label class="flex items-center justify-between">
                    <span>Ku Band</span>
                    <input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.ku} disabled={!editingSettings} />
                </label>
            </div>
        </div>
    </details>

    <details class="surface-collapse">
        <summary class="collapse-title min-h-0 py-3 text-sm font-semibold">Mute Control</summary>
        <div class="collapse-content space-y-3">
            <div class="grid grid-cols-1 gap-2 pt-2 text-sm sm:grid-cols-2">
                <label class="flex items-center justify-between">
                    <span>Mute-to-Muted Volume</span>
                    <input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.muteToMuteVolume} disabled={!editingSettings} />
                </label>
                <label class="flex items-center justify-between">
                    <span>Bogey-Lock tone Loud after muting</span>
                    <input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.bogeyLockLoud} disabled={!editingSettings} />
                </label>
                <label class="flex items-center justify-between">
                    <span>Mute Rear X &amp; K alerts</span>
                    <input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.muteXKRear} disabled={!editingSettings} />
                </label>
                <div class="flex items-center justify-between">
                    <span>Auto Mute</span>
                    {#if editingSettings}
                        <select aria-label="X, K, Ku Automute" class="select w-28 select-xs" bind:value={settings.autoMute}>
                            <option value={2}>On</option>
                            <option value={1}>Advanced</option>
                            <option value={3}>Off</option>
                        </select>
                    {:else}
                        <span class="badge badge-info">{settings.autoMute === 3 ? 'Off' : settings.autoMute === 2 ? 'On' : 'Advanced'}</span>
                    {/if}
                </div>
            </div>
            {#if detector}
                <div class="grid gap-3 sm:grid-cols-2">
                    <label class="field-control">
                        <span class="field-label copy-caption">Volume policy</span>
                        <select
                            class="select select-sm"
                            bind:value={detector.volumePolicy}
                            onchange={() => {
                                if (detector.volumePolicy !== 'temporary') detector.volumeDisconnect = 'restore_saved';
                            }}
                            disabled={!editingSettings}
                        >
                            <option value="unchanged">Leave unchanged</option>
                            <option value="temporary">Temporary</option>
                            <option value="saved">Save on V1</option>
                        </select>
                    </label>
                    <label class="field-control">
                        <span class="field-label copy-caption">Main volume (0–9)</span>
                        <input class="input input-sm" type="number" min="0" max="9" bind:value={detector.mainVolume} disabled={!editingSettings || detector.volumePolicy === 'unchanged'} />
                    </label>
                    <label class="field-control">
                        <span class="field-label copy-caption">Muted volume (0–9)</span>
                        <input class="input input-sm" type="number" min="0" max="9" bind:value={detector.mutedVolume} disabled={!editingSettings || detector.volumePolicy === 'unchanged'} />
                    </label>
                    <label class="field-control">
                        <span class="field-label copy-caption">Volume feedback</span>
                        <select class="select select-sm" bind:value={detector.volumeFeedback} disabled={!editingSettings || detector.volumePolicy === 'unchanged'}>
                            <option value="none">None</option>
                            <option value="changed_only">Only when changed</option>
                            <option value="always">Always</option>
                        </select>
                    </label>
                    <label class="field-control">
                        <span class="field-label copy-caption">After Bluetooth disconnect</span>
                        <select class="select select-sm" bind:value={detector.volumeDisconnect} disabled={!editingSettings || detector.volumePolicy !== 'temporary'}>
                            <option value="restore_saved">Restore saved volume</option>
                            <option value="keep_current">Keep temporary volume</option>
                        </select>
                    </label>
                </div>
            {/if}
        </div>
    </details>

    <details class="surface-collapse">
        <summary class="collapse-title min-h-0 py-3 text-sm font-semibold">Photo Radar</summary>
        <div class="collapse-content space-y-3">
            <div class="grid grid-cols-1 gap-2 pt-2 text-sm sm:grid-cols-2">
                <label class="flex items-center justify-between"><span>Photo Verifier</span><input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.photoVerifier} disabled={!editingSettings} /></label>
                <label class="flex items-center justify-between"><span>MRCT</span><input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.mrct} disabled={!editingSettings} /></label>
                <label class="flex items-center justify-between"><span>DriveSafe™ 3D</span><input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.driveSafe3D} disabled={!editingSettings} /></label>
                <label class="flex items-center justify-between"><span>DriveSafe™ 3DHD</span><input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.driveSafe3DHD} disabled={!editingSettings} /></label>
                <label class="flex items-center justify-between"><span>Redflex® Halo</span><input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.redflexHalo} disabled={!editingSettings} /></label>
                <label class="flex items-center justify-between"><span>Redflex® NK7</span><input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.redflexNK7} disabled={!editingSettings} /></label>
                <label class="flex items-center justify-between"><span>Ekin</span><input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.ekin} disabled={!editingSettings} /></label>
                <label class="flex items-center justify-between"><span>Gatso RT4</span><input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.gatsoRT4} disabled={!editingSettings} /></label>
                <label class="flex items-center justify-between"><span>Photo Radar Intersection Management Filter</span><input aria-label="Intersection Management Filter" type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.photoIntersectionFilter} disabled={!editingSettings} /></label>
            </div>
            {#if settings.photoIntersectionFilter}
                <StatusAlert
                    fallbackType="warning"
                    message="Intersection Management suppresses DriveSafe 3D, DriveSafe 3DHD, and Ekin alerts while enabled. Those saved settings are not changed."
                />
            {/if}
        </div>
    </details>

    <details class="surface-collapse">
        <summary class="collapse-title min-h-0 py-3 text-sm font-semibold">Special</summary>
        <div class="collapse-content space-y-3">
            <div class="grid grid-cols-1 gap-2 pt-2 text-sm sm:grid-cols-2">
                <label class="flex items-center justify-between"><span>Euro Mode</span><input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.euroMode} disabled={!editingSettings} /></label>
                <label class="flex items-center justify-between"><span>K-Verifier (TMF)</span><input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.kVerifier} disabled={!editingSettings} /></label>
                <label class="flex items-center justify-between"><span>Ka Always Radar Priority</span><input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.kaAlwaysPriority} disabled={!editingSettings} /></label>
                <label class="flex items-center justify-between"><span>Fast Laser Detection</span><input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.fastLaserDetect} disabled={!editingSettings} /></label>
                <label class="flex items-center justify-between"><span>Startup Sequence</span><input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.startupSequence} disabled={!editingSettings} /></label>
                <label class="flex items-center justify-between"><span>Resting Display</span><input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.restingDisplay} disabled={!editingSettings} /></label>
                <label class="flex items-center justify-between"><span>BSM Plus</span><input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.bsmPlus} disabled={!editingSettings} /></label>
            </div>
            <StatusAlert
                fallbackType="info"
                message="Valentine profile Alert Persistence is not mapped yet. V1Simple's existing 0–5 second display persistence remains a separate Auto-Push slot setting."
            />
            <div class="grid grid-cols-1 gap-3 text-sm sm:grid-cols-3">
                <label class="flex items-center justify-between">
                    <span>Ka Sensitivity</span>
                    <select aria-label="Ka Sensitivity" class="select w-24 select-xs" bind:value={settings.kaSensitivity} disabled={!editingSettings}>
                        <option value={1}>Relaxed</option><option value={2}>Original</option><option value={3}>Full</option>
                    </select>
                </label>
                <label class="flex items-center justify-between">
                    <span>K Sensitivity</span>
                    <select aria-label="K Sensitivity" class="select w-24 select-xs" bind:value={settings.kSensitivity} disabled={!editingSettings}>
                        <option value={1}>Relaxed</option><option value={2}>Full</option><option value={3}>Original</option>
                    </select>
                </label>
                <label class="flex items-center justify-between">
                    <span>X Sensitivity</span>
                    <select aria-label="X Sensitivity" class="select w-24 select-xs" bind:value={settings.xSensitivity} disabled={!editingSettings}>
                        <option value={1}>Relaxed</option><option value={2}>Full</option><option value={3}>Original</option>
                    </select>
                </label>
            </div>
            {#if detector}
                <div class="grid gap-3 sm:grid-cols-2">
                    <label class="field-control">
                        <span class="field-label copy-caption">User settings bytes</span>
                        <select class="select select-sm" bind:value={detector.userSettings} disabled={!editingSettings}>
                            <option value="value">Apply profile settings</option>
                            <option value="unchanged">Leave unchanged</option>
                        </select>
                    </label>
                    <div class="grid grid-cols-2 gap-2">
                        <label class="field-control">
                            <span class="field-label copy-caption">Valentine One mode</span>
                            <select class="select select-sm" bind:value={detector.modePolicy} disabled={!editingSettings}>
                                <option value="unchanged">Leave unchanged</option>
                                <option value="value">Set mode</option>
                            </select>
                        </label>
                        <label class="field-control">
                            <span class="field-label copy-caption">Mode value</span>
                            <select class="select select-sm" bind:value={detector.mode} disabled={!editingSettings || detector.modePolicy !== 'value'}>
                                <option value={1}>All Bogeys</option><option value={2}>Logic</option><option value={3}>Advanced Logic</option>
                            </select>
                        </label>
                    </div>
                    <label class="field-control">
                        <span class="field-label copy-caption">Dark mode</span>
                        <select
                            class="select select-sm"
                            bind:value={detector.display}
                            onchange={() => {
                                if (detector.display !== 'off') detector.bluetoothLed = 'unchanged';
                            }}
                            disabled={!editingSettings}
                        >
                            <option value="unchanged">Leave unchanged</option><option value="on">Off (display on)</option><option value="off">On (main display off)</option>
                        </select>
                    </label>
                    <label class="field-control">
                        <span class="field-label copy-caption">Bluetooth indicator while display is off</span>
                        <select class="select select-sm" bind:value={detector.bluetoothLed} disabled={!editingSettings || detector.display !== 'off'}>
                            <option value="unchanged">Leave unchanged</option><option value="off">Off</option><option value="on">Keep indicator active (on or blinking)</option>
                        </select>
                    </label>
                </div>
            {/if}
        </div>
    </details>

    <details class="surface-collapse">
        <summary class="collapse-title min-h-0 py-3 text-sm font-semibold">SAVVY Settings</summary>
        <div class="collapse-content pt-2">
            <StatusAlert
                fallbackType="info"
                message="SAVVY accessory controls are unavailable. V1Simple does not save or send SAVVY settings until their device and protocol behavior is qualified."
            />
        </div>
    </details>

    <details class="surface-collapse">
        <summary class="collapse-title min-h-0 py-3 text-sm font-semibold">Custom Frequencies</summary>
        <div class="collapse-content space-y-3">
            <label class="flex items-center justify-between pt-2 text-sm">
                <span>Enable Custom Frequencies</span>
                <input
                    type="checkbox"
                    class="toggle toggle-primary toggle-sm"
                    checked={settings.customFreqs}
                    onchange={(event) => {
                        settings.customFreqs = event.currentTarget.checked;
                        oncustomFrequenciesChange?.(settings.customFreqs);
                    }}
                    disabled={!editingSettings}
                />
            </label>
            <p class="copy-caption">
                A band is profile-owned while it has at least one authored range. An unowned band uses the fresh DUT ranges during Apply. Removing a band's last range relinquishes that ownership. Ranges remain editable while Custom Frequencies is disabled.
            </p>
            <div class="grid gap-2 text-sm sm:grid-cols-2">
                <div class="surface-panel space-y-1">
                    <div class="flex items-center justify-between gap-2">
                        <span class="font-semibold">K ranges</span>
                        <span class={`badge ${bandDefinitionCount('K') > 0 ? 'badge-primary' : 'badge-ghost'}`}>
                            {bandDefinitionCount('K') > 0 ? 'Profile-owned' : 'Fresh DUT ranges on Apply'}
                        </span>
                    </div>
                    {#if bandDefinitionCount('K') === 0}
                        <p class="copy-caption">Add a K range to make this profile own K.</p>
                    {/if}
                </div>
                <div class="surface-panel space-y-1">
                    <div class="flex items-center justify-between gap-2">
                        <span class="font-semibold">Ka ranges</span>
                        <span class={`badge ${bandDefinitionCount('Ka') > 0 ? 'badge-primary' : 'badge-ghost'}`}>
                            {bandDefinitionCount('Ka') > 0 ? 'Profile-owned' : 'Fresh DUT ranges on Apply'}
                        </span>
                    </div>
                    {#if bandDefinitionCount('Ka') === 0}
                        <p class="copy-caption">Add a Ka range to make this profile own Ka.</p>
                    {/if}
                </div>
            </div>
            {#if detector?.customFrequencyPolicy === 'value'}
                <div class="max-h-72 overflow-auto">
                    <table class="table table-xs">
                        <thead><tr><th>Band</th><th>Lower MHz</th><th>Upper MHz</th><th><span class="sr-only">Actions</span></th></tr></thead>
                        <tbody>
                            {#each detector.customFrequencyDefinitions as definition (definition.index)}
                                <tr>
                                    <td>{customFrequencyBand(definition) || 'Invalid'}</td>
                                    <td><input aria-label={`Custom ${definition.index} lower MHz`} class="input input-xs w-28" type="number" min="0" max="65535" bind:value={definition.lowerMHz} disabled={!editingSettings} /></td>
                                    <td><input aria-label={`Custom ${definition.index} upper MHz`} class="input input-xs w-28" type="number" min="0" max="65535" bind:value={definition.upperMHz} disabled={!editingSettings} /></td>
                                    <td>
                                        <button
                                            class="btn btn-ghost btn-xs"
                                            type="button"
                                            aria-label={relinquishesBandOwnership(definition)
                                                ? `Remove custom frequency ${definition.index} and relinquish ${customFrequencyBand(definition)} ownership`
                                                : `Remove custom frequency ${definition.index}`}
                                            onclick={() => onremoveCustomFrequencyRange?.(definition.index)}
                                            disabled={!editingSettings}
                                        >
                                            {relinquishesBandOwnership(definition)
                                                ? `Remove; use DUT ${customFrequencyBand(definition)}`
                                                : 'Remove'}
                                        </button>
                                    </td>
                                </tr>
                            {/each}
                        </tbody>
                    </table>
                </div>
            {:else}
                <p class="copy-caption">
                    This profile owns no frequency table and sends no table update. Add a K or Ka range to take ownership of that band.
                </p>
            {/if}
            {#if frequencyError}
                <div class="surface-alert alert-warning" role="alert">{frequencyError}</div>
            {/if}
            <div class="flex flex-wrap gap-2">
                <button class="btn btn-outline btn-xs" type="button" onclick={() => onaddCustomFrequencyRange?.('k')} disabled={!editingSettings}>Add K range</button>
                <button class="btn btn-outline btn-xs" type="button" onclick={() => onaddCustomFrequencyRange?.('ka')} disabled={!editingSettings}>Add Ka range</button>
            </div>
        </div>
    </details>

    <details class="surface-collapse">
        <summary class="collapse-title min-h-0 py-3 text-sm font-semibold">In-the-Box Options</summary>
        <div class="collapse-content pt-2">
            <StatusAlert
                fallbackType="info"
                message="In-the-Box profile controls are unavailable. V1Simple does not save or enforce box classification, muting, or unmuting behavior until the feature is qualified end to end."
            />
        </div>
    </details>
</div>
