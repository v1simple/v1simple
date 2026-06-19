<script>
	import { onMount } from 'svelte';
	import { fetchWithTimeout } from '$lib/utils/poll';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import ColorControl from '$lib/components/ColorControl.svelte';
	import ColorFieldsCard from '$lib/features/colors/ColorFieldsCard.svelte';
	import ColorPickerModal from '$lib/components/ColorPickerModal.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusIndicatorsCard from '$lib/features/colors/StatusIndicatorsCard.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	import ToggleSetting from '$lib/components/ToggleSetting.svelte';
	import {
		buildColorParams,
		cloneDefaultColors,
		normalizeColorPayload,
		parseColorInput,
		rgb565ToHex,
		rgb565ToRgb888,
		rgb888ToRgb565
	} from '$lib/utils/colors';

	import {
		ALP_BADGE_FIELDS,
		ARROW_FIELDS,
		BADGE_FIELDS,
		BAND_FIELDS,
		DISPLAY_PREVIEW_CLEAR_ENDPOINT,
		DISPLAY_PREVIEW_ENDPOINT,
		DISPLAY_SETTINGS_ENDPOINT,
		DISPLAY_SETTINGS_RESET_ENDPOINT,
		QUIET_SETTINGS_ENDPOINT,
		SIGNAL_BARS,
		STATUS_FIELD_ROWS,
		VISIBILITY_TOGGLES
	} from '$lib/features/colors/colorsPageConfig';

	let colors = $state(cloneDefaultColors());
	let stealthEnabled = $state(false);
	let loading = $state(true);
	let saving = $state(false);
	let message = $state(null);
	let stealthSaveRequestId = 0;

	let pickerOpen = $state(false);
	let pickerKey = $state(null);
	let pickerLabel = $state('');
	let pickerR = $state(0);
	let pickerG = $state(0);
	let pickerB = $state(0);

	onMount(async () => {
		await fetchColors();
	});

	function openPicker(key, label) {
		pickerKey = key;
		pickerLabel = label;
		const rgb = rgb565ToRgb888(colors[key]);
		pickerR = rgb.red;
		pickerG = rgb.green;
		pickerB = rgb.blue;
		pickerOpen = true;
	}

	function applyPickerColor() {
		if (pickerKey) {
			colors[pickerKey] = rgb888ToRgb565(pickerR, pickerG, pickerB);
		}
		pickerOpen = false;
	}

	function cancelPicker() {
		pickerOpen = false;
	}

	async function fetchColors() {
		loading = true;
		try {
			const res = await fetchWithTimeout(DISPLAY_SETTINGS_ENDPOINT);
			if (res.ok) {
				const data = await res.json();
				colors = normalizeColorPayload(data, colors);
			}
			const quietRes = await fetchWithTimeout(QUIET_SETTINGS_ENDPOINT);
			if (quietRes.ok) {
				const quietData = await quietRes.json();
				stealthEnabled = quietData.stealthEnabled ?? false;
			}
		} catch (_) {
			message = { type: 'error', text: 'Failed to load colors' };
		} finally {
			loading = false;
		}
	}

	function handleHexInput(key, value) {
		const parsed = parseColorInput(value);
		if (parsed !== null) {
			colors[key] = parsed;
		}
	}

	async function saveColors() {
		saving = true;
		message = null;
		try {
			const res = await fetchWithTimeout(DISPLAY_SETTINGS_ENDPOINT, {
				method: 'POST',
				headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
				body: buildColorParams(colors)
			});
			if (!res.ok) {
				message = { type: 'error', text: 'Failed to save colors' };
				return;
			}
			message = { type: 'success', text: 'Colors saved! Previewing on display...' };
			await fetchColors();
			setTimeout(() => {
				fetchWithTimeout(DISPLAY_PREVIEW_CLEAR_ENDPOINT, { method: 'POST' }).catch((error) => {
					console.warn('Failed to clear display color preview', error);
				});
			}, 3000);
		} catch (_) {
			message = { type: 'error', text: 'Connection error' };
		} finally {
			saving = false;
		}
	}

	async function testColors() {
		try {
			await fetchWithTimeout(DISPLAY_PREVIEW_ENDPOINT, { method: 'POST' });
		} catch (_) {
			// Ignore preview failures.
		}
	}

	async function resetDefaults() {
		if (saving) return;
		if (!confirm('Reset all colors to defaults?')) return;
		saving = true;
		try {
			const res = await fetchWithTimeout(DISPLAY_SETTINGS_RESET_ENDPOINT, { method: 'POST' });
			if (!res.ok) {
				message = { type: 'error', text: 'Failed to reset' };
				return;
			}
			colors = cloneDefaultColors();
			message = { type: 'success', text: 'Colors reset to defaults!' };
			await fetchColors();
		} catch (_) {
			message = { type: 'error', text: 'Failed to reset' };
		} finally {
			saving = false;
		}
	}

	function updateToggle(key, checked) {
		colors[key] = checked;
	}

	async function saveStealthEnabled(checked) {
		const previous = stealthEnabled;
		const requestId = ++stealthSaveRequestId;
		stealthEnabled = checked;
		message = null;

		try {
			const body = new URLSearchParams();
			body.set('stealthEnabled', String(checked));
			const res = await fetchWithTimeout(QUIET_SETTINGS_ENDPOINT, {
				method: 'POST',
				headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
				body
			});
			if (!res.ok) {
				throw new Error('Failed to save stealth setting');
			}
			if (requestId === stealthSaveRequestId) {
				message = { type: 'success', text: 'Stealth setting saved' };
			}
		} catch (_) {
			if (requestId === stealthSaveRequestId) {
				stealthEnabled = previous;
				message = { type: 'error', text: 'Failed to save stealth setting' };
			}
		}
	}
