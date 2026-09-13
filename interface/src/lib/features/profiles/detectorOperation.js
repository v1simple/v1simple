export const DETECTOR_OPERATION_STORAGE_KEY = 'v1simple.detectorSettingsOperationId';
export const DETECTOR_OPERATION_POLL_WINDOW_MS = 5 * 60 * 1000;
export const DETECTOR_OPERATION_STATES = [
    'pending_normal_boot', 'waiting_for_detector', 'preparing', 'running', 'recapturing'
];
export const DETECTOR_OPERATION_COMPONENTS = [
    'userSettings', 'display', 'mode', 'volume', 'customFrequencies', 'factoryReset'
];

const KINDS = ['apply_slot', 'apply_profile', 'factory_reset'];
const SOURCES = ['maintenance_ui', 'triple_tap'];
const REASONS = [
    'none', 'detector_timeout', 'wrong_detector', 'queue_rejected', 'detector_disconnected',
    'executor_busy', 'no_profile_configured', 'profile_busy', 'profile_load_failed',
    'invalid_configuration', 'unsupported_configuration', 'active_slot_persist_failed',
    'staging_unavailable', 'apply_partial', 'apply_failed', 'factory_reset_send_failed',
    'factory_reset_scope_unverified', 'factory_reset_defaults_mismatch',
    'recapture_start_failed', 'recapture_timed_out', 'snapshot_store_unavailable',
    'snapshot_persist_failed', 'interrupted', 'storage_unavailable', 'invalid_record'
];
const OUTCOMES = [
    'not_requested', 'pending', 'unchanged', 'sent', 'verified', 'unsupported',
    'invalid', 'blocked', 'write_failed', 'read_failed', 'mismatch', 'timeout',
    'disconnected', 'session_changed', 'load_failed'
];
const COMPONENT_REASONS = [
    'none', 'disconnected', 'session_changed', 'missing_live_snapshot', 'version_unknown',
    'unsupported_firmware', 'profile_busy', 'profile_load_failed', 'invalid_profile_schema',
    'invalid_user_setting_value', 'invalid_policy', 'invalid_volume_pair',
    'unsupported_saved_volume', 'unsupported_custom_frequencies', 'unsupported_bluetooth_led',
    'custom_frequency_preservation_required', 'euro_advanced_mode_invalid', 'volume_owner_busy',
    'user_bytes_before_required', 'user_bytes_write_failed', 'user_bytes_read_failed',
    'user_bytes_mismatch', 'user_bytes_timeout', 'display_write_failed', 'display_mismatch',
    'display_timeout', 'mode_write_failed', 'mode_mismatch', 'mode_timeout',
    'volume_write_failed', 'volume_read_failed', 'volume_mismatch', 'volume_timeout',
    'custom_configuration_invalid', 'custom_write_failed', 'custom_commit_rejected',
    'custom_commit_timeout', 'custom_read_failed', 'custom_readback_invalid',
    'custom_readback_timeout', 'factory_default_unverified',
    'factory_user_defaults_mismatch', 'factory_reset_write_failed', 'proxy_owns_detector'
];

export function validDetectorOperationId(value) {
    return Number.isSafeInteger(value) && value > 0 && value <= 0xffffffff;
}

export function readDetectorOperationId(storage) {
    try {
        const raw = storage.getItem(DETECTOR_OPERATION_STORAGE_KEY);
        const parsed = /^(?:[1-9][0-9]*)$/.test(raw || '') ? Number(raw) : null;
        if (validDetectorOperationId(parsed)) return parsed;
        if (raw) storage.removeItem(DETECTOR_OPERATION_STORAGE_KEY);
    } catch {
        // Storage is only a resume hint; the device owns durable truth.
    }
    return null;
}

export function rememberDetectorOperationId(storage, operationId) {
    if (!validDetectorOperationId(operationId)) return false;
    try {
        storage.setItem(DETECTOR_OPERATION_STORAGE_KEY, String(operationId));
        return true;
    } catch {
        return false;
    }
}

