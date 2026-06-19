<script>
	import { rgb565ToHex, rgb565ToHexStr } from '$lib/utils/colors';

	let {
		id,
		label,
		labelHint = '',
		value = 0,
		ariaLabel = label,
		swatchSize = 'md',
		inputClass = 'input input-xs w-16 font-mono text-xs',
		disabled = false,
		placeholder = 'F800',
		onPick = null,
		onHexChange = null,
		children
	} = $props();

	const swatchClassBySize = {
		sm: 'color-swatch-btn sm',
		md: 'color-swatch-btn md',
		lg: 'color-swatch-btn lg'
	};

	function handleHexChange(event) {
		onHexChange?.(event.currentTarget.value);
	}
</script>

<div class="field-control">
	<label class="label" for={id}>
		<span class="field-label">{label}</span>
		{#if labelHint}
			<span class="field-hint copy-micro">{labelHint}</span>
		{/if}
	</label>
	<div class="flex items-center gap-2">
		<button
			{id}
			type="button"
			aria-label={ariaLabel}
			class={swatchClassBySize[swatchSize] ?? swatchClassBySize.md}
			style={`background-color: ${rgb565ToHex(value)}`}
			disabled={disabled}
			onclick={onPick}
		></button>
		<input
			type="text"
			class={inputClass}
			value={rgb565ToHexStr(value)}
			title="RGB565 hex (or RGB888)"
			{placeholder}
			{disabled}
			onchange={handleHexChange}
		/>
		{#if children}
			<div class="flex items-center gap-1">
				{@render children()}
			</div>
		{/if}
	</div>
</div>
