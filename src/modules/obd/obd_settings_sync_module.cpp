#include "obd_settings_sync_module.h"

#include <cstring>

#include "obd_string_utils.h"

void ObdSettingsSyncModule::begin(SettingsManager* settings, ObdRuntimeModule* obdRuntimeModule) {
    settings_ = settings;
    obdRuntimeModule_ = obdRuntimeModule;
    pendingSnapshot_ = {};
    pendingValid_ = false;
    pendingChangedAtMs_ = 0;
}

void ObdSettingsSyncModule::copyString(char* dest, size_t destLen, const char* src) {
    ObdStringUtils::copyString(dest, destLen, src);
}

bool ObdSettingsSyncModule::snapshotsEqual(const Snapshot& lhs, const Snapshot& rhs) {
    return lhs.savedAddrType == rhs.savedAddrType &&
           strcmp(lhs.savedAddress, rhs.savedAddress) == 0;
}

ObdSettingsSyncModule::Snapshot ObdSettingsSyncModule::captureRuntimeSnapshot() const {
    Snapshot snapshot;
    if (!obdRuntimeModule_) {
        return snapshot;
    }

    copyString(snapshot.savedAddress,
               sizeof(snapshot.savedAddress),
               obdRuntimeModule_->getSavedAddress());
    snapshot.savedAddrType = obdRuntimeModule_->getSavedAddrType();
    return snapshot;
}

bool ObdSettingsSyncModule::settingsMatchSnapshot(const Snapshot& snapshot) const {
    if (!settings_) {
        return true;
    }

    const V1Settings& settings = settings_->get();
    return settings.obdSavedAddress == snapshot.savedAddress &&
            settings.obdSavedAddrType == snapshot.savedAddrType;
}

void ObdSettingsSyncModule::applySnapshot(const Snapshot& snapshot) {
    if (!settings_) {
        return;
    }

    ObdSettingsUpdate update;
    update.hasSavedAddress = true;
    update.savedAddress = snapshot.savedAddress;
    update.hasSavedAddrType = true;
    update.savedAddrType = snapshot.savedAddrType;
    update.resetSavedNameOnAddressChange = true;
    settings_->applyObdSettingsUpdate(update, SettingsPersistMode::Deferred);
}

void ObdSettingsSyncModule::process(uint32_t nowMs) {
    if (!settings_ || !obdRuntimeModule_) {
        return;
    }

    const Snapshot runtimeSnapshot = captureRuntimeSnapshot();
    if (settingsMatchSnapshot(runtimeSnapshot)) {
        pendingValid_ = false;
        return;
    }

    if (!pendingValid_ || !snapshotsEqual(pendingSnapshot_, runtimeSnapshot)) {
        pendingSnapshot_ = runtimeSnapshot;
        pendingValid_ = true;
        pendingChangedAtMs_ = nowMs;
        return;
    }

    if (static_cast<int32_t>(nowMs - pendingChangedAtMs_) <
        static_cast<int32_t>(STABILITY_WINDOW_MS)) {
        return;
    }

    if (settingsMatchSnapshot(pendingSnapshot_)) {
        pendingValid_ = false;
        return;
    }

    applySnapshot(pendingSnapshot_);
    pendingValid_ = false;
}
