function emptyScanState(runId = 0) {
    return {
        runId,
        open: false,
        scanning: false,
        networks: [],
        selectedNetwork: null,
        password: '',
        saving: false,
        targetIndex: null
    };
}

export function createWifiScanState() {
    return emptyScanState();
}

export function beginWifiScan(state, targetIndex = state.targetIndex) {
    return {
        ...emptyScanState(state.runId + 1),
        open: true,
        scanning: true,
        targetIndex:
            targetIndex === null || typeof targetIndex === 'number'
                ? targetIndex
                : state.targetIndex
    };
}

export function isCurrentWifiScan(state, runId) {
    return state.open && state.runId === runId;
}

export function applyWifiScanResponse(state, runId, payload) {
    if (!isCurrentWifiScan(state, runId)) {
        return { state, accepted: false, shouldPoll: false };
    }

    const shouldPoll = Boolean(payload?.scanning);
    return {
        state: {
            ...state,
            scanning: shouldPoll,
            networks: shouldPoll
                ? state.networks
                : Array.isArray(payload?.networks)
                  ? payload.networks
                  : []
        },
        accepted: true,
        shouldPoll
    };
}

export function selectWifiScanNetwork(state, network) {
    return { ...state, selectedNetwork: network, password: '' };
}

export function setWifiScanSaving(state, saving) {
    return { ...state, saving };
}

export function closeWifiScan(state, { force = false } = {}) {
    if (state.saving && !force) {
        return { state, closed: false };
    }
    return { state: emptyScanState(state.runId + 1), closed: true };
}

export function disposeWifiScan(state) {
    return emptyScanState(state.runId + 1);
}

export function createWifiEditorState() {
    return {
        open: false,
        mode: 'add',
        index: null,
        label: '',
        ssid: '',
        password: '',
        priority: null,
        showPassword: false,
        hasExistingPassword: false
    };
}

export function openWifiEditorState(slotIndex = null, seed = {}) {
    return {
        ...createWifiEditorState(),
        open: true,
        mode: seed.mode || 'add',
        index: slotIndex,
        label: seed.label || '',
        ssid: seed.ssid || '',
        hasExistingPassword: Boolean(seed.hasExistingPassword)
    };
}

export function closeWifiEditorState(state, { force = false, actionInFlight = false } = {}) {
    if (actionInFlight && !force) {
        return { state, closed: false };
    }
    return { state: createWifiEditorState(), closed: true };
}

export function buildWifiEditorRequest(state, expectedSsid) {
    // Spaces are part of a network's identity, including during label-only edits.
    const ssid = state.ssid;
    if (!ssid) return null;

    const body = {
        ssid,
        label: state.label.trim()
    };
    if (Number.isFinite(state.priority)) {
        body.priority = Math.max(0, Math.min(255, Math.trunc(state.priority)));
    }
    if (state.index !== null) {
        body.index = state.index;
        if (expectedSsid !== undefined) body.expectedSsid = expectedSsid;
    }
    const keepCurrentPassword = state.mode === 'edit' &&
        (ssid === expectedSsid || state.hasExistingPassword);
    if (state.password || !keepCurrentPassword) {
        body.password = state.password;
    }
    return body;
}
