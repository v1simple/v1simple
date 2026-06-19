<script>
	let { title, subtitle = '', children } = $props();
</script>

<div class="section-head">
	<div>
		<h2 class="card-title">{title}</h2>
		{#if subtitle}
			<p class="copy-subtle">{subtitle}</p>
		{/if}
	</div>
	{#if children}
		{@render children()}
	{/if}
</div>
