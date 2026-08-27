/**
 * WiFi route-handler runtime factories.
 */

#include "wifi_manager_internals.h"
#include "ble_client.h"
#include "audio_beep.h"
#include "settings.h"
#include "settings_sanitize.h"
#include "display.h"
#include "v1_profiles.h"
#include "v1_devices.h"
#include "battery_manager.h"
#include "modules/wifi/wifi_autopush_api_service.h"
#include "modules/wifi/wifi_display_colors_api_service.h"
#include "modules/wifi/wifi_settings_api_service.h"
#include "modules/wifi/wifi_status_api_service.h"
#include "modules/wifi/wifi_client_api_service.h"
#include "modules/wifi/wifi_v1_profile_api_service.h"
#include "modules/wifi/wifi_v1_devices_api_service.h"
#include "modules/wifi/backup_api_service.h"
#include "modules/obd/obd_api_service.h"
#include "backup_payload_builder.h"
#include "storage_manager.h"
#include "settings_runtime_sync.h"
#include "modules/speed/speed_source_selector.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/display/display_preview_module.h"
#include "config.h"

WifiAutoPushApiService::Runtime WiFiManager::makeAutoPushRuntime() {
    WifiAutoPushApiService::Runtime runtime{
        [](WifiAutoPushApiService::SlotsSnapshot& snapshot, void* ctx) {
            const V1Settings& s = static_cast<WiFiManager*>(ctx)->settings_.get();
            snapshot.enabled = s.autoPushEnabled;
            snapshot.activeSlot = s.activeSlot;

            for (int slotIndex = 0; slotIndex < 3; ++slotIndex) {
                const V1Settings::ConstAutoPushSlotView slot = s.autoPushSlotView(slotIndex);
                snapshot.slots[slotIndex].name = slot.name;
                snapshot.slots[slotIndex].profile = slot.config.profileName;
                snapshot.slots[slotIndex].mode = slot.config.mode;
                snapshot.slots[slotIndex].color = slot.color;
                snapshot.slots[slotIndex].volume = slot.volume;
                snapshot.slots[slotIndex].muteVolume = slot.muteVolume;
                snapshot.slots[slotIndex].volumeConfigured = slot.volume <= 9 && slot.muteVolume <= 9;
                snapshot.slots[slotIndex].darkMode = slot.darkMode;
                snapshot.slots[slotIndex].muteToZero = slot.muteToZero;
                snapshot.slots[slotIndex].alertPersist = slot.alertPersist;
                snapshot.slots[slotIndex].priorityArrowOnly = slot.priorityArrow;
            }
        },
        this,
        [](String& json, void* ctx) {
            auto* mgr = static_cast<WiFiManager*>(ctx);
            if (!mgr->getPushStatusJson_) {
                return false;
            }
            json = mgr->getPushStatusJson_(mgr->getPushStatusJsonCtx_);
            return true;
        },
        this,
        [](const WifiAutoPushApiService::SlotUpdateRequest& request, void* ctx) {
            AutoPushSlotUpdate update;
            update.slot = request.slot;
            update.hasName = request.hasName;
            update.name = request.name;
            update.hasColor = request.hasColor;
            update.color = request.color;
            update.hasVolume = request.hasVolume;
            update.volume = request.volume;
            update.hasMuteVolume = request.hasMuteVolume;
            update.muteVolume = request.muteVolume;
            update.hasDarkMode = request.hasDarkMode;
            update.darkMode = request.darkMode;
            update.hasMuteToZero = request.hasMuteToZero;
            update.muteToZero = request.muteToZero;
            update.hasAlertPersist = request.hasAlertPersist;
            update.alertPersist = request.alertPersist;
            update.hasPriorityArrowOnly = request.hasPriorityArrowOnly;
            update.priorityArrowOnly = request.priorityArrowOnly;
            update.hasProfileName = true;
            update.profileName = request.profile;
            update.hasMode = true;
            update.mode = normalizeV1ModeValue(request.mode);
            return static_cast<WiFiManager*>(ctx)->settings_.applyAutoPushSlotUpdatePersisted(update).success;
        },
        this,
        [](int slot, const String& name, void* ctx) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasName = true;
            update.name = name;
            (void)static_cast<WiFiManager*>(ctx)->settings_.applyAutoPushSlotUpdate(
                update, SettingsPersistMode::ImmediateNvsDeferredBackup);
        },
        this,
        [](int slot, uint16_t color, void* ctx) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasColor = true;
            update.color = color;
            (void)static_cast<WiFiManager*>(ctx)->settings_.applyAutoPushSlotUpdate(
                update, SettingsPersistMode::ImmediateNvsDeferredBackup);
        },
        this,
        [](int slot, void* ctx) { return static_cast<WiFiManager*>(ctx)->settings_.getSlotVolume(slot); },
        this,
        [](int slot, void* ctx) { return static_cast<WiFiManager*>(ctx)->settings_.getSlotMuteVolume(slot); },
        this,
        [](int slot, uint8_t volume, uint8_t muteVolume, void* ctx) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasVolume = true;
            update.volume = volume;
            update.hasMuteVolume = true;
            update.muteVolume = muteVolume;
            (void)static_cast<WiFiManager*>(ctx)->settings_.applyAutoPushSlotUpdate(
                update, SettingsPersistMode::ImmediateNvsDeferredBackup);
        },
        this,
        [](int slot, bool darkMode, void* ctx) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasDarkMode = true;
            update.darkMode = darkMode;
            (void)static_cast<WiFiManager*>(ctx)->settings_.applyAutoPushSlotUpdate(
                update, SettingsPersistMode::ImmediateNvsDeferredBackup);
        },
        this,
        [](int slot, bool muteToZero, void* ctx) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasMuteToZero = true;
            update.muteToZero = muteToZero;
            (void)static_cast<WiFiManager*>(ctx)->settings_.applyAutoPushSlotUpdate(
                update, SettingsPersistMode::ImmediateNvsDeferredBackup);
        },
        this,
        [](int slot, uint8_t alertPersistSec, void* ctx) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasAlertPersist = true;
            update.alertPersist = alertPersistSec;
            (void)static_cast<WiFiManager*>(ctx)->settings_.applyAutoPushSlotUpdate(
                update, SettingsPersistMode::ImmediateNvsDeferredBackup);
        },
        this,
        [](int slot, bool priorityArrowOnly, void* ctx) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasPriorityArrowOnly = true;
            update.priorityArrowOnly = priorityArrowOnly;
            (void)static_cast<WiFiManager*>(ctx)->settings_.applyAutoPushSlotUpdate(
                update, SettingsPersistMode::ImmediateNvsDeferredBackup);
        },
        this,
        [](int slot, const String& profile, int mode, void* ctx) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasProfileName = true;
            update.profileName = profile;
            update.hasMode = true;
            update.mode = normalizeV1ModeValue(mode);
            (void)static_cast<WiFiManager*>(ctx)->settings_.applyAutoPushSlotUpdate(
                update, SettingsPersistMode::ImmediateNvsDeferredBackup);
        },
        this,
        [](void* ctx) { return static_cast<int>(static_cast<WiFiManager*>(ctx)->settings_.get().activeSlot); },
        this,
        [](int slot, void* ctx) { static_cast<WiFiManager*>(ctx)->display_->drawProfileIndicator(slot); },
        this,
        [](const WifiAutoPushApiService::ActivationRequest& request, void* ctx) {
            AutoPushStateUpdate update;
            update.hasActiveSlot = true;
            update.activeSlot = request.slot;
            update.hasEnabled = true;
            update.enabled = request.enable;
            return static_cast<WiFiManager*>(ctx)->settings_.applyAutoPushStateUpdate(
                update, SettingsPersistMode::ImmediateNvsDeferredBackup);
        },
        this,
        [](int slot, void* ctx) {
            AutoPushStateUpdate update;
            update.hasActiveSlot = true;
            update.activeSlot = slot;
            (void)static_cast<WiFiManager*>(ctx)->settings_.applyAutoPushStateUpdate(
                update, SettingsPersistMode::ImmediateNvsDeferredBackup);
        },
        this,
        [](bool enabled, void* ctx) {
            AutoPushStateUpdate update;
            update.hasEnabled = true;
            update.enabled = enabled;
            (void)static_cast<WiFiManager*>(ctx)->settings_.applyAutoPushStateUpdate(
                update, SettingsPersistMode::ImmediateNvsDeferredBackup);
        },
        this,
        [](const String& profileName, void* ctx) {
            V1Profile profile;
            const ProfileOperationResult result =
                static_cast<WiFiManager*>(ctx)->profiles_.loadProfileResult(profileName, profile, 0);
            switch (result.status) {
            case ProfileStorageStatus::Success:
                return WifiAutoPushApiService::ProfileAssignmentStatus::Success;
            case ProfileStorageStatus::NotFound:
                return WifiAutoPushApiService::ProfileAssignmentStatus::NotFound;
            case ProfileStorageStatus::Busy:
                return WifiAutoPushApiService::ProfileAssignmentStatus::Busy;
            case ProfileStorageStatus::Corrupt:
                return WifiAutoPushApiService::ProfileAssignmentStatus::Corrupt;
            case ProfileStorageStatus::InvalidName:
                return WifiAutoPushApiService::ProfileAssignmentStatus::InvalidName;
            case ProfileStorageStatus::IoError:
                return WifiAutoPushApiService::ProfileAssignmentStatus::IoError;
            }
            return WifiAutoPushApiService::ProfileAssignmentStatus::IoError;
        },
        this,
    };
    return runtime;
}

