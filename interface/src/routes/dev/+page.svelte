<script>
	import { dev } from '$app/environment';
	import { onMount } from 'svelte';

	let DevPage = $state(null);

	onMount(() => {
		if (!dev) return;
		void import('$lib/features/dev/DevPage.svelte').then((module) => {
			DevPage = module.default;
		});
	});
</script>

{#if dev && DevPage}
	<DevPage />
{/if}
