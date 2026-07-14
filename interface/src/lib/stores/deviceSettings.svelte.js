import { writable } from 'svelte/store';

import { createPoll, fetchWithTimeout } from '$lib/utils/poll';

const DEVICE_SETTINGS_POLL_INTERVAL_MS = 15000;

function createDefaultDeviceSettings() {
    return {};
}

const deviceSettings = writable(createDefaultDeviceSettings());
const deviceSettingsError = writable(null);
const deviceSettingsLoading = writable(true);

let settingsFetchPromise = null;
let settingsInvalidatedDuringFetch = false;
let settingsConsumerCount = 0;
let stateVersion = 0;
let settingsPoll = null;

function resetDeviceSettingsState() {
    stateVersion += 1;
    deviceSettings.set(createDefaultDeviceSettings());
    deviceSettingsError.set(null);
    deviceSettingsLoading.set(true);
    settingsFetchPromise = null;
    settingsInvalidatedDuringFetch = false;
}

function stopSettingsPoll() {
    if (settingsPoll) {
        settingsPoll.stop();
        settingsPoll = null;
    }
}

function syncSettingsPoll() {
    if (settingsConsumerCount <= 0) {
        stopSettingsPoll();
        return;
    }

    if (!settingsPoll) {
        settingsPoll = createPoll(refreshDeviceSettings, DEVICE_SETTINGS_POLL_INTERVAL_MS);
    }
    settingsPoll.start();
}

async function fetchDeviceSettingsOnce(fetchVersion) {
    const res = await fetchWithTimeout('/api/device/settings');
    if (fetchVersion !== stateVersion || settingsConsumerCount <= 0) return undefined;

    if (!res.ok) {
        deviceSettingsError.set('API error');
        return undefined;
    }

    const data = await res.json();
    if (fetchVersion !== stateVersion || settingsConsumerCount <= 0) return undefined;
    deviceSettings.set(data);
    deviceSettingsError.set(null);
    return data;
}

export function refreshDeviceSettings() {
    if (settingsConsumerCount <= 0) return Promise.resolve(undefined);

    if (settingsFetchPromise) {
        return settingsFetchPromise;
    }

    const fetchVersion = stateVersion;
    settingsFetchPromise = (async () => {
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
        } catch (error) {
            if (fetchVersion === stateVersion && settingsConsumerCount > 0) {
                deviceSettingsError.set('Connection lost');
            }
            return undefined;
        } finally {
            if (fetchVersion === stateVersion && settingsConsumerCount > 0) {
                deviceSettingsLoading.set(false);
            }
            settingsFetchPromise = null;
        }
    })();

    return settingsFetchPromise;
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
    syncSettingsPoll();
    void refreshDeviceSettings();
    return releaseDeviceSettings;
}

function releaseDeviceSettings() {
    if (settingsConsumerCount > 0) {
        settingsConsumerCount -= 1;
    }

    syncSettingsPoll();

    if (settingsConsumerCount === 0) {
        resetDeviceSettingsState();
    }
}