WifiDisplayColorsApiService::Runtime WiFiManager::makeDisplayColorsRuntime() {
    return WifiDisplayColorsApiService::Runtime{
        [](void* ctx) -> const V1Settings& { return static_cast<WiFiManager*>(ctx)->settings_.get(); },
        this,
        [](const DisplaySettingsUpdate& update, void* ctx) {
            static_cast<WiFiManager*>(ctx)->settings_.applyDisplaySettingsUpdate(
                update, SettingsPersistMode::ImmediateNvsDeferredBackup);
        },
        this,
        [](void* ctx) {
            static_cast<WiFiManager*>(ctx)->settings_.resetDisplaySettings(
                SettingsPersistMode::ImmediateNvsDeferredBackup);
        },
        this,
        [](uint8_t brightness, void* ctx) { static_cast<WiFiManager*>(ctx)->display_->setBrightness(brightness); },
        this,
        [](void* ctx) {
            auto* self = static_cast<WiFiManager*>(ctx);
            self->display_->updateColorTheme();
            self->display_->forceNextRedraw();
        },
        this,
        [](uint32_t durationMs, void* ctx) { static_cast<WiFiManager*>(ctx)->displayPreview_->requestHold(durationMs); },
        this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->displayPreview_->isRunning(); },
        this,
        [](void* ctx) { static_cast<WiFiManager*>(ctx)->displayPreview_->cancel(); },
        this,
    };
}

