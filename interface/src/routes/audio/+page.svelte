<script>
	import { onMount } from 'svelte';
	import { fetchWithTimeout } from '$lib/utils/poll';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	
	let settings = $state({
		voiceAlertMode: 3,  // 0=disabled, 1=band, 2=freq, 3=band+freq
		voiceDirectionEnabled: true,
		announceBogeyCount: true,
		muteVoiceIfVolZero: false,
		voiceVolume: 75,  // Speaker volume (0-100)
		// Secondary alert settings
		announceSecondaryAlerts: false,
		secondaryLaser: true,
		secondaryKa: true,
		secondaryK: false,
		secondaryX: false,
		// Volume fade settings
		alertVolumeFadeEnabled: false,
		alertVolumeFadeDelaySec: 2,
		alertVolumeFadeVolume: 1,
		// Speed mute settings
		speedMuteEnabled: false,
		speedMuteThresholdMph: 25,
		speedMuteHysteresisMph: 3,
		speedMuteVolume: 0,
		speedMuteVoice: true,
		// Stealth (merged from the former Quiet page)
		stealthEnabled: false,
	});
	
	let loading = $state(true);
	let saving = $state(false);
	let settingsLoaded = $state(false);
	let message = $state(null);
	
	// Voice mode options for dropdown
	const voiceModes = [
		{ value: 0, label: 'Disabled', desc: 'No voice announcements' },
		{ value: 1, label: 'Band Only', desc: '"Ka", "K", "Laser"' },
		{ value: 2, label: 'Frequency Only', desc: '"34.712"' },
		{ value: 3, label: 'Band + Frequency', desc: '"Ka 34.712"' }
	];
	
	onMount(async () => {
		await fetchSettings();
	});
	
	async function fetchSettings() {
		loading = true;
		settingsLoaded = false;
		try {
			const res = await fetchWithTimeout('/api/audio/settings');
			if (!res.ok) {
				message = { type: 'error', text: 'Failed to load settings' };
				return;
			}
			const data = await res.json();
			settings.voiceAlertMode = data.voiceAlertMode ?? 3;
			settings.voiceDirectionEnabled = data.voiceDirectionEnabled ?? true;
			settings.announceBogeyCount = data.announceBogeyCount ?? true;
			settings.muteVoiceIfVolZero = data.muteVoiceIfVolZero ?? false;
			settings.voiceVolume = data.voiceVolume ?? 75;
			// Secondary alert settings
			settings.announceSecondaryAlerts = data.announceSecondaryAlerts ?? false;
			settings.secondaryLaser = data.secondaryLaser ?? true;
			settings.secondaryKa = data.secondaryKa ?? true;
			settings.secondaryK = data.secondaryK ?? false;
			settings.secondaryX = data.secondaryX ?? false;
			// Volume fade settings
			settings.alertVolumeFadeEnabled = data.alertVolumeFadeEnabled ?? false;
			settings.alertVolumeFadeDelaySec = data.alertVolumeFadeDelaySec ?? 2;
			settings.alertVolumeFadeVolume = data.alertVolumeFadeVolume ?? 1;
			// Speed mute settings
			settings.speedMuteEnabled = data.speedMuteEnabled ?? false;
			settings.speedMuteThresholdMph = data.speedMuteThresholdMph ?? 25;
			settings.speedMuteHysteresisMph = data.speedMuteHysteresisMph ?? 3;
			settings.speedMuteVolume = data.speedMuteVolume ?? 0;
			settings.speedMuteVoice = data.speedMuteVoice ?? true;
			settings.stealthEnabled = data.stealthEnabled ?? false;
			settingsLoaded = true;
		} catch (e) {
			message = { type: 'error', text: 'Failed to load settings' };
		} finally {
			loading = false;
		}
	}
	
	async function saveSettings() {
		if (!settingsLoaded) {
			message = { type: 'error', text: 'Failed to load settings' };
			return;
		}
		saving = true;
		message = null;
		
		try {
			const params = new URLSearchParams();
			params.append('voiceAlertMode', settings.voiceAlertMode);
			params.append('voiceDirectionEnabled', settings.voiceDirectionEnabled);
			params.append('announceBogeyCount', settings.announceBogeyCount);
			params.append('muteVoiceIfVolZero', settings.muteVoiceIfVolZero);
			params.append('voiceVolume', settings.voiceVolume);
			// Secondary alert settings
			params.append('announceSecondaryAlerts', settings.announceSecondaryAlerts);
			params.append('secondaryLaser', settings.secondaryLaser);
			params.append('secondaryKa', settings.secondaryKa);
			params.append('secondaryK', settings.secondaryK);
			params.append('secondaryX', settings.secondaryX);
			// Volume fade settings
			params.append('alertVolumeFadeEnabled', settings.alertVolumeFadeEnabled);
			params.append('alertVolumeFadeDelaySec', settings.alertVolumeFadeDelaySec);
			params.append('alertVolumeFadeVolume', settings.alertVolumeFadeVolume);
			// Speed mute settings
			params.append('speedMuteEnabled', settings.speedMuteEnabled);
			params.append('speedMuteThresholdMph', settings.speedMuteThresholdMph);
			params.append('speedMuteHysteresisMph', settings.speedMuteHysteresisMph);
			params.append('speedMuteVolume', settings.speedMuteVolume);
			params.append('speedMuteVoice', settings.speedMuteVoice);
			params.append('stealthEnabled', settings.stealthEnabled);

			const res = await fetchWithTimeout('/api/audio/settings', {
				method: 'POST',
				headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
				body: params
			});
			
			if (res.ok) {
				message = { type: 'success', text: 'Audio settings saved!' };
				await fetchSettings();
			} else {
				message = { type: 'error', text: 'Failed to save settings' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		} finally {
			saving = false;
		}
	}
	
	// Build preview text based on current settings
	function getPreviewText() {
		if (settings.voiceAlertMode === 0) return '(silent)';
		let parts = [];
		if (settings.voiceAlertMode === 1) parts.push('Ka');
		else if (settings.voiceAlertMode === 2) parts.push('34.712');
		else if (settings.voiceAlertMode === 3) parts.push('Ka 34.712');
		if (settings.voiceDirectionEnabled && settings.voiceAlertMode > 0) parts.push('ahead');
		if (settings.announceBogeyCount && settings.voiceAlertMode > 0) parts.push('2 bogeys');
		return `"${parts.join(' ')}"`;
	}
</script>

<div class="page-stack">
	<PageHeader title="Audio &amp; Quiet" subtitle="Voice alerts, speaker, and quiet-driving (fade, speed mute, stealth)" />
	
	<StatusAlert {message} fallbackType="success" />
	<p class="copy-caption-soft">Changes apply on next normal boot.</p>
	
	{#if loading}
		<div class="state-loading">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<!-- Voice Alerts -->
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="Voice Alerts"
					subtitle="Speak alert information through the built-in speaker when no phone app is connected."
				/>
				
				<div class="space-y-4">
					<!-- Voice Content Mode Dropdown -->
					<div class="form-control">
						<label class="label" for="voice-mode">
							<span class="label-text font-medium">Voice Content</span>
						</label>
						<select 
							id="voice-mode"
							class="select select-bordered w-full"
							bind:value={settings.voiceAlertMode}
						>
							{#each voiceModes as mode}
								<option value={mode.value}>{mode.label} - {mode.desc}</option>
							{/each}
						</select>
					</div>
					
					<!-- Direction Toggle -->
					<div class="form-control">
						<label class="label cursor-pointer">
							<div>
								<span class="label-text font-medium">Include Direction</span>
								<p class="copy-caption-soft">Append "ahead", "side", or "behind" to announcement</p>
							</div>
							<input 
								type="checkbox" 
								class="toggle toggle-primary" 
								bind:checked={settings.voiceDirectionEnabled}
								disabled={settings.voiceAlertMode === 0}
							/>
						</label>
					</div>
					
					<!-- Bogey Count Toggle -->
					<div class="form-control">
						<label class="label cursor-pointer">
							<div>
								<span class="label-text font-medium">Announce Bogey Count</span>
								<p class="copy-caption-soft">Append "2 bogeys", "3 bogeys", etc. when multiple alerts active</p>
							</div>
							<input 
								type="checkbox" 
								class="toggle toggle-primary" 
								bind:checked={settings.announceBogeyCount}
								disabled={settings.voiceAlertMode === 0}
							/>
						</label>
					</div>
					
					<!-- Preview -->
					<div class="surface-panel">
						<p class="copy-caption-soft mb-1">Preview:</p>
						<p class="text-lg font-mono">{getPreviewText()}</p>
					</div>
					
					<div class="divider my-2"></div>
					
					<!-- Mute at Vol 0 -->
					<div class="form-control">
						<label class="label cursor-pointer">
							<div>
								<span class="label-text font-medium">Mute Voice at Volume 0</span>
								<p class="copy-caption-soft">Silence alert announcements when V1 volume is 0</p>
								<p class="copy-warning mt-1">Note: "Warning Volume Zero" will still play</p>
							</div>
							<input 
								type="checkbox" 
								class="toggle toggle-primary" 
								bind:checked={settings.muteVoiceIfVolZero}
								disabled={settings.voiceAlertMode === 0}
							/>
						</label>
					</div>
				</div>
			</div>
		</div>
		
		<!-- Secondary Alerts -->
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="Secondary Alert Announcements"
					subtitle="Optionally announce non-priority alerts (lower bars) after priority stabilizes."
				/>
				
				<div class="space-y-4">
					<!-- Master Toggle -->
					<div class="form-control">
						<label class="label cursor-pointer">
							<div>
								<span class="label-text font-medium">Announce Secondary Alerts</span>
								<p class="copy-caption-soft">Speak non-priority alerts once after 1s priority stability + 1.5s gap</p>
							</div>
							<input 
								type="checkbox" 
								class="toggle toggle-primary" 
								bind:checked={settings.announceSecondaryAlerts}
								disabled={settings.voiceAlertMode === 0}
							/>
						</label>
					</div>
					
					<!-- Band Filters (nested, only shown when master enabled) -->
					{#if settings.announceSecondaryAlerts && settings.voiceAlertMode !== 0}
						<div class="surface-subsection tight">
							<p class="copy-caption-soft mb-2">Which bands to announce:</p>
							
							<div class="form-control">
								<label class="label cursor-pointer py-1">
									<span class="label-text">Laser</span>
									<input 
										type="checkbox" 
										class="toggle toggle-sm toggle-primary" 
										bind:checked={settings.secondaryLaser}
									/>
								</label>
							</div>
							
							<div class="form-control">
								<label class="label cursor-pointer py-1">
									<span class="label-text">Ka Band</span>
									<input 
										type="checkbox" 
										class="toggle toggle-sm toggle-primary" 
										bind:checked={settings.secondaryKa}
									/>
								</label>
							</div>
							
							<div class="form-control">
								<label class="label cursor-pointer py-1">
									<span class="label-text">K Band</span>
									<input 
										type="checkbox" 
										class="toggle toggle-sm toggle-primary" 
										bind:checked={settings.secondaryK}
									/>
								</label>
							</div>
							
							<div class="form-control">
								<label class="label cursor-pointer py-1">
									<span class="label-text">X Band</span>
									<input 
										type="checkbox" 
										class="toggle toggle-sm toggle-primary" 
										bind:checked={settings.secondaryX}
									/>
								</label>
							</div>
						</div>
					{/if}
				</div>
			</div>
		</div>
		
		<!-- Volume Fade -->
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="V1 Volume Fade"
					subtitle="Reduce V1 volume after initial alert period (doesn't affect muted alerts)."
				/>
				
				<div class="space-y-4">
					<!-- Master Toggle -->
					<div class="form-control">
						<label class="label cursor-pointer">
							<div>
								<span class="label-text font-medium">Enable Volume Fade</span>
								<p class="copy-caption-soft">Lower V1 volume after delay, restore when alert clears</p>
							</div>
							<input 
								type="checkbox" 
								class="toggle toggle-primary" 
								bind:checked={settings.alertVolumeFadeEnabled}
							/>
						</label>
					</div>
					
					{#if settings.alertVolumeFadeEnabled}
						<div class="surface-subsection">
							<!-- Delay -->
							<div class="form-control">
								<label class="label" for="fade-delay">
									<span class="label-text">Delay (seconds)</span>
									<span class="label-text-alt">{settings.alertVolumeFadeDelaySec}s</span>
								</label>
								<input 
									id="fade-delay"
									type="range" 
									min="1" 
									max="10" 
									bind:value={settings.alertVolumeFadeDelaySec}
									class="range range-primary range-sm" 
								/>
								<p class="copy-caption-soft mt-1">Time at full volume before reducing</p>
							</div>
							
							<!-- Reduced Volume -->
							<div class="form-control">
								<label class="label" for="fade-volume">
									<span class="label-text">Reduced Volume</span>
									<span class="label-text-alt">Level {settings.alertVolumeFadeVolume}</span>
								</label>
								<input 
									id="fade-volume"
									type="range" 
									min="1" 
									max="9" 
									bind:value={settings.alertVolumeFadeVolume}
									class="range range-primary range-sm" 
								/>
								<p class="copy-caption-soft mt-1">V1 volume to fade to (1-9)</p>
							</div>
							
							<!-- Preview -->
							<div class="surface-panel text-sm">
								<p class="copy-subtle">
									Alert starts → <strong>full volume</strong> for {settings.alertVolumeFadeDelaySec}s → 
									fade to <strong>level {settings.alertVolumeFadeVolume}</strong> → 
									alert clears → <strong>restore volume</strong>
								</p>
							</div>
						</div>
					{/if}
				</div>
			</div>
		</div>
		
		<!-- Speed-Aware Muting -->
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="Speed-Aware Muting"
					subtitle="Suppress voice alerts below a speed threshold (requires OBD speed source)."
				/>
				
				<div class="space-y-4">
					<!-- Master Toggle -->
					<div class="form-control">
						<label class="label cursor-pointer">
							<div>
								<span class="label-text font-medium">Enable Speed Mute</span>
								<p class="copy-caption-soft">Suppress voice announcements when driving below the speed threshold</p>
							</div>
							<input 
								type="checkbox" 
								class="toggle toggle-primary" 
								bind:checked={settings.speedMuteEnabled}
							/>
						</label>
					</div>
					
					{#if settings.speedMuteEnabled}
						<div class="surface-subsection">
							<!-- Threshold -->
							<div class="form-control">
								<label class="label" for="speed-mute-threshold">
									<span class="label-text">Mute Below (mph)</span>
									<span class="label-text-alt">{settings.speedMuteThresholdMph} mph</span>
								</label>
								<input 
									id="speed-mute-threshold"
									type="range" 
									min="5" 
									max="60" 
									bind:value={settings.speedMuteThresholdMph}
									class="range range-primary range-sm" 
								/>
								<p class="copy-caption-soft mt-1">Alerts are suppressed below this speed</p>
							</div>
							
							<!-- Hysteresis -->
							<div class="form-control">
								<label class="label" for="speed-mute-hysteresis">
									<span class="label-text">Hysteresis (mph)</span>
									<span class="label-text-alt">{settings.speedMuteHysteresisMph} mph</span>
								</label>
								<input 
									id="speed-mute-hysteresis"
									type="range" 
									min="1" 
									max="10" 
									bind:value={settings.speedMuteHysteresisMph}
									class="range range-primary range-sm" 
								/>
								<p class="copy-caption-soft mt-1">Unmutes at threshold + hysteresis to prevent cycling</p>
							</div>
							
							<!-- V1 Alert Volume -->
							<div class="form-control mt-4">
								<label class="label" for="speed-mute-volume">
									<span class="label-text">V1 Alert Volume</span>
									<span class="label-text-alt">{settings.speedMuteVolume}{settings.speedMuteVolume === 0 ? ' (silent)' : ''}</span>
								</label>
								<input 
									id="speed-mute-volume"
									type="range" 
									min="0" 
									max="9" 
									bind:value={settings.speedMuteVolume}
									class="range range-primary range-sm" 
								/>
								<p class="copy-caption-soft mt-1">V1 hardware volume when speed-muted (0 = silent)</p>
							</div>

							<!-- Voice Suppression Toggle -->
							<div class="form-control mt-4">
								<label class="label cursor-pointer">
									<div>
										<span class="label-text font-medium">Mute Voice Alerts</span>
										<p class="copy-caption-soft">Also suppress voice announcements when speed-muted</p>
									</div>
									<input 
										type="checkbox" 
										class="toggle toggle-primary" 
										bind:checked={settings.speedMuteVoice}
									/>
								</label>
							</div>

							<!-- Preview -->
							<div class="surface-panel text-sm mt-4">
								<p class="copy-subtle">
									Below <strong>{settings.speedMuteThresholdMph} mph</strong> → V1 vol → {settings.speedMuteVolume}{settings.speedMuteVoice ? ' + voice muted' : ''} →
									above <strong>{settings.speedMuteThresholdMph + settings.speedMuteHysteresisMph} mph</strong> → restored
								</p>
							</div>
						</div>
					{/if}
				</div>
			</div>
		</div>

<!-- Stealth Display -->
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="Stealth Display"
					subtitle="Blank the idle screen while still showing alerts normally."
				/>

				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text font-medium">Enable Stealth Mode</span>
							<p class="copy-caption-soft">Blanks the display while idle; alerts still show normally</p>
						</div>
						<input
							type="checkbox"
							class="toggle toggle-primary"
							bind:checked={settings.stealthEnabled}
						/>
					</label>
				</div>
			</div>
		</div>

		<!-- Speaker Volume -->
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="Speaker Volume"
					subtitle="Tune Waveshare ES8311 output level for local voice playback."
				/>
				
				<div class="form-control">
					<label class="label" for="voice-volume-slider">
						<span class="label-text font-medium">Volume Level</span>
						<span class="label-text-alt">{settings.voiceVolume}%</span>
					</label>
					<div class="flex items-center gap-3">
						<span class="text-lg">🔈</span>
						<input 
							id="voice-volume-slider"
							type="range" 
							min="0" 
							max="100" 
							bind:value={settings.voiceVolume}
							class="range range-primary flex-1" 
						/>
						<span class="text-lg">🔊</span>
					</div>
					<p class="copy-caption-soft mt-1">Controls the Waveshare ES8311 DAC output level</p>
				</div>
			</div>
		</div>
		
		<!-- Info Card -->
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead title="How It Works" />
				<ul class="copy-subtle space-y-2 list-disc list-inside">
					<li>Voice alerts only play when <strong>no phone app</strong> is connected via BLE proxy</li>
					<li>New alert: full announcement based on your content settings</li>
					<li>Direction change: direction-only announcement (e.g., "behind") if direction is enabled</li>
					<li>2-second cooldown between announcements to prevent spam</li>
					<li>Quiet driving: volume fade, speed-aware muting, and stealth display reduce distraction</li>
				</ul>
			</div>
		</div>
		
		<!-- Save Button -->
		<button 
			class="btn btn-primary btn-block" 
			onclick={saveSettings}
			disabled={saving || !settingsLoaded}
		>
			{#if saving}
				<span class="loading loading-spinner loading-sm"></span>
			{/if}
			Save Audio Settings
		</button>
	{/if}
</div>
