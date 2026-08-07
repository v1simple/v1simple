<script>
    import CardSectionHead from '$lib/components/CardSectionHead.svelte';

    let { loading, profiles = [], oneditProfile, ondeleteProfile } = $props();
</script>

<div class="surface-card">
    <div class="card-body">
        <CardSectionHead
            title="Saved Profiles"
            subtitle="Named detector configurations available to Auto-Push during normal operation."
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
                    <div class="surface-panel flex items-center justify-between">
                        <div>
                            <div class="font-medium">{profile.name}</div>
                            <div class="copy-caption">
                                {profile.description || 'No description'}
                            </div>
                        </div>
                        <div class="flex gap-2">
                            <button
                                class="btn btn-secondary btn-xs"
                                onclick={() => oneditProfile(profile.name)}
                            >
                                Edit
                            </button>
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
</div>