WifiAudioSettingsRuntime WiFiManager::makeAudioRuntime() {
    WifiAudioSettingsRuntime r;
    r.ctx = this;
    r.getSettings = [](void* ctx) -> const V1Settings& {
        return static_cast<WiFiManager*>(ctx)->settings_.get();
    };
    r.applySettingsUpdate = [](const AudioSettingsUpdate& update, void* ctx) {
        static_cast<WiFiManager*>(ctx)->settings_.applyAudioSettingsUpdate(
            update, SettingsPersistMode::ImmediateNvsDeferredBackup);
    };
    r.setAudioVolume = [](uint8_t volume, void* /*ctx*/) { audio_set_volume(volume); };
    r.checkRateLimit = [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); };
    return r;
}

WifiStatusApiService::StatusRuntime WiFiManager::makeStatusRuntime() {
    return WifiStatusApiService::StatusRuntime{
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->isSetupModeActive(); },
        this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->wifiClientState_ == WIFI_CLIENT_CONNECTED; },
        this,
        [](void* /*ctx*/) { return WiFi.localIP().toString(); },
        nullptr,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->getAPIPAddress(); },
        this,
        [](void* /*ctx*/) { return WiFi.SSID(); },
        nullptr,
        [](void* /*ctx*/) { return static_cast<int32_t>(WiFi.RSSI()); },
        nullptr,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->settings_.get().wifiClientEnabled; },
        this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->settings_.get().wifiClientSSID; },
        this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->settings_.get().apSSID; },
        this,
        [](void* /*ctx*/) -> unsigned long { return millis() / 1000; },
        nullptr,
        [](void* /*ctx*/) { return ESP.getFreeHeap(); },
        nullptr,
        [](void* /*ctx*/) { return String("v1simple"); },
        nullptr,
        [](void* /*ctx*/) { return String(FIRMWARE_VERSION); },
        nullptr,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->battery_->getVoltageMillivolts(); },
        this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->battery_->getPercentage(); },
        this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->battery_->isOnBattery(); },
        this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->battery_->hasBattery(); },
        this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->bleRuntime_->isConnected(); },
        this,
        mergeStatus_,
        mergeStatusCtx_,
        mergeStatus2_,
        mergeStatus2Ctx_,
        mergeAlert_,
        mergeAlertCtx_,
    };
}

