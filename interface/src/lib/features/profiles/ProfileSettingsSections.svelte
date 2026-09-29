<script>
    import StatusAlert from '$lib/components/StatusAlert.svelte';
    import ProfileInTheBoxControls from '$lib/features/profiles/ProfileInTheBoxControls.svelte';
    import ProfileEverydayControls from '$lib/features/profiles/ProfileEverydayControls.svelte';
    import { customFrequencyBand } from '$lib/features/profiles/profileSettingsAdapter';
    import { PROFILE_SETTING_GROUPS } from '$lib/features/profiles/profileSettingsCatalog';

    let {
        editingSettings, currentProfile, capturedSnapshot = null,
        editedSettings = $bindable(null), editedDetector = $bindable(null),
        editedInTheBox = $bindable(null),
        frequencyError = null,
        onaddCustomFrequencyRange, onremoveCustomFrequencyRange
    } = $props();

    let settings = $derived(editingSettings ? editedSettings : currentProfile.settings);
    let detector = $derived(editingSettings ? editedDetector : currentProfile.detector);
    let inTheBox = $derived(editingSettings ? editedInTheBox : currentProfile.inTheBox);
    let activeGroup = $state('everyday');
    let showAll = $state(false);
    let search = $state('');
    let query = $derived(search.trim().toLowerCase());
    let knownFirmware = $derived(capturedSnapshot?.capabilities?.versionKnown && capturedSnapshot?.capabilities?.gen2
        ? capturedSnapshot.firmware?.value : null);
    const groupLabels = { bands: 'Bands & sensitivity', muting: 'Muting', photo: 'Photo radar', filtering: 'Filters & startup' };
    const everydayKeywords = 'display everyday detector lights dark bluetooth ble operating mode logic volume main muted feedback disconnect';
    const boxKeywords = 'in-the-box out-of-the-box in the box out of the box boxes x ku k ka muting unmuting frequency lower upper mhz';
    const frequencyKeywords = 'custom frequencies frequency ranges sweeps k ka mhz';

    function matches(text) { return text.toLowerCase().includes(query); }
    function fieldMatches(field) {
        return matches(`${field.label} ${field.description || ''} ${field.key}`);
    }
    function groupMatches(group) {
        return matches(`${group.title} ${group.description}`) || group.fields.some(fieldMatches);
    }
    function visible(id) {
        if (!query) return showAll || activeGroup === id;
        if (id === 'everyday') return matches(everydayKeywords);
        if (id === 'frequencies') return matches(frequencyKeywords);
        if (id === 'boxes') return matches(boxKeywords);
        return groupMatches(PROFILE_SETTING_GROUPS.find((group) => group.id === id));
    }
    function chooseGroup(id) { activeGroup = id; search = ''; showAll = false; }
    function summary(group) {
        if (group.id === 'bands') {
            return group.fields.filter((field) => !field.options && settings[field.key])
                .map((field) => field.label.replace(' Band', '')).join(', ') || 'All bands off';
        }
        if (group.id === 'muting') {
            return `Automute ${settings.autoMute === 3 ? 'off' : settings.autoMute === 2 ? 'on' : 'advanced'}`;
        }
        const toggles = group.fields.filter((field) => !field.options);
        return `${toggles.filter((field) => settings[field.key]).length} of ${toggles.length} enabled`;
    }
    function formatFirmware(version) { return `${String(version)[0]}.${String(version).slice(1)}`; }
    function bandDefinitionCount(band) {
        if (detector?.customFrequencyPolicy !== 'value' || !Array.isArray(detector?.customFrequencyDefinitions)) return 0;
        return detector.customFrequencyDefinitions.filter((definition) => customFrequencyBand(definition) === band).length;
    }
    function relinquishesBandOwnership(definition) {
        const band = customFrequencyBand(definition);
        return band !== null && bandDefinitionCount(band) === 1;
    }
</script>

