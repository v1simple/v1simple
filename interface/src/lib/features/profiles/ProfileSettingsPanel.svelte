<script>
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';

	let {
		v1Connected,
		isMaintenance = false,
		editingSettings,
		currentProfile,
		editedSettings = $bindable(null),
		editDescription = $bindable(''),
		oncancelEditing,
		onsaveEdits,
		onsaveEditedProfile,
		onpullFromV1,
		oncreateNewProfile,
		onstartEditing,
		onshowSaveDialog
	} = $props();
</script>

<div class="surface-card">
	<div class="card-body">
		<CardSectionHead
			title="Current V1 Settings"
			subtitle={isMaintenance
				? 'Offline authoring is active; V1 push/pull resumes in normal runtime.'
				: 'Pull, edit, and push your detector configuration.'}
		/>
		{#if editingSettings}
			<StatusAlert
				message={{
					type: 'info',
					text: currentProfile?.name
						? `Editing profile: ${currentProfile.name}`
						: 'Creating new offline profile'
				}}
			/>
			<div class="field-control max-w-md">
				<label class="label" for="edit-description">
					<span class="field-label">Description</span>
				</label>
				<input
					id="edit-description"
					type="text"
					placeholder="Update description"
					class="input input-sm w-full"
					bind:value={editDescription}
				/>
			</div>
		{/if}
		{#if !v1Connected && !editingSettings}
			<p class="copy-warning">
				{isMaintenance
					? 'V1 BLE is not running in maintenance mode. Create or edit saved profiles offline.'
					: 'Connect to V1 to view/edit settings'}
			</p>
		{:else if currentProfile && currentProfile.settings}
			{@const settings = editingSettings ? editedSettings : currentProfile.settings}
			<div class="space-y-3">
				<div class="surface-panel">
					<h3 class="copy-mini-title">Band Detection</h3>
					<div class="grid grid-cols-2 sm:grid-cols-3 gap-2 text-sm">
						<label class="flex items-center gap-2">
							<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.ka} disabled={!editingSettings} />
							<span>Ka Band</span>
						</label>
						<label class="flex items-center gap-2">
							<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.k} disabled={!editingSettings} />
							<span>K Band</span>
						</label>
						<label class="flex items-center gap-2">
							<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.x} disabled={!editingSettings} />
							<span>X Band</span>
						</label>
						<label class="flex items-center gap-2">
							<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.ku} disabled={!editingSettings} />
							<span>Ku Band</span>
						</label>
						<label class="flex items-center gap-2">
							<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.laser} disabled={!editingSettings} />
							<span>Laser</span>
						</label>
						<label class="flex items-center gap-2">
							<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.euroMode} disabled={!editingSettings} />
							<span>Euro Mode</span>
						</label>
					</div>
				</div>

				<div class="surface-panel">
					<h3 class="copy-mini-title">Sensitivity</h3>
					<div class="grid grid-cols-1 sm:grid-cols-3 gap-3 text-sm">
						<div class="flex items-center justify-between">
							<span>Ka Sensitivity</span>
							{#if editingSettings}
								<select class="select select-xs w-24" bind:value={settings.kaSensitivity}>
									<option value={1}>Relaxed</option>
									<option value={2}>Original</option>
									<option value={3}>Full</option>
								</select>
							{:else}
								<span class="badge badge-info">{settings.kaSensitivity === 3 ? 'Full' : settings.kaSensitivity === 2 ? 'Original' : 'Relaxed'}</span>
							{/if}
						</div>
						<div class="flex items-center justify-between">
							<span>K Sensitivity</span>
							{#if editingSettings}
								<select class="select select-xs w-24" bind:value={settings.kSensitivity}>
									<option value={1}>Relaxed</option>
									<option value={2}>Full</option>
									<option value={3}>Original</option>
								</select>
							{:else}
								<span class="badge badge-info">{settings.kSensitivity === 3 ? 'Original' : settings.kSensitivity === 2 ? 'Full' : 'Relaxed'}</span>
							{/if}
						</div>
						<div class="flex items-center justify-between">
							<span>X Sensitivity</span>
							{#if editingSettings}
								<select class="select select-xs w-24" bind:value={settings.xSensitivity}>
									<option value={1}>Relaxed</option>
									<option value={2}>Full</option>
									<option value={3}>Original</option>
								</select>
							{:else}
								<span class="badge badge-info">{settings.xSensitivity === 3 ? 'Original' : settings.xSensitivity === 2 ? 'Full' : 'Relaxed'}</span>
							{/if}
						</div>
					</div>
				</div>

				<div class="surface-panel">
					<h3 class="copy-mini-title">Audio & Mute</h3>
					<div class="grid grid-cols-1 sm:grid-cols-2 gap-2 text-sm">
						<label class="flex items-center justify-between">
							<span>Mute-to-Muted Volume</span>
							<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.muteToMuteVolume} disabled={!editingSettings} />
						</label>
						<div class="flex items-center justify-between">
							<span>X, K, Ku Automute</span>
							{#if editingSettings}
								<select class="select select-xs w-28" bind:value={settings.autoMute}>
									<option value={1}>On</option>
									<option value={2}>Advanced</option>
									<option value={3}>Off</option>
								</select>
							{:else}
								<span class="badge badge-info">{settings.autoMute === 3 ? 'Off' : settings.autoMute === 2 ? 'Advanced' : 'On'}</span>
							{/if}
						</div>
						<label class="flex items-center justify-between">
							<span>Bogey-Lock Loud</span>
							<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.bogeyLockLoud} disabled={!editingSettings} />
						</label>
					</div>
				</div>

				<div class="surface-panel">
					<h3 class="copy-mini-title">Laser Options</h3>
					<div class="grid grid-cols-2 gap-2 text-sm">
						<label class="flex items-center justify-between">
							<span>Rear Laser</span>
							<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.laserRear} disabled={!editingSettings} />
						</label>
						<label class="flex items-center justify-between">
							<span>Fast Laser</span>
							<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.fastLaserDetect} disabled={!editingSettings} />
						</label>
					</div>
				</div>

				<div class="surface-panel">
					<h3 class="copy-mini-title">Logic & Priority</h3>
					<div class="grid grid-cols-1 sm:grid-cols-2 gap-2 text-sm">
						<label class="flex items-center justify-between">
							<span>X&K Rear Mute in Logic</span>
							<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.muteXKRear} disabled={!editingSettings} />
						</label>
						<label class="flex items-center justify-between">
							<span>Ka Always Radar Priority</span>
							<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.kaAlwaysPriority} disabled={!editingSettings} />
						</label>
						<label class="flex items-center justify-between">
							<span>K-Verifier (TMF)</span>
							<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.kVerifier} disabled={!editingSettings} />
						</label>
						<label class="flex items-center justify-between">
							<span>BSM Plus</span>
							<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.bsmPlus} disabled={!editingSettings} />
						</label>
					</div>
				</div>

				<div class="surface-panel">
					<h3 class="copy-mini-title">V1 Display</h3>
					<div class="grid grid-cols-2 gap-2 text-sm">
						<label class="flex items-center justify-between">
							<span>Startup Sequence</span>
							<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.startupSequence} disabled={!editingSettings} />
						</label>
						<label class="flex items-center justify-between">
							<span>Resting Display</span>
							<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.restingDisplay} disabled={!editingSettings} />
						</label>
					</div>
				</div>

				<details class="surface-collapse">
					<summary class="collapse-title font-semibold text-sm min-h-0 py-3">
						Photo Radar
					</summary>
					<div class="collapse-content">
						<div class="grid grid-cols-1 sm:grid-cols-2 gap-2 text-sm pt-2">
							<label class="flex items-center justify-between">
								<span>Photo Verifier</span>
								<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.photoVerifier} disabled={!editingSettings} />
							</label>
							<label class="flex items-center justify-between">
								<span>MRCT</span>
								<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.mrct} disabled={!editingSettings} />
							</label>
							<label class="flex items-center justify-between">
								<span>DriveSafe™ 3D</span>
								<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.driveSafe3D} disabled={!editingSettings} />
							</label>
							<label class="flex items-center justify-between">
								<span>DriveSafe™ 3DHD</span>
								<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.driveSafe3DHD} disabled={!editingSettings} />
							</label>
							<label class="flex items-center justify-between">
								<span>Redflex® Halo</span>
								<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.redflexHalo} disabled={!editingSettings} />
							</label>
							<label class="flex items-center justify-between">
								<span>Redflex® NK7</span>
								<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.redflexNK7} disabled={!editingSettings} />
							</label>
							<label class="flex items-center justify-between">
								<span>Ekin</span>
								<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.ekin} disabled={!editingSettings} />
							</label>
						</div>
					</div>
				</details>

				<details class="surface-collapse">
					<summary class="collapse-title font-semibold text-sm min-h-0 py-3">
						Advanced
					</summary>
					<div class="collapse-content">
						<div class="grid grid-cols-1 sm:grid-cols-2 gap-2 text-sm pt-2">
							<label class="flex items-center justify-between">
								<span>Custom Frequencies</span>
								<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={settings.customFreqs} disabled={!editingSettings} />
							</label>
						</div>
					</div>
				</details>
			</div>
		{:else}
			<p class="state-empty">No settings available. Pull from V1 to view.</p>
		{/if}
		<div class="card-actions justify-end gap-2">
			{#if editingSettings}
				<button class="btn btn-ghost btn-sm" onclick={oncancelEditing}>
					Cancel
				</button>
				<button
					class="btn btn-success btn-sm"
					onclick={onsaveEdits}
					disabled={!v1Connected || isMaintenance}
					title={isMaintenance ? 'Push/Pull is a normal-runtime operation.' : ''}
				>
					Push to V1
				</button>
				{#if currentProfile?.name}
					<button class="btn btn-primary btn-sm" onclick={onsaveEditedProfile}>
						Save Profile
					</button>
				{:else}
					<button class="btn btn-primary btn-sm" onclick={onshowSaveDialog}>
						Save as Profile
					</button>
				{/if}
			{:else}
				<button class="btn btn-secondary btn-sm" onclick={oncreateNewProfile}>
					New Profile
				</button>
				<button
					class="btn btn-primary btn-sm"
					onclick={onpullFromV1}
					disabled={!v1Connected || isMaintenance}
					title={isMaintenance ? 'Push/Pull is a normal-runtime operation.' : ''}
				>
					Pull from V1
				</button>
				{#if currentProfile && currentProfile.settings}
					<button class="btn btn-secondary btn-sm" onclick={onstartEditing} disabled={!v1Connected}>
						Edit
					</button>
					<button class="btn btn-success btn-sm" onclick={onshowSaveDialog}>
						Save
					</button>
				{/if}
			{/if}
		</div>
	</div>
</div>
