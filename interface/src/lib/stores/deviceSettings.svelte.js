import { fetchJsonWithTimeout } from '#lib/utils/poll.js';

let settingsFetchPromise = null;
let settingsInvalidatedDuringFetch = false;
let settingsConsumerCount = 0;
let stateVersion = 0;

function resetDeviceSettingsState() {
    stateVersion += 1;
    settingsFetchPromise = null;
    settingsInvalidatedDuringFetch = false;
}

async function fetchDeviceSettingsOnce(fetchVersion) {
    const res = await fetchJsonWithTimeout('/api/device/settings');
    if (!res.ok || fetchVersion !== stateVersion || settingsConsumerCount <= 0) return undefined;
    return res.data;
}

export function refreshDeviceSettings() {
    if (settingsConsumerCount <= 0) return Promise.resolve(undefined);

    if (settingsFetchPromise) {
        return settingsFetchPromise;
    }

    const fetchVersion = stateVersion;
    const fetchPromise = (async () => {
        let latest;
        try {
            do {
                settingsInvalidatedDuringFetch = false;
                latest = await fetchDeviceSettingsOnce(fetchVersion);
            } while (
                settingsInvalidatedDuringFetch &&
                fetchVersion === stateVersion &&
                settingsConsumerCount > 0
            );
            return latest;
        } catch {
            return undefined;
        } finally {
            if (settingsFetchPromise === fetchPromise) {
                settingsFetchPromise = null;
            }
        }
    })();
    settingsFetchPromise = fetchPromise;

    return fetchPromise;
}

export function invalidateDeviceSettings() {
    if (settingsFetchPromise) {
        settingsInvalidatedDuringFetch = true;
        return settingsFetchPromise;
    }
    return refreshDeviceSettings();
}

export function retainDeviceSettings() {
    settingsConsumerCount += 1;
    void refreshDeviceSettings();
    return releaseDeviceSettings;
}

function releaseDeviceSettings() {
    if (settingsConsumerCount > 0) {
        settingsConsumerCount -= 1;
    }

    if (settingsConsumerCount === 0) {
        resetDeviceSettingsState();
    }
}
