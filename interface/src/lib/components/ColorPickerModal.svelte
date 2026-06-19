<script>
	import { COLOR_PICKER_PRESETS, rgbComponentsToHex } from '$lib/utils/colors';

	let {
		open = false,
		label = '',
		red = $bindable(),
		green = $bindable(),
		blue = $bindable(),
		oncancel,
		onapply
	} = $props();
</script>

{#if open}
	<div class="modal modal-open">
		<div class="modal-box surface-modal">
			<h3 class="font-bold text-lg mb-4">{label}</h3>

			<div class="surface-color-preview" style={`background-color: ${rgbComponentsToHex(red, green, blue)}`}></div>

			<div class="space-y-4">
				<div class="field-control">
					<label class="label" for="picker-red">
						<span class="field-label font-semibold text-error">Red</span>
						<span class="field-hint font-mono">{red}</span>
					</label>
					<input
						id="picker-red"
						type="range"
						min="0"
						max="248"
						step="8"
						bind:value={red}
						class="range range-error"
					/>
				</div>

				<div class="field-control">
					<label class="label" for="picker-green">
						<span class="field-label font-semibold text-success">Green</span>
						<span class="field-hint font-mono">{green}</span>
					</label>
					<input
						id="picker-green"
						type="range"
						min="0"
						max="252"
						step="4"
						bind:value={green}
						class="range range-success"
					/>
				</div>

				<div class="field-control">
					<label class="label" for="picker-blue">
						<span class="field-label font-semibold text-info">Blue</span>
						<span class="field-hint font-mono">{blue}</span>
					</label>
					<input
						id="picker-blue"
						type="range"
						min="0"
						max="248"
						step="8"
						bind:value={blue}
						class="range range-info"
					/>
				</div>
			</div>

			<div class="mt-4">
				<span class="copy-muted">Quick colors:</span>
				<div class="flex gap-2 mt-2 flex-wrap">
					{#each COLOR_PICKER_PRESETS as preset}
						<button
							class={preset.buttonClass ?? 'btn btn-sm'}
							style={`background-color: ${preset.hex}`}
							onclick={() => {
								red = preset.red;
								green = preset.green;
								blue = preset.blue;
							}}
						>
							{preset.label}
						</button>
					{/each}
				</div>
			</div>

			<div class="modal-action">
				<button class="btn btn-ghost" onclick={oncancel}>Cancel</button>
				<button class="btn btn-primary" onclick={onapply}>Apply</button>
			</div>
		</div>
		<div
			class="modal-backdrop"
			onclick={oncancel}
			onkeydown={(event) => {
				if (event.key === 'Enter' || event.key === ' ') {
					event.preventDefault();
					oncancel();
				}
			}}
			role="button"
			tabindex="0"
			aria-label="Close color picker"
		></div>
	</div>
{/if}
