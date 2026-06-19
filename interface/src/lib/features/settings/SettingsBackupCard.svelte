<script>
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';

	let {
		backingUpNow = false,
		onbackupNowToSd,
		ondownloadBackup,
		ondownloadBackupWithPasswords,
		restoreFile = null,
		restoring = false,
		onfileSelect,
		onrestoreBackup
	} = $props();
</script>

<div class="surface-card">
	<div class="card-body space-y-4">
		<CardSectionHead
			title="Backup & Restore"
			subtitle="Download your settings or restore from a backup file."
		/>

		<div class="flex flex-col gap-3">
			<button class="btn btn-primary btn-sm" onclick={onbackupNowToSd} disabled={backingUpNow}>
				{#if backingUpNow}
					<span class="loading loading-spinner loading-sm"></span>
				{/if}
				Backup to SD Now
			</button>

			<div class="divider my-0">OR</div>

			<button class="btn btn-outline btn-sm" onclick={() => ondownloadBackup?.()}>
				Download Backup
			</button>

			<button class="btn btn-warning btn-outline btn-sm" onclick={() => ondownloadBackupWithPasswords?.()}>
				Download Backup with WiFi Passwords
			</button>
			<p class="text-xs copy-muted">
				Only use password exports to migrate devices; the default backup omits saved WiFi passwords.
			</p>

			<div class="divider my-0">OR</div>

			<input
				type="file"
				accept=".json,application/json"
				class="file-input file- file-input-sm w-full"
				onchange={onfileSelect}
			/>

			<button class="btn btn-warning btn-sm" onclick={onrestoreBackup} disabled={!restoreFile || restoring}>
				{#if restoring}
					<span class="loading loading-spinner loading-sm"></span>
				{/if}
				Restore from Backup
			</button>
		</div>
	</div>
</div>
