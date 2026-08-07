<script>
    import PageHeader from '$lib/components/PageHeader.svelte';
    import SettingsObdCard from '$lib/features/settings/SettingsObdCard.svelte';
</script>

<div class="page-stack">
    <PageHeader
        title="OBD"
        subtitle="OBD-II speed settings, saved adapter management, and pairing instructions."
    />

    <SettingsObdCard />
</div>