</script>

<div class="page-stack">
	<PageHeader title="Display Colors" subtitle="Customize alert colors" />

	<StatusAlert {message} fallbackType="success" dismiss={() => (message = null)} />

	{#if loading}
		<div class="state-loading">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead title="Counter & Frequency" />
				<div class="grid grid-cols-2 gap-4">
					<ColorControl
						id="bogey-color"
						label="Bogey Counter"
						value={colors.bogey}
						swatchSize="lg"
						inputClass="input input-sm w-20 font-mono text-xs"
						ariaLabel="Bogey counter color"
						onPick={() => openPicker('bogey', 'Bogey Counter')}
						onHexChange={(value) => handleHexInput('bogey', value)}
					>
						<span class="text-2xl font-bold font-mono" style={`color: ${rgb565ToHex(colors.bogey)}`}>1.</span>
					</ColorControl>

					<div class="space-y-2">
						<ColorControl
							id="freq-color"
							label="Frequency Display"
							value={colors.freq}
							swatchSize="lg"
							inputClass="input input-sm w-20 font-mono text-xs"
							ariaLabel="Frequency display color"
							disabled={colors.freqUseBandColor}
							onPick={() => openPicker('freq', 'Frequency Display')}
							onHexChange={(value) => handleHexInput('freq', value)}
						>
							<span
								class="text-2xl font-bold font-mono"
								class:opacity-50={colors.freqUseBandColor}
								style={`color: ${rgb565ToHex(colors.freq)}`}
							>
								35.5
							</span>
						</ColorControl>
						<label class="label cursor-pointer justify-start gap-2 mt-1">
							<input
								type="checkbox"
								class="toggle toggle-sm toggle-primary"
								bind:checked={colors.freqUseBandColor}
							/>
							<span class="field-label-inline">Use band color for frequency</span>
						</label>
					</div>
				</div>

				<div class="divider my-2"></div>

				<div class="grid grid-cols-1 md:grid-cols-2 gap-4">
					<ColorControl
						id="muted-color"
						label="Muted Alert Color"
						labelHint="When alert is muted"
						value={colors.muted}
						swatchSize="lg"
						inputClass="input input-sm w-20 font-mono text-xs"
						ariaLabel="Muted alert color"
						onPick={() => openPicker('muted', 'Muted Alert')}
						onHexChange={(value) => handleHexInput('muted', value)}
					>
						<span class="text-2xl font-bold font-mono" style={`color: ${rgb565ToHex(colors.muted)}`}>35.5</span>
						<span class="copy-muted">(muted)</span>
					</ColorControl>

					<ColorControl
						id="persisted-color"
						label="Persisted Alert Color"
						labelHint="Ghost alert after V1 clears"
						value={colors.persisted}
						swatchSize="lg"
						inputClass="input input-sm w-20 font-mono text-xs"
						ariaLabel="Persisted alert color"
						onPick={() => openPicker('persisted', 'Persisted Alert')}
						onHexChange={(value) => handleHexInput('persisted', value)}
					>
						<span class="text-2xl font-bold font-mono" style={`color: ${rgb565ToHex(colors.persisted)}`}>35.5</span>
						<span class="copy-muted">(persisted)</span>
					</ColorControl>
				</div>

				<div class="divider my-2"></div>
				<h3 class="copy-subheading mt-2">Volume Indicator</h3>
				<div class="grid grid-cols-2 gap-4">
					<ColorControl
						id="volumeMain-color"
						label="Main Volume"
						value={colors.volumeMain}
						swatchSize="sm"
						ariaLabel="Main volume color"
						onPick={() => openPicker('volumeMain', 'Main Volume')}
						onHexChange={(value) => handleHexInput('volumeMain', value)}
					>
						<span class="text-lg font-bold font-mono" style={`color: ${rgb565ToHex(colors.volumeMain)}`}>5V</span>
					</ColorControl>

					<ColorControl
						id="volumeMute-color"
						label="Mute Volume"
						value={colors.volumeMute}
						swatchSize="sm"
						ariaLabel="Mute volume color"
						onPick={() => openPicker('volumeMute', 'Mute Volume')}
						onHexChange={(value) => handleHexInput('volumeMute', value)}
					>
						<span class="text-lg font-bold font-mono" style={`color: ${rgb565ToHex(colors.volumeMute)}`}>0M</span>
					</ColorControl>
				</div>

				<div class="divider my-2"></div>
				<h3 class="copy-subheading mt-2">RSSI Labels</h3>
				<p class="copy-subtle mb-2">Colors for V1 and Proxy connection strength labels</p>
				<div class="grid grid-cols-2 gap-4">
					<ColorControl
						id="rssiV1-color"
						label="V1 RSSI (V)"
						value={colors.rssiV1}
						swatchSize="sm"
						ariaLabel="V1 RSSI label color"
						onPick={() => openPicker('rssiV1', 'V1 RSSI Label')}
						onHexChange={(value) => handleHexInput('rssiV1', value)}
					>
						<span class="text-lg font-bold font-mono" style={`color: ${rgb565ToHex(colors.rssiV1)}`}>V</span>
						<span class="text-lg font-mono text-success">-55</span>
					</ColorControl>

					<ColorControl
						id="rssiProxy-color"
						label="Proxy RSSI (P)"
						value={colors.rssiProxy}
						swatchSize="sm"
						ariaLabel="Proxy RSSI label color"
						onPick={() => openPicker('rssiProxy', 'Proxy RSSI Label')}
						onHexChange={(value) => handleHexInput('rssiProxy', value)}
					>
						<span class="text-lg font-bold font-mono" style={`color: ${rgb565ToHex(colors.rssiProxy)}`}>P</span>
						<span class="text-lg font-mono text-success">-62</span>
					</ColorControl>
				</div>
			</div>
		</div>

		<ColorFieldsCard
			title="Band Indicators"
			fields={BAND_FIELDS}
			gridClass="grid grid-cols-2 md:grid-cols-4 gap-4"
			{colors}
			onPick={openPicker}
			onHexChange={handleHexInput}
		/>

		<ColorFieldsCard
			title="Direction Arrows"
			fields={ARROW_FIELDS}
			gridClass="grid grid-cols-3 gap-4"
			defaultSwatchSize="md"
			{colors}
			onPick={openPicker}
			onHexChange={handleHexInput}
		/>

		<ColorFieldsCard
			title="OBD"
			subtitle="OBD status text color."
			fields={BADGE_FIELDS}
			gridClass="grid grid-cols-1 md:grid-cols-3 gap-4"
			defaultSwatchSize="md"
			{colors}
			onPick={openPicker}
			onHexChange={handleHexInput}
		/>

		<ColorFieldsCard
			title="ALP"
			subtitle="Laser jammer badge colors — matches control pad LED states."
			fields={ALP_BADGE_FIELDS}
			gridClass="grid grid-cols-2 md:grid-cols-4 gap-4"
			defaultSwatchSize="md"
			{colors}
			onPick={openPicker}
			onHexChange={handleHexInput}
		/>

		<StatusIndicatorsCard
			colors={colors}
			rows={STATUS_FIELD_ROWS}
			toggles={VISIBILITY_TOGGLES}
			onPick={openPicker}
			onHexChange={handleHexInput}
			onToggle={updateToggle}
		/>

		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="Stealth Mode"
					subtitle="Blank screen with OBD speed when idle. Alerts display normally, then return to stealth."
				/>
				<ToggleSetting
					title="Enable Stealth Mode"
					description="Double-press BOOT to toggle on/off. Shows -- if OBD speed unavailable."
					checked={stealthEnabled}
					onChange={saveStealthEnabled}
				/>
			</div>
		</div>
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="Display Brightness"
					subtitle="Adjust the screen backlight level."
				/>
				<div class="field-control">
					<div class="flex items-center gap-4">
						<label for="brightness-slider" class="copy-subtle">🌑</label>
						<input
							id="brightness-slider"
							type="range"
							min="1"
							max="255"
							bind:value={colors.brightness}
							class="range range-primary flex-1"
						/>
						<span class="copy-subtle">☀️</span>
						<span class="text-sm font-mono w-12 text-right">{Math.round((colors.brightness / 255) * 100)}%</span>
					</div>
				</div>
			</div>
		</div>

		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="Signal Bars"
					subtitle="Bar 1 = weakest, Bar 6 = strongest."
				/>
				<div class="grid grid-cols-3 md:grid-cols-6 gap-2">
					{#each SIGNAL_BARS as barNum}
						{@const barKey = `bar${barNum}`}
						<ColorControl
							id={`bar-${barNum}-color`}
							label={`Bar ${barNum}`}
							value={colors[barKey]}
							swatchSize="sm"
							inputClass="input input-xs w-14 font-mono text-xs text-center"
							ariaLabel={`Signal bar ${barNum} color`}
							onPick={() => openPicker(barKey, `Signal Bar ${barNum}`)}
							onHexChange={(value) => handleHexInput(barKey, value)}
						>
							<div class="w-8 h-3 rounded" style={`background-color: ${rgb565ToHex(colors[barKey])}`}></div>
						</ColorControl>
					{/each}
				</div>
				<div class="flex justify-center mt-3">
					<div class="flex flex-col-reverse gap-1">
						{#each SIGNAL_BARS as barNum}
							<div class="w-12 h-2 rounded" style={`background-color: ${rgb565ToHex(colors[`bar${barNum}`])}`}></div>
						{/each}
					</div>
				</div>
			</div>
		</div>

		<div class="flex gap-3">
			<button class="btn btn-primary flex-1" onclick={saveColors} disabled={saving}>
				{#if saving}
					<span class="loading loading-spinner loading-sm"></span>
				{:else}
					Save Colors
				{/if}
			</button>
			<button class="btn btn-secondary" onclick={testColors} disabled={saving}>Preview</button>
			<button class="btn btn-outline" onclick={resetDefaults} disabled={saving}>Reset Defaults</button>
		</div>

		<div class="copy-micro text-center">
			Colors use RGB565 format. Save triggers a preview on the display.
		</div>
	{/if}

	<ColorPickerModal
		open={pickerOpen}
		label={pickerLabel}
		bind:red={pickerR}
		bind:green={pickerG}
		bind:blue={pickerB}
		oncancel={cancelPicker}
		onapply={applyPickerColor}
	/>
</div>
