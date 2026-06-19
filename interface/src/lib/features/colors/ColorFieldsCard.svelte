<script>
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import ColorControl from '$lib/components/ColorControl.svelte';
	import { rgb565ToHex } from '$lib/utils/colors';

	let {
		title,
		subtitle = '',
		fields = [],
		rows = null,
		gridClass = 'grid grid-cols-2 gap-4',
		defaultSwatchSize = 'lg',
		defaultPreviewClass = 'text-2xl font-bold',
		colors,
		onPick,
		onHexChange
	} = $props();

	const fieldRows = $derived(rows ?? [fields]);
</script>

<div class="surface-card">
	<div class="card-body">
		<CardSectionHead {title} {subtitle} />

		<div class="space-y-4">
			{#each fieldRows as row}
				<div class={gridClass}>
					{#each row as field}
						<ColorControl
							id={field.id}
							label={field.label}
							value={colors[field.key]}
							swatchSize={field.swatchSize ?? defaultSwatchSize}
							ariaLabel={field.ariaLabel ?? `${field.label} color`}
							onPick={() => onPick(field.key, field.pickerLabel)}
							onHexChange={(value) => onHexChange(field.key, value)}
						>
							<span
								class={field.previewClass ?? defaultPreviewClass}
								style={`color: ${rgb565ToHex(colors[field.key])}`}
							>{field.preview}</span>
						</ColorControl>
					{/each}
				</div>
			{/each}
		</div>
	</div>
</div>
