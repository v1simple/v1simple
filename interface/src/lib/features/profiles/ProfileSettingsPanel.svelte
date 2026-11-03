<script>
    import CardSectionHead from '#lib/components/CardSectionHead.svelte';
    import ProfileSettingsSections from '#lib/features/profiles/ProfileSettingsSections.svelte';

    let {
        authoring,
        capturedSnapshot = null,
        canApply = false,
        applyDisabled = true,
        onapply,
        mutationBusy = false,
        frequencyError = null,
        oncancelEditing,
        onsaveEditedProfile,
        oncreateNewProfile,
        onstartEditing,
        onaddCustomFrequencyRange,
        onremoveCustomFrequencyRange,
        onresetDraft,
        onshowSaveDialog
    } = $props();
</script>

<div class="surface-card" id="profile-editor" tabindex="-1">
    <div class="card-body">
        <CardSectionHead
            title={authoring.acceptedProfile?.name ? `Profile: ${authoring.acceptedProfile.name}` : 'Profile Editor'}
            subtitle="Save your choices here. Apply the saved profile to your V1, or assign it to Auto-Push."
        />
        {#if authoring.draft}
            <p class="copy-caption" role="status">
                {authoring.acceptedProfile?.name ? `Editing profile: ${authoring.acceptedProfile.name}` : 'Creating new offline profile'}
            </p>
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
                    bind:value={authoring.draft.description}
                />
            </div>
        {/if}
        <div class="space-y-2 border-y border-base-300 py-3">
            <div class="flex flex-wrap items-center justify-end gap-2">
                {#if authoring.draft}
                    <button class="btn btn-ghost btn-sm" onclick={oncancelEditing}>Cancel</button>
                    {#if authoring.acceptedProfile?.name}
                        <button class="btn btn-primary btn-sm" onclick={onsaveEditedProfile} disabled={mutationBusy || !!authoring.savingProfile}>
                            {authoring.savingProfile ? `Saving ${authoring.savingProfile}...` : 'Save Profile'}
                        </button>
                    {:else}
                        <button class="btn btn-primary btn-sm" onclick={onshowSaveDialog}>Save as Profile</button>
                    {/if}
                {:else if authoring.acceptedProfile?.settings}
                    <button class="btn btn-secondary btn-sm" onclick={onstartEditing}>Edit</button>
                    <button class="btn btn-sm btn-success" onclick={onshowSaveDialog}>Save under a name…</button>
                {:else}
                    <button class="btn btn-primary btn-sm" onclick={oncreateNewProfile}>New Profile</button>
                {/if}
                {#if canApply}
                    <button class="btn btn-outline btn-sm" disabled={applyDisabled} onclick={onapply}>Apply saved profile to V1</button>
                {/if}
            </div>
            <p class="copy-caption">
                {#if canApply}
                    Save stores your edits. Apply uses the saved profile and briefly restarts V1Simple to connect to your captured detector.
                {:else if authoring.draft}
                    Save this profile to make it available for Apply and Auto-Push.
                {:else}
                    Choose a saved profile or create one to get started.
                {/if}
            </p>
        </div>
        {#if authoring.acceptedProfile && authoring.acceptedProfile.settings}
            <ProfileSettingsSections
                editingSettings={!!authoring.draft}
                currentProfile={authoring.acceptedProfile}
                {capturedSnapshot}
                bind:editedSettings={() => authoring.draft?.settings, (value) => { if (authoring.draft) authoring.draft.settings = value; }}
                bind:editedDetector={() => authoring.draft?.detector, (value) => { if (authoring.draft) authoring.draft.detector = value; }}
                bind:editedInTheBox={() => authoring.draft?.inTheBox, (value) => { if (authoring.draft) authoring.draft.inTheBox = value; }}
                {frequencyError}
                {onaddCustomFrequencyRange}
                {onremoveCustomFrequencyRange}
            />
            {#if authoring.draft}
                <details class="mt-3">
                    <summary class="cursor-pointer copy-caption">Reset this draft</summary>
                    <p class="copy-caption my-2">Replace the choices in this editor with V1Simple defaults. Save the profile to keep them.</p>
                    <button class="btn btn-outline btn-sm" type="button" onclick={onresetDraft}>
                        Reset this draft to local defaults
                    </button>
                </details>
            {/if}
        {:else}
            <p class="state-empty">Select a saved profile to edit, or create a new one.</p>
        {/if}
    </div>
</div>
