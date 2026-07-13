<script>
	let { title, subtitle = '', eyebrow = 'V1 SIMPLE', children } = $props();
</script>

<div class="page-header">
	<div class="page-header-copy">
		{#if eyebrow}
			<p class="page-eyebrow">{eyebrow}</p>
		{/if}
		<h1 class="page-title">{title}</h1>
		{#if subtitle}
			<p class="page-subtitle">{subtitle}</p>
		{/if}
	</div>
	{#if children}
		<div class="page-header-actions">
			{@render children()}
		</div>
	{/if}
</div>