WifiSettingsApiService::Runtime WiFiManager::makeSettingsRuntime() {
    WifiSettingsApiService::Runtime r;
    r.ctx = this;
    r.getSettings = [](void* ctx) -> const V1Settings& {
        return static_cast<WiFiManager*>(ctx)->settings_.get();
    };
    r.applySettingsUpdate = [](const DeviceSettingsUpdate& update, void* ctx) {
        auto* self = static_cast<WiFiManager*>(ctx);
        const bool maintenanceBoot = self && self->isMaintenanceBootMode();
        self->settings_.applyDeviceSettingsUpdate(
            update, maintenanceBoot ? SettingsPersistMode::Immediate : SettingsPersistMode::ImmediateNvsDeferredBackup);
        const V1Settings& settings = self->settings_.get();
        if (maintenanceBoot) {
            return;
        }
        self->bleRuntime_->setProxyRuntimeEnabled(settings.proxyBLE, settings.proxyName.c_str());
        if (self && self->obdRuntime_ && self->speedSelector_) {
            SettingsRuntimeSync::syncObdVehicleRuntimeSettings(settings, *self->obdRuntime_, *self->speedSelector_);
        }
    };
    r.checkRateLimit = [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); };
    r.getNvsDiagnostic = [](void* ctx) { return static_cast<WiFiManager*>(ctx)->settings_.getNvsDiagnostic(); };
    return r;
}

WifiClientApiService::Runtime WiFiManager::makeWifiClientRuntime() {
    return WifiClientApiService::Runtime{
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->settings_.get().wifiClientEnabled; },
        this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->settings_.get().wifiClientSSID; },
        this,
        [](void* ctx) { return wifiClientStateApiName(static_cast<WiFiManager*>(ctx)->wifiClientState_); },
        this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->isWifiScanRunning(); },
        this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->wifiClientState_ == WIFI_CLIENT_CONNECTED; },
        this,
        [](void* ctx) {
            auto* self = static_cast<WiFiManager*>(ctx);
            WifiClientApiService::ConnectedNetworkPayload payload;
            payload.ssid = WiFi.SSID();
            payload.connectedSlotIndex = self->currentConnectedSlotIndex_;
            payload.ip = WiFi.localIP().toString();
            payload.rssi = WiFi.RSSI();
            return payload;
        },
        this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->isWifiScanInProgress(); },
        this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->hasCompletedWifiScanResults(); },
        this,
        [](void* ctx) {
            auto* self = static_cast<WiFiManager*>(ctx);
            std::vector<ScannedNetwork> networks = self->getScannedNetworks();
            std::vector<WifiClientApiService::ScannedNetworkPayload> payloads;
            payloads.reserve(networks.size());
            for (const auto& net : networks) {
                WifiClientApiService::ScannedNetworkPayload payload;
                payload.ssid = net.ssid;
                payload.rssi = net.rssi;
                payload.secure = !net.isOpen();
                payloads.push_back(payload);
            }
            return payloads;
        },
        this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->startWifiScan(); },
        this,
        [](void* ctx) { static_cast<WiFiManager*>(ctx)->disconnectFromNetwork(); },
        this,
        [](void* ctx) { static_cast<WiFiManager*>(ctx)->forgetWifiClient(); },
        this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->enableWifiClientFromSavedCredentials(); },
        this,
        [](void* ctx) { static_cast<WiFiManager*>(ctx)->disableWifiClient(); },
        this,
        maintenanceBootMode_,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->getSavedNetworkSlots(); },
        this,
        [](const WifiClientApiService::SavedNetworkUpsertPayload& request, size_t& indexOut, void* ctx) {
            return static_cast<WiFiManager*>(ctx)->upsertSavedNetwork(request, indexOut);
        },
        this,
        [](size_t index, void* ctx) { return static_cast<WiFiManager*>(ctx)->deleteSavedNetwork(index); },
        this,
        [](size_t index, void* ctx) { return static_cast<WiFiManager*>(ctx)->testSavedNetwork(index); },
        this,
    };
}

