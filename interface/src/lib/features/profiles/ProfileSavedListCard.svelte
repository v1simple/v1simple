<script>
    import CardSectionHead from '$lib/components/CardSectionHead.svelte';

    let { loading, profiles = [], allowEdit = true, currentName = '', compact = false, oneditProfile, oncopyProfile, ondeleteProfile } = $props();
</script>

<div class="surface-card">
    {#key currentName}
    <details open={!compact}>
        <summary class="cursor-pointer p-4" hidden={!compact}>
            <span class="font-semibold">Browse saved profiles ({profiles.length})</span>
            <span class="copy-caption ml-2">{currentName ? `Selected: ${currentName}` : 'New profile draft'}</span>
        </summary>
    <div class="card-body">
        <CardSectionHead
            title="Saved Profiles"
            subtitle="Choose a profile to review or edit. Copy one to use it as a starting point."
        />

        {#if loading}
            <div class="state-loading compact">
                <span class="loading loading-spinner"></span>
            </div>
        {:else if profiles.length === 0}
            <p class="state-empty">
                No saved profiles. Create one offline, then assign it to an Auto-Push slot.
            </p>
        {:else}
            <div class="space-y-2">
                {#each profiles as profile}
                    <div class="surface-panel flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between" class:border-primary={profile.name === currentName}>
                        <div class="min-w-0">
                            <div class="flex flex-wrap items-center gap-2">
                                <span class="font-medium break-words">{profile.name}</span>
                                {#if profile.name === currentName}<span class="badge badge-outline badge-sm">Selected</span>{/if}
                            </div>
                            <div class="copy-caption">
                                {profile.description || 'No description'}
                            </div>
                        </div>
                        <div class="flex shrink-0 gap-2">
                            {#if allowEdit}
                                <button
                                    class="btn btn-secondary btn-xs"
                                    onclick={() => oneditProfile(profile.name)}
                                >
                                    Edit
                                </button>
                                <button
                                    class="btn btn-secondary btn-xs"
                                    onclick={() => oncopyProfile(profile.name)}
                                >
                                    Copy
                                </button>
                            {/if}
                            <button
                                class="btn btn-outline btn-error btn-xs"
                                onclick={() => ondeleteProfile(profile.name)}
                            >
                                Delete
                            </button>
                        </div>
                    </div>
                {/each}
            </div>
        {/if}
    </div>
    </details>
    {/key}
</div>