<div class="space-y-4">
    <div class="flex flex-wrap items-end gap-3">
        <label class="field-control min-w-0 flex-1">
            <span class="field-label text-sm">Find a profile setting</span>
            <input class="input w-full" type="search" placeholder="Try Bluetooth, rear laser, or Gatso" bind:value={search} />
        </label>
        <button class="btn btn-outline btn-sm" aria-pressed={showAll && !query}
            onclick={() => { showAll = !showAll; search = ''; }}>Show all settings</button>
    </div>
    <div class={query ? 'space-y-4' : 'grid gap-4 lg:grid-cols-[12rem_minmax(0,1fr)]'}>
        <nav hidden={!!query} class="grid content-start grid-cols-2 gap-2 sm:grid-cols-3 lg:grid-cols-1" aria-label="Profile setting groups">
            <button class="setting-group" class:selected={!query && !showAll && activeGroup === 'everyday'}
                aria-pressed={!query && !showAll && activeGroup === 'everyday'} onclick={() => chooseGroup('everyday')}>
                <strong>Lights, mode & volume</strong>
                <span>Display {detector?.display === 'unchanged' ? 'kept' : detector?.display} · volume {detector?.volumePolicy === 'unchanged' ? 'kept' : detector?.volumePolicy}</span>
            </button>
            {#each PROFILE_SETTING_GROUPS as group}
                <button class="setting-group" class:selected={!query && !showAll && activeGroup === group.id}
                    aria-pressed={!query && !showAll && activeGroup === group.id} onclick={() => chooseGroup(group.id)}>
                    <strong>{groupLabels[group.id]}</strong>
                    <span>{summary(group)}</span>
                </button>
            {/each}
            <button class="setting-group" class:selected={!query && !showAll && activeGroup === 'boxes'}
                aria-pressed={!query && !showAll && activeGroup === 'boxes'} onclick={() => chooseGroup('boxes')}>
                <strong>In-the-Box</strong>
                <span>{Object.values(inTheBox?.bands || {}).some((band) => band.muteOutside || band.unmuteInside) ? 'App muting options on' : 'App muting options off'}</span>
            </button>
            <button class="setting-group" class:selected={!query && !showAll && activeGroup === 'frequencies'}
                aria-pressed={!query && !showAll && activeGroup === 'frequencies'} onclick={() => chooseGroup('frequencies')}>
                <strong>Frequency ranges</strong>
                <span>{detector?.customFrequencyPolicy === 'value' ? `${detector.customFrequencyDefinitions.length} profile ranges` : 'Keep detector ranges'}</span>
            </button>
        </nav>
        <div class="min-w-0 space-y-4">
            {#if query}
                <p class="copy-caption" role="status">Matching settings for “{search}” · <button class="link" onclick={() => (search = '')}>Clear search</button></p>
                {#if !visible('everyday') && !visible('frequencies') && !visible('boxes') && !PROFILE_SETTING_GROUPS.some((group) => visible(group.id))}
                    <p>No matching profile setting. Try a band, feature name, lights, or volume.</p>
                {/if}
            {/if}
            <div hidden={!visible('everyday')}>
                {#if detector}
                    {#if editingSettings}
                        <ProfileEverydayControls bind:detector={editedDetector} {editingSettings} {knownFirmware} />
                    {:else}
                        <ProfileEverydayControls {detector} {editingSettings} {knownFirmware} />
                    {/if}
                    {#if detector.userSettings === 'value' && settings.euroMode && detector.modePolicy === 'value' && detector.mode === 3}
                        <StatusAlert fallbackType="warning" message="Advanced Logic (L) cannot be applied with Euro Mode. Choose All Bogeys or Logic, or turn Euro Mode off in Filters & startup." />
                    {/if}
                {/if}
            </div>
            <div hidden={!PROFILE_SETTING_GROUPS.some((group) => visible(group.id)) && !visible('frequencies')} class="space-y-2">
                {#if detector}
                    <label class="field-control">
                        <span class="field-label">Apply detection settings</span>
                        <select class="select w-full" bind:value={detector.userSettings} disabled={!editingSettings}>
                            <option value="value">Use this profile’s detection settings</option>
                            <option value="unchanged">Keep the detector’s detection settings</option>
                        </select>
                    </label>
                    {#if detector.userSettings === 'unchanged'}
                        <p class="surface-note copy-caption">Bands, sensitivity, muting, filtering and custom-frequency enable stay saved here but will not be applied. The range-replacement choice is separate.</p>
                    {/if}
                    <p class="copy-caption">{knownFirmware ? `Last observed V1: ${formatFirmware(knownFirmware)}. Newer features remain editable for other detectors; requirements appear beside them.` : 'No known V1 firmware captured. You can build a profile offline; compatibility is checked when applied.'}</p>
                {/if}
            </div>
            {#each PROFILE_SETTING_GROUPS as group}
                <div hidden={!visible(group.id)}>
                    <details class="surface-collapse" open>
                        <summary class="collapse-title min-h-0 py-3 text-sm font-semibold">{group.title}</summary>
                        <div class="collapse-content space-y-3">
                            <p class="copy-caption">{group.description}</p>
                            <div class="divide-y divide-base-300">
                                {#each group.fields as field}
                                    <div hidden={!!query && !matches(`${group.title} ${group.description}`) && !fieldMatches(field)} class="setting-row">
                                        <div class="min-w-0">
                                            <label class="text-sm font-medium" for={`profile-setting-${field.key}`}>{field.label}</label>
                                            {#if field.description}<p class="copy-caption mt-1">{field.description}</p>{/if}
                                            {#if field.minFirmware && knownFirmware && knownFirmware < field.minFirmware}
                                                <p class="mt-1 text-xs text-warning">Requires V1 {formatFirmware(field.minFirmware)}. This choice will not be applied to the last observed detector.</p>
                                            {/if}
                                            {#if field.key === 'laser'}
                                                <p class="copy-caption mt-1"><a class="link" href="/alp">Manage ALP laser handoff</a></p>
                                            {/if}
                                        </div>
                                        {#if field.options}
                                            <select id={`profile-setting-${field.key}`} aria-label={field.ariaLabel || field.label} class="select select-sm w-32 shrink-0" bind:value={settings[field.key]} disabled={!editingSettings}>
                                                {#each field.options as option}<option value={option.value}>{option.label}</option>{/each}
                                            </select>
                                        {:else}
                                            <input id={`profile-setting-${field.key}`} aria-label={field.ariaLabel || field.label} type="checkbox" class="toggle toggle-primary toggle-sm shrink-0" bind:checked={settings[field.key]} disabled={!editingSettings} />
                                        {/if}
                                    </div>
                                {/each}
                            </div>
                            {#if group.id === 'photo' && settings.photoIntersectionFilter}
                                <StatusAlert fallbackType="warning" message="Intersection Management suppresses DriveSafe 3D, DriveSafe 3DHD, and Ekin alerts while enabled. Those saved settings are not changed." />
                            {/if}
                            {#if group.id === 'filtering' && detector?.userSettings === 'value' && settings.euroMode && detector.modePolicy === 'value' && detector.mode === 3}
                                <StatusAlert fallbackType="warning" message="Euro Mode cannot be applied with Advanced Logic (L). Choose All Bogeys or Logic in Lights, mode & volume." />
                            {/if}
                        </div>
                    </details>
                </div>
            {/each}
            <div hidden={!visible('boxes')}>
                {#if inTheBox}
                    {#if editingSettings}
                        <ProfileInTheBoxControls bind:settings={editedInTheBox} {editingSettings} />
                    {:else}
                        <ProfileInTheBoxControls settings={inTheBox} {editingSettings} />
                    {/if}
                {/if}
            </div>
            <div hidden={!visible('frequencies')}>
                <details class="surface-collapse" open>
                    <summary class="collapse-title min-h-0 py-3 text-sm font-semibold">Custom Frequencies</summary>
                    <div class="collapse-content space-y-3">
                        <label class="field-control pt-2">
                            <span class="field-label">Range replacement</span>
                            <select class="select w-full" bind:value={detector.customFrequencyPolicy} disabled={!editingSettings}>
                                <option value="unchanged">Keep the detector’s current ranges</option>
                                <option value="value">Use this profile’s ranges</option>
                            </select>
                        </label>
                        <p class="copy-caption">Range replacement and custom-frequency detection are separate choices. Switching detection off does not stop profile ranges from being written.</p>
                        <label class="flex items-center justify-between pt-2 text-sm">
                            <span>Enable Custom Frequencies</span>
                            <input
                                type="checkbox"
                                class="toggle toggle-primary toggle-sm"
                                checked={settings.customFreqs}
                                onchange={(event) => {
                                    settings.customFreqs = event.currentTarget.checked;
                                }}
                                disabled={!editingSettings}
                            />
                        </label>
                        <p class="copy-caption">
                            Ranges listed here replace the detector’s ranges for that band when applied. With no ranges for a band, its current detector ranges are kept. You can edit ranges while Custom Frequencies is off.
                        </p>
                        <div class="grid gap-2 text-sm sm:grid-cols-2">
                            <div class="surface-panel space-y-1">
                                <div class="flex items-center justify-between gap-2">
                                    <span class="font-semibold">K ranges</span>
                                    <span class={`badge ${bandDefinitionCount('K') > 0 ? 'badge-primary' : 'badge-ghost'}`}>
                                        {bandDefinitionCount('K') > 0 ? 'Use profile ranges' : 'Keep detector ranges'}
                                    </span>
                                </div>
                                {#if bandDefinitionCount('K') === 0}
                                    <p class="copy-caption">Add a K range to replace the detector’s K ranges.</p>
                                {/if}
                            </div>
                            <div class="surface-panel space-y-1">
                                <div class="flex items-center justify-between gap-2">
                                    <span class="font-semibold">Ka ranges</span>
                                    <span class={`badge ${bandDefinitionCount('Ka') > 0 ? 'badge-primary' : 'badge-ghost'}`}>
                                        {bandDefinitionCount('Ka') > 0 ? 'Use profile ranges' : 'Keep detector ranges'}
                                    </span>
                                </div>
                                {#if bandDefinitionCount('Ka') === 0}
                                    <p class="copy-caption">Add a Ka range to replace the detector’s Ka ranges.</p>
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
                                                            ? `Remove; keep detector ${customFrequencyBand(definition)}`
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
                                This profile keeps the detector’s current ranges. Add a K or Ka range to replace the ranges for that band.
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

            </div>

        </div>
    </div>
    <details class="surface-collapse">
        <summary class="collapse-title min-h-0 py-3 text-sm font-semibold">Feature availability</summary>
        <div class="collapse-content space-y-2 text-sm">
            <p><strong>SAVVY Settings:</strong> Accessory controls are not currently supported.</p>
            <p class="copy-caption">V1Simple alert persistence is available in <a class="link" href="/autopush">Auto-Push</a>. Detector-native alert persistence is not currently configurable here.</p>
        </div>
    </details>
</div>

<style>
    .setting-group { min-width: 0; padding: .65rem .75rem; border: 1px solid var(--app-border-color); border-radius: .55rem; text-align: left; background: var(--color-base-100); cursor: pointer; }
    .setting-group strong { display: block; font-size: .8rem; }
    .setting-group span { display: block; margin-top: .25rem; font-size: .7rem; opacity: .65; line-height: 1.4; }
    .setting-group.selected { border-color: var(--color-primary); background: color-mix(in oklab, var(--color-primary) 10%, var(--color-base-100)); }
    .setting-group:focus-visible { outline: 2px solid var(--color-primary); outline-offset: 2px; }
    .setting-row { display: flex; align-items: center; justify-content: space-between; gap: 1rem; padding: .75rem 0; }
    [hidden] { display: none !important; }
</style>