WifiV1ProfileApiService::Runtime WiFiManager::makeV1ProfileRuntime() {
    return WifiV1ProfileApiService::Runtime{
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->profiles_.listProfiles(); },
        this,
        [](const String& name, WifiV1ProfileApiService::ProfileSummary& summary, void* ctx) {
            auto& profiles = static_cast<WiFiManager*>(ctx)->profiles_;
            V1Profile profile;
            if (!profiles.loadProfile(name, profile)) {
                return false;
            }
            summary.name = profile.name;
            summary.description = profile.description;
            summary.displayOn = profile.displayOn;
            return true;
        },
        this,
        [](const String& name, String& json, void* ctx) {
            auto& profiles = static_cast<WiFiManager*>(ctx)->profiles_;
            V1Profile profile;
            if (!profiles.loadProfile(name, profile)) {
                return false;
            }
            json = profiles.profileToJson(profile);
            return true;
        },
        this,
        [](const JsonObject& settingsObj, uint8_t outBytes[6], void* ctx) {
            V1UserSettings settings;
            if (!static_cast<WiFiManager*>(ctx)->profiles_.jsonToSettings(settingsObj, settings)) {
                return false;
            }
            memcpy(outBytes, settings.bytes, 6);
            return true;
        },
        this,
        [](const String& name, const String& description, bool displayOn, const uint8_t inBytes[6], String& error,
           void* ctx) {
            V1Profile profile;
            profile.name = name;
            profile.description = description;
            profile.displayOn = displayOn;
            memcpy(profile.settings.bytes, inBytes, 6);
            ProfileSaveResult result = static_cast<WiFiManager*>(ctx)->profiles_.saveProfile(profile);
            if (!result.success) {
                error = result.error;
                return false;
            }
            return true;
        },
        this,
        nullptr,
        nullptr,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->profiles_.hasCurrentSettings(); },
        this,
        [](void* ctx) {
            auto& profiles = static_cast<WiFiManager*>(ctx)->profiles_;
            return profiles.settingsToJson(profiles.getCurrentSettings());
        },
        this,
        [](void* ctx) { return static_cast<WiFiManager*>(ctx)->bleRuntime_->isConnected(); },
        this,
        [](void* ctx) { static_cast<WiFiManager*>(ctx)->settings_.requestDeferredBackupFromCurrentState(); },
        this,
        [](std::vector<String>& names, void* ctx) {
            const ProfileListResult result = static_cast<WiFiManager*>(ctx)->profiles_.listProfilesResult(0);
            names = result.profiles;
            switch (result.status) {
            case ProfileStorageStatus::Success:
                return WifiV1ProfileApiService::CatalogStatus::Success;
            case ProfileStorageStatus::Busy:
                return WifiV1ProfileApiService::CatalogStatus::Busy;
            case ProfileStorageStatus::Corrupt:
                return WifiV1ProfileApiService::CatalogStatus::Corrupt;
            default:
                return WifiV1ProfileApiService::CatalogStatus::IoError;
            }
        },
        this,
        [](const String& name, String& json, void* ctx) {
            auto& profileManager = static_cast<WiFiManager*>(ctx)->profiles_;
            V1Profile profile;
            const ProfileOperationResult result = profileManager.loadProfileResult(name, profile, 0);
            if (result.success()) json = profileManager.profileToJson(profile);
            switch (result.status) {
            case ProfileStorageStatus::Success:
                return WifiV1ProfileApiService::CatalogStatus::Success;
            case ProfileStorageStatus::NotFound:
                return WifiV1ProfileApiService::CatalogStatus::NotFound;
            case ProfileStorageStatus::Busy:
                return WifiV1ProfileApiService::CatalogStatus::Busy;
            case ProfileStorageStatus::Corrupt:
                return WifiV1ProfileApiService::CatalogStatus::Corrupt;
            case ProfileStorageStatus::InvalidName:
                return WifiV1ProfileApiService::CatalogStatus::InvalidName;
            case ProfileStorageStatus::IoError:
                return WifiV1ProfileApiService::CatalogStatus::IoError;
            }
            return WifiV1ProfileApiService::CatalogStatus::IoError;
        },
        this,
        [](const String& name, void* ctx) {
            auto* self = static_cast<WiFiManager*>(ctx);
            bool referencesChanged = false;
            if (!self->settings_.clearProfileReferencesPersisted(name, referencesChanged)) {
                Serial.printf("[V1Profiles] DELETE aborted name='%s': slot reconciliation did not persist\n",
                              name.c_str());
                return WifiV1ProfileApiService::CatalogStatus::IoError;
            }
            const ProfileOperationResult result = self->profiles_.deleteProfileResult(name, 250);
            switch (result.status) {
            case ProfileStorageStatus::Success:
                return WifiV1ProfileApiService::CatalogStatus::Success;
            case ProfileStorageStatus::NotFound:
                return WifiV1ProfileApiService::CatalogStatus::NotFound;
            case ProfileStorageStatus::Busy:
                return WifiV1ProfileApiService::CatalogStatus::Busy;
            case ProfileStorageStatus::Corrupt:
                return WifiV1ProfileApiService::CatalogStatus::Corrupt;
            case ProfileStorageStatus::InvalidName:
                return WifiV1ProfileApiService::CatalogStatus::InvalidName;
            case ProfileStorageStatus::IoError:
                return WifiV1ProfileApiService::CatalogStatus::IoError;
            }
            return WifiV1ProfileApiService::CatalogStatus::IoError;
        },
        this,
    };
}