export function forgetDetectorOperationId(storage) {
    try {
        storage.removeItem(DETECTOR_OPERATION_STORAGE_KEY);
    } catch {
        // Nothing else to clear.
    }
}

export function validDetectorOperationStart(response) {
    return response?.success === true && response.queued === true && response.rebooting === true &&
        response.target === 'normal' &&
        response.state === 'pending_normal_boot' && validDetectorOperationId(response.operationId);
}

export function validDetectorOperationStatus(status, operationId) {
    if (!status || status.operationId !== operationId || typeof status.terminal !== 'boolean' ||
        !KINDS.includes(status.kind) || !SOURCES.includes(status.source) ||
        !REASONS.includes(status.reason) ||
        !/^[0-9A-F]{2}(?::[0-9A-F]{2}){5}$/.test(status.targetAddress || '') ||
        typeof status.returnToMaintenance !== 'boolean' ||
        typeof status.components !== 'object' || status.components === null ||
        Object.keys(status.components).length !== DETECTOR_OPERATION_COMPONENTS.length ||
        !DETECTOR_OPERATION_COMPONENTS.every((name) => Object.hasOwn(status.components, name))) return false;
    if (status.source === 'triple_tap' &&
        (status.kind !== 'apply_slot' || status.returnToMaintenance)) return false;
    if (status.kind === 'factory_reset' && status.source !== 'maintenance_ui') return false;
    if (status.kind === 'apply_slot' &&
        (!Number.isInteger(status.slot) || status.slot < 0 || status.slot > 2)) return false;
    if (status.kind === 'apply_profile' &&
        (typeof status.profileName !== 'string' || !status.profileName)) return false;
    for (const name of DETECTOR_OPERATION_COMPONENTS) {
        const component = status.components[name];
        const unchanged = component?.outcome === 'unchanged';
        const verified = component?.outcome === 'verified';
        if (!component || typeof component.requested !== 'boolean' ||
            typeof component.sent !== 'boolean' || typeof component.verified !== 'boolean' ||
            !OUTCOMES.includes(component.outcome) || !COMPONENT_REASONS.includes(component.reason) ||
            (component.sent && !component.requested) ||
            (!component.requested &&
                (component.sent || component.verified || component.outcome !== 'not_requested' ||
                 component.reason !== 'none')) ||
            (unchanged && (!component.requested || component.sent || !component.verified ||
                component.reason !== 'none')) ||
            (verified && (!component.requested || !component.sent || !component.verified ||
                component.reason !== 'none')) ||
            (component.verified && !verified && !unchanged) ||
            (component.outcome === 'pending' && (component.sent || component.reason !== 'none')) ||
            (component.outcome === 'sent' &&
                (!component.requested || !component.sent || component.verified))) return false;
    }
    if (status.kind === 'factory_reset' && !status.components.factoryReset.requested) {
        const preSendFailure = status.state === 'failed' && [
            'detector_timeout', 'wrong_detector', 'snapshot_store_unavailable',
            'interrupted', 'storage_unavailable'
        ].includes(status.reason);
        if (['running', 'recapturing', 'succeeded', 'partial'].includes(status.state) ||
            (status.state === 'failed' && !preSendFailure)) return false;
    }
    if (status.terminal) {
        if (!['succeeded', 'partial', 'failed'].includes(status.state) ||
            status.result !== status.state) return false;
        if (status.state === 'succeeded') {
            const requested = DETECTOR_OPERATION_COMPONENTS.map((name) => status.components[name])
                .filter((component) => component.requested);
            return status.kind !== 'factory_reset' && status.reason === 'none' &&
                requested.every((component) =>
                    (component.outcome === 'verified' && component.sent && component.verified &&
                        component.reason === 'none') ||
                    (component.outcome === 'unchanged' && !component.sent && component.verified &&
                        component.reason === 'none'));
        }
        return status.reason !== 'none';
    }
    if (!DETECTOR_OPERATION_STATES.includes(status.state) || status.result !== 'in_progress') return false;
    return status.state === 'recapturing' || status.reason === 'none';
}
