<script>
    let { operationStatus } = $props();

    const COMPONENT_NAMES = {
        userSettings: 'Detection settings',
        display: 'Detector display',
        mode: 'Operating mode',
        volume: 'Detector volume',
        customFrequencies: 'Custom frequency ranges',
        factoryReset: 'Factory reset command'
    };

    const OUTCOME_TEXT = {
        not_requested: 'Not requested',
        pending: 'Waiting to apply',
        unchanged: 'Already matched',
        sent: 'Sent, not verified',
        verified: 'Verified',
        unsupported: 'Not supported',
        invalid: 'Invalid settings',
        blocked: 'Could not apply',
        write_failed: 'Could not send',
        read_failed: 'Could not read back',
        mismatch: 'Readback did not match',
        timeout: 'Timed out',
        disconnected: 'Detector disconnected',
        session_changed: 'Detector connection changed',
        load_failed: 'Could not load profile'
    };

    const REASON_TEXT = {
        detector_timeout: 'The detector operation timed out.',
        wrong_detector: 'The connected V1 did not match the captured detector.',
        queue_rejected: 'V1Simple could not queue the operation.',
        detector_disconnected: 'The V1 disconnected before the operation finished.',
        disconnected: 'The V1 disconnected before this setting was confirmed.',
        executor_busy: 'Another detector operation was already running.',
        no_profile_configured: 'No profile is assigned to this Auto-Push slot.',
        profile_busy: 'The profile was in use and could not be loaded.',
        profile_load_failed: 'V1Simple could not load the saved profile.',
        invalid_configuration: 'The profile contains invalid settings.',
        unsupported_configuration: 'The connected V1 does not support the requested settings.',
        active_slot_persist_failed: 'V1Simple could not save the active Auto-Push slot.',
        in_the_box_persist_failed: 'V1Simple could not confirm saving the In-the-Box choices, so it did not activate them. Detector results are shown below.',
        staging_unavailable: 'V1Simple could not prepare the profile for application.',
        apply_partial: 'Some requested settings could not be confirmed. Check the results below.',
        apply_failed: 'The requested settings could not be fully applied. Check the results below.',
        factory_reset_send_failed: 'The factory reset command could not be sent.',
        factory_reset_scope_unverified: 'The reset command was sent, but the detector cannot confirm every factory default.',
        factory_reset_defaults_mismatch: 'The settings read after reset did not match the expected factory defaults.',
        recapture_start_failed: 'V1Simple could not start reading the detector settings after the operation.',
        recapture_timed_out: 'Reading the detector settings after the operation timed out.',
        snapshot_store_unavailable: 'Storage for the captured detector settings was unavailable.',
        snapshot_persist_failed: 'V1Simple could not save the detector settings captured after the operation.',
        interrupted: 'The operation was interrupted before it finished.',
        storage_unavailable: 'Storage for the operation result was unavailable.',
        invalid_record: 'The saved operation result could not be read reliably.',
        session_changed: 'The detector connection changed before this setting was confirmed.',
        missing_live_snapshot: 'Current detector settings were unavailable.',
        version_unknown: 'The detector firmware version could not be confirmed.',
        unsupported_firmware: 'The detector firmware does not support this setting.',
        invalid_profile_schema: 'The saved profile format was invalid.',
        invalid_user_setting_value: 'The profile contains an invalid detection setting.',
        invalid_policy: 'The profile contains an invalid setting choice.',
        invalid_volume_pair: 'The main or muted volume was invalid.',
        unsupported_saved_volume: 'This detector cannot save the requested volume.',
        unsupported_custom_frequencies: 'This detector does not support custom frequency ranges.',
        unsupported_bluetooth_led: 'This detector does not support the requested Bluetooth indicator setting.',
        custom_frequency_preservation_required: 'The detector’s existing custom frequency ranges must be preserved before this profile can be applied.',
        euro_advanced_mode_invalid: 'Advanced Logic is unavailable in Euro mode.',
        volume_owner_busy: 'Another volume control was active.',
        user_bytes_before_required: 'Current detection settings could not be confirmed before applying the profile.',
        user_bytes_write_failed: 'The detection settings could not be sent.',
        user_bytes_read_failed: 'The detection settings could not be read back.',
        user_bytes_mismatch: 'The detection settings read back did not match the profile.',
        user_bytes_timeout: 'Confirmation of the detection settings timed out.',
        display_write_failed: 'The display setting could not be sent.',
        display_mismatch: 'The display setting read back did not match the profile.',
        display_timeout: 'Confirmation of the display setting timed out.',
        mode_write_failed: 'The operating mode could not be sent.',
        mode_mismatch: 'The operating mode read back did not match the profile.',
        mode_timeout: 'Confirmation of the operating mode timed out.',
        volume_write_failed: 'The volume setting could not be sent.',
        volume_read_failed: 'The volume setting could not be read back.',
        volume_mismatch: 'The volume read back did not match the profile.',
        volume_timeout: 'Confirmation of the volume setting timed out.',
        custom_configuration_invalid: 'The custom frequency ranges were invalid.',
        custom_write_failed: 'The custom frequency ranges could not be sent.',
        custom_commit_rejected: 'The detector rejected the custom frequency ranges.',
        custom_commit_timeout: 'Saving the custom frequency ranges on the detector timed out.',
        custom_read_failed: 'The custom frequency ranges could not be read back.',
        custom_readback_invalid: 'The custom frequency ranges read back were invalid.',
        custom_readback_timeout: 'Reading back the custom frequency ranges timed out.',
        factory_default_unverified: 'The detector cannot confirm every factory default.',
        factory_user_defaults_mismatch: 'The detection settings did not match the expected factory defaults.',
        factory_reset_write_failed: 'The factory reset command could not be sent.',
        proxy_owns_detector: 'Another connected accessory was controlling the detector.'
    };

    function stateText(status) {
        const reset = status.kind === 'factory_reset';
        const states = {
            pending_normal_boot: 'Restarting V1Simple',
            waiting_for_detector: 'Waiting for your V1',
            preparing: reset ? 'Preparing factory reset' : 'Preparing profile',
            running: reset ? 'Resetting detector' : 'Applying profile',
            recapturing: reset ? 'Checking detector after reset' : 'Reading back detector settings',
            succeeded: reset ? 'Reset not fully verified' : 'Applied and verified',
            partial: reset ? 'Reset not fully verified' : 'Apply not fully verified',
            failed: reset ? 'Factory reset failed' : 'Apply failed'
        };
        return states[status.state] || 'Detector operation status unavailable';
    }

    function reasonText(reason) {
        return !reason || reason === 'none' ? null :
            REASON_TEXT[reason] || 'This result could not be confirmed. Check the operation details.';
    }
