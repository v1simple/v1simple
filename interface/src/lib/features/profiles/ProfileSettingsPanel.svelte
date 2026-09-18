<script>
    import CardSectionHead from '$lib/components/CardSectionHead.svelte';
    import StatusAlert from '$lib/components/StatusAlert.svelte';
    import ProfileSettingsSections from '$lib/features/profiles/ProfileSettingsSections.svelte';

    let {
        editingSettings,
        currentProfile,
        savingProfile = null,
        editedSettings = $bindable(null),
        editedDetector = $bindable(null),
        editDescription = $bindable(''),
        frequencyError = null,
        oncancelEditing,
        onsaveEditedProfile,
        oncreateNewProfile,
        onstartEditing,
        oncustomFrequenciesChange,
        onaddCustomFrequencyRange,
        onremoveCustomFrequencyRange,
        onresetDraft,
        onshowSaveDialog
    } = $props();
</script>

<div class="surface-card">
    <div class="card-body">
        <CardSectionHead
            title="Profile Editor"
            subtitle="Build detector settings offline, then assign the saved profile to an Auto-Push slot."
        />
        {#if editingSettings}
            <StatusAlert
                message={{
                    type: 'info',
                    text: currentProfile?.name
                        ? `Editing profile: ${currentProfile.name}`
                        : 'Creating new offline profile'
                }}
            />
            <div class="field-control max-w-md">
                <label class="label" for="edit-description">
                    <span class="field-label">Description</span>
                </label>
                <input
                    id="edit-description"
                    type="text"
                    placeholder="Update description"
                    class="input w-full input-sm"
                    maxlength="4096"
                    bind:value={editDescription}
                />
            </div>
        {/if}
        {#if currentProfile && currentProfile.settings}
            <ProfileSettingsSections
                {editingSettings}
                {currentProfile}
                bind:editedSettings
                bind:editedDetector
                {frequencyError}
                {oncustomFrequenciesChange}
                {onaddCustomFrequencyRange}
                {onremoveCustomFrequencyRange}
            />
            <p class="copy-caption mt-3">
                Bluetooth indicator control is independent only while the main display is off and requires supported firmware to keep it on. Volume feedback and disconnect behavior are sent as command policy, but the detector protocol provides no readback for those two policy bits.
            </p>
            {#if editingSettings}
                <div class="mt-3 flex flex-wrap gap-2">
                    <button class="btn btn-outline btn-sm" type="button" onclick={onresetDraft}>
                        Reset this draft to local defaults
                    </button>
                </div>
                <p class="copy-caption mt-2">
                    Local defaults only edit this draft. Detector factory reset is the separately confirmed workflow attached to the captured V1 above.
                </p>
            {/if}
        {:else}
            <p class="state-empty">Select a saved profile to edit, or create a new one.</p>
        {/if}
        <div class="card-actions justify-end gap-2">
            {#if editingSettings}
                <button class="btn btn-ghost btn-sm" onclick={oncancelEditing}> Cancel </button>
                {#if currentProfile?.name}
                    <button class="btn btn-primary btn-sm" onclick={onsaveEditedProfile} disabled={!!savingProfile}>
                        {savingProfile ? `Saving ${savingProfile}...` : 'Save Profile'}
                    </button>
                {:else}
                    <button class="btn btn-primary btn-sm" onclick={onshowSaveDialog}>
                        Save as Profile
                    </button>
                {/if}
            {:else}
                <button class="btn btn-secondary btn-sm" onclick={oncreateNewProfile}>
                    New Profile
                </button>
                {#if currentProfile && currentProfile.settings}
                    <button class="btn btn-secondary btn-sm" onclick={onstartEditing}>
                        Edit
                    </button>
                    <button class="btn btn-sm btn-success" onclick={onshowSaveDialog}>
                        Save
                    </button>
                {/if}
            {/if}
        </div>
    </div>
</div>