WifiV1DevicesApiService::Runtime WiFiManager::makeV1DevicesRuntime() {
    return WifiV1DevicesApiService::Runtime{
        [](void* ctx) {
            auto* self = static_cast<WiFiManager*>(ctx);
            std::vector<WifiV1DevicesApiService::DeviceInfo> payload;
            if (!self->devices_.isReady()) {
                return payload;
            }

            auto devices = self->devices_.listDevices();
            auto hasAddress = [&](const String& address) {
                if (address.length() == 0) {
                    return true;
                }
                for (const auto& device : devices) {
                    if (device.address.equalsIgnoreCase(address)) {
                        return true;
                    }
                }
                return false;
            };

            const String lastV1Address = normalizeV1DeviceAddress(self->settings_.get().lastV1Address);
            if (!hasAddress(lastV1Address)) {
                self->devices_.touchDeviceInMemory(lastV1Address);
                devices = self->devices_.listDevices();
            }

            String connectedAddress;
            NimBLEAddress connected = self->bleRuntime_->getConnectedAddress();
            if (!connected.isNull()) {
                connectedAddress = normalizeV1DeviceAddress(String(connected.toString().c_str()));
                if (!hasAddress(connectedAddress)) {
                    self->devices_.touchDeviceInMemory(connectedAddress);
                    devices = self->devices_.listDevices();
                }
            }

            payload.reserve(devices.size());
            for (const auto& device : devices) {
                WifiV1DevicesApiService::DeviceInfo info;
                info.address = device.address;
                info.name = device.name;
                info.defaultProfile = device.defaultProfile;
                info.connected = connectedAddress.length() > 0 && connectedAddress.equalsIgnoreCase(device.address);
                payload.push_back(info);
            }
            return payload;
        },
        this,
        [](const String& address, const String& name, void* ctx) {
            return static_cast<WiFiManager*>(ctx)->devices_.setDeviceName(address, name);
        },
        this,
        [](const String& address, uint8_t defaultProfile, void* ctx) {
            return static_cast<WiFiManager*>(ctx)->devices_.setDeviceDefaultProfile(address, defaultProfile);
        },
        this,
        [](const String& address, void* ctx) { return static_cast<WiFiManager*>(ctx)->devices_.removeDevice(address); },
        this,
    };
}

