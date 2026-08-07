<script>
    import PageHeader from '$lib/components/PageHeader.svelte';
    import SettingsGpsCard from '$lib/features/settings/SettingsGpsCard.svelte';
</script>

<div class="page-stack">
    <PageHeader title="GPS" subtitle="GPS module and UTC logging settings for normal operation." />

    <SettingsGpsCard />
</div>
