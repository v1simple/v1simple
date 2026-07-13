<script>
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import ColorControl from '$lib/components/ColorControl.svelte';
	import ToggleSetting from '$lib/components/ToggleSetting.svelte';
	import { rgb565ToHex } from '$lib/utils/colors';

	let {
		colors,
		rows,
		toggles,
		onPick,
		onHexChange,
		onToggle
	} = $props();
</script>

<div class="surface-card">
	<div class="card-body">
		<CardSectionHead title="Status Indicators" />

		<div class="space-y-4">
			{#each rows as row}
				<div class="grid grid-cols-2 gap-4">
					{#each row as field}
						<ColorControl
							id={field.id}
							label={field.label}
							value={colors[field.key]}
							swatchSize="lg"
							ariaLabel={`${field.label} color`}
							onPick={() => onPick(field.key, field.pickerLabel)}
							onHexChange={(value) => onHexChange(field.key, value)}
						>
							<span class="text-2xl font-bold" style={`color: ${rgb565ToHex(colors[field.key])}`}>{field.preview}</span>
						</ColorControl>
					{/each}
				</div>
			{/each}
		</div>

		<div class="divider my-2"></div>

		<div class="grid grid-cols-1 xl:grid-cols-2 gap-3">
			{#each toggles as toggle}
				<ToggleSetting
					title={toggle.title}
					description={toggle.description}
					checked={colors[toggle.key]}
					disabled={toggle.disabled?.(colors) ?? false}
					onChange={(checked) => onToggle(toggle.key, checked)}
				/>
			{/each}
		</div>
	</div>
</div>
