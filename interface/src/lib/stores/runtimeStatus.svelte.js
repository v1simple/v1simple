import { derived, writable } from 'svelte/store';

import { createPoll, fetchWithTimeout } from '$lib/utils/poll';

const STATUS_POLL_INTERVAL_MS = 3000;

function createDefaultStatus() {
    return {
        wifi: {
            sta_connected: false,
            ap_active: false,
            sta_ip: '',
            ap_ip: '',
            ssid: '',
            rssi: 0
        },
        device: {
            uptime: 0,
            heap_free: 0,
            hostname: 'v1simple'
        },
        maintenanceBoot: false,
        maintenanceBootUptimeMs: 0,
        v1_connected: false,
        alert: null
    };
}

export const runtimeStatus = writable(createDefaultStatus());
export const isMaintenance = derived(
    runtimeStatus,
    ($runtimeStatus) => !!$runtimeStatus.maintenanceBoot
);
export const runtimeStatusError = writable(null);
export const runtimeStatusLoading = writable(true);

let statusFetchInFlight = false;
let statusConsumerCount = 0;
let stateVersion = 0;

let statusPoll = null;

function resetRuntimeState() {
    stateVersion += 1;
    runtimeStatus.set(createDefaultStatus());
    runtimeStatusError.set(null);
    runtimeStatusLoading.set(true);
    statusFetchInFlight = false;
}

function stopStatusPoll() {
    if (statusPoll) {
        statusPoll.stop();
        statusPoll = null;
    }
}

function syncStatusPoll() {
    if (statusConsumerCount <= 0) {
        stopStatusPoll();
        return;
    }

    if (!statusPoll) {
        statusPoll = createPoll(fetchRuntimeStatus, STATUS_POLL_INTERVAL_MS);
    }
    statusPoll.start();
}

async function fetchRuntimeStatus() {
    if (statusFetchInFlight) return;
    statusFetchInFlight = true;
    const fetchVersion = stateVersion;

    try {
        const statusRes = await fetchWithTimeout('/api/status');
        if (fetchVersion !== stateVersion || statusConsumerCount <= 0) return;

        if (statusRes.ok) {
            runtimeStatus.set(await statusRes.json());
            runtimeStatusError.set(null);
            return;
        }

        runtimeStatusError.set('API error');
    } catch (e) {
        if (fetchVersion === stateVersion && statusConsumerCount > 0) {
            runtimeStatusError.set('Connection lost');
        }
    } finally {
        if (fetchVersion === stateVersion && statusConsumerCount > 0) {
            runtimeStatusLoading.set(false);
        }
        statusFetchInFlight = false;
    }
}

export function retainRuntimeStatus({ needsStatus = false } = {}) {
    if (needsStatus) {
        statusConsumerCount += 1;
    }

    syncStatusPoll();

    if (needsStatus) {
        void fetchRuntimeStatus();
    }

    return () => {
        if (needsStatus && statusConsumerCount > 0) {
            statusConsumerCount -= 1;
        }

        syncStatusPoll();

        if (statusConsumerCount === 0) {
            resetRuntimeState();
        }
    };
}