</script>

<div class="surface-card" aria-label="Detector operation status">
    <div class="card-body space-y-3">
        <h2 class="card-title"
            class:text-success={operationStatus.state === 'succeeded' && operationStatus.kind !== 'factory_reset'}
            class:text-warning={operationStatus.state === 'partial'}
            class:text-error={operationStatus.state === 'failed'}>
            {stateText(operationStatus)}
        </h2>
        {#if operationStatus.profileName}
            <p class="copy-muted">Profile: {operationStatus.profileName}</p>
        {/if}
        {#if reasonText(operationStatus.reason)}
            <p class="copy-caption">{reasonText(operationStatus.reason)}</p>
        {/if}
        {#if operationStatus.components}
            <div class="grid gap-2 sm:grid-cols-2">
                {#each Object.entries(operationStatus.components) as [name, component] (name)}
                    {#if component.requested}
                        <div class="surface-panel">
                            <div class="copy-caption">{COMPONENT_NAMES[name] || name}</div>
                            <div>{OUTCOME_TEXT[component.outcome] || 'Result unavailable'}</div>
                            {#if reasonText(component.reason)}
                                <p class="copy-caption">{reasonText(component.reason)}</p>
                            {/if}
                            <div class="copy-caption">
                                Sent: {component.sent ? 'Yes' : 'No'} · Verified: {component.verified ? 'Yes' : 'No'}
                            </div>
                        </div>
                    {/if}
                {/each}
            </div>
        {/if}
        <details class="surface-collapse">
            <summary class="collapse-title min-h-0 py-3 text-sm font-semibold">Operation details</summary>
            <div class="collapse-content space-y-2">
                <p class="copy-caption">Detector operation {operationStatus.operationId}</p>
                {#if operationStatus.kind && operationStatus.targetAddress}
                    <p class="copy-caption">
                        {operationStatus.kind} · target {operationStatus.targetAddress} · source {operationStatus.source}
                    </p>
                {/if}
                <p class="copy-caption">
                    State: {operationStatus.state || 'unknown'} · Result: {operationStatus.result || 'in_progress'} · Reason: {operationStatus.reason || 'none'}
                </p>
                {#each Object.entries(operationStatus.components || {}) as [name, component] (name)}
                    {#if component.requested}
                        <p class="copy-caption">{name}: {component.outcome} · {component.reason}</p>
                    {/if}
                {/each}
            </div>
        </details>
    </div>
</div>