BackupApiService::BackupRuntime WiFiManager::makeBackupRuntime() {
    BackupApiService::BackupRuntime runtime{
        // getBackupRevision
        [](void* ctx) -> uint32_t { return static_cast<WiFiManager*>(ctx)->settings_.backupRevision(); },
        // getCatalogRevision
        [](void* ctx) -> uint32_t { return static_cast<WiFiManager*>(ctx)->profiles_.catalogRevision(); },
        // buildDocument
        [](JsonDocument& doc, uint32_t snapshotMs, void* ctx) {
            auto* self = static_cast<WiFiManager*>(ctx);
            BackupPayloadBuilder::buildBackupDocument(doc, self->settings_.get(), self->profiles_,
                                                      BackupPayloadBuilder::BackupTransport::HttpDownload, snapshotMs);
        },
        // isStorageReady
        [](void* ctx) -> bool { return static_cast<WiFiManager*>(ctx)->storage_.isReady(); },
        // isSDCard
        [](void* ctx) -> bool { return static_cast<WiFiManager*>(ctx)->storage_.isSDCard(); },
        // backupToSD
        [](void* ctx) -> bool { return static_cast<WiFiManager*>(ctx)->settings_.backupToSD(); },
        // applyBackup
        [](const JsonDocument& doc, bool fullRestore, int& profilesRestored, void* ctx) -> bool {
            // A restore rewrites NVS and re-saves every profile in the backup;
            // on a slow SD that outruns the task watchdog. Feed it between
            // restore phases so a large backup cannot panic mid-restore.
            const SettingsBackupApplyResult result = static_cast<WiFiManager*>(ctx)->settings_.applyBackupDocument(
                doc, fullRestore, SettingsRestoreWatchdog{&BackupApiService::feedTaskWatchdog, nullptr});
            profilesRestored = result.profilesRestored;
            return result.success;
        },
        // syncAfterRestore
        [](void* ctx) {
            WiFiManager* self = static_cast<WiFiManager*>(ctx);
            const V1Settings& settings = self->settings_.get();
            if (self && self->isMaintenanceBootMode()) {
                return;
            }
            self->bleRuntime_->setProxyRuntimeEnabled(settings.proxyBLE, settings.proxyName.c_str());
            SettingsRuntimeSync::syncObdVehicleRuntimeSettings(settings, *self->obdRuntime_, *self->speedSelector_);
        },
        // ctx
        this,
    };
    return runtime;
}

ObdApiService::Runtime WiFiManager::makeObdRuntime() {
    ObdApiService::Runtime r;
    r.ctx = this;
    r.markUiActivity = [](void* ctx) { static_cast<WiFiManager*>(ctx)->markUiActivity(); };
    r.checkRateLimit = [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); };
    r.syncAfterConfigChange = [](void* ctx) {
        auto* self = static_cast<WiFiManager*>(ctx);
        // Maintenance boot intentionally skips BLE/OBD runtime init, so the
        // settings save still persists to NVS via applyObdSettingsUpdate, but
        // we must not touch proxy/BLE or OBD runtime here. Mirrors the guards
        // in applySettingsUpdate and syncAfterRestore.
        if (self && self->isMaintenanceBootMode()) {
            return;
        }
        const V1Settings& settings = self->settings_.get();
        self->bleRuntime_->setProxyRuntimeEnabled(settings.proxyBLE, settings.proxyName.c_str());
        SettingsRuntimeSync::syncObdVehicleRuntimeSettings(settings, *self->obdRuntime_, *self->speedSelector_);
    };
    r.maintenanceBootActive = maintenanceBootMode_;
    return r;
}
