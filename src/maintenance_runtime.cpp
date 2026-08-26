#include "maintenance_runtime.h"

#include <ArduinoJson.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>

#include "audio_beep.h"
#include "battery_manager.h"
#include "ble_bond_backup_writer.h"
#include "ble_client.h"
#include "build_metadata.h"
#include "config.h"
#include "display.h"
#include "main_internals.h"
#include "modules/auto_push/auto_push_module.h"
#include "modules/display/display_preview_module.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/health/health_journal.h"
#include "modules/event_log/product_event_log.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/power/power_module.h"
#include "modules/quiet/quiet_coordinator_module.h"
#include "modules/speed/speed_source_selector.h"
#include "packet_parser.h"
#include "settings.h"
#include "storage_manager.h"
#include "touch_handler.h"
#include "v1_devices.h"
#include "v1_profiles.h"
#include "wifi_manager.h"

namespace {
constexpr uint32_t kMaintenanceExitLongPressMs = 4000UL;
}

MaintenanceRuntime::MaintenanceRuntime(WiFiManager& wifi, SettingsManager& settings, V1ProfileManager& profiles,
                                       V1DeviceStore& devices, StorageManager& storage, V1Display& display,
                                       DisplayPreviewModule& preview, PowerModule& power, BatteryManager& battery,
                                       TouchHandler& touch,
                                       V1BLEClient& ble, PacketParser& parser, AutoPushModule& autoPush,
                                       ObdRuntimeModule& obd, SpeedSourceSelector& speed, GpsRuntimeModule& gps,
                                       QuietCoordinatorModule& quiet, ProductEventLog& events, HealthJournal& health,
                                       MainRuntimeState& state)
    : wifi_(wifi), settings_(settings), profiles_(profiles), devices_(devices), storage_(storage), display_(display),
      preview_(preview), power_(power), battery_(battery), touch_(touch), ble_(ble), parser_(parser),
      autoPush_(autoPush), obd_(obd),
      speed_(speed), gps_(gps), quiet_(quiet), events_(events), health_(health), state_(state),
      wifiOrchestrator_(wifi, ble, parser, storage, autoPush) {}

void MaintenanceRuntime::logHeapSnapshot(const char* stage) const {
    const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t freeDma = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    const uint32_t largestDma =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    Serial.printf("[MaintBoot] heap stage=%s freeInternal=%lu largestInternal=%lu freeDma=%lu largestDma=%lu\n",
                  stage, static_cast<unsigned long>(freeInternal), static_cast<unsigned long>(largestInternal),
                  static_cast<unsigned long>(freeDma), static_cast<unsigned long>(largestDma));
}

void MaintenanceRuntime::appendStatus(JsonObject obj, void* context) {
    auto& self = *static_cast<MaintenanceRuntime*>(context);
    obj["maintenanceBoot"] = self.active_;
    obj["maintenanceBootUptimeMs"] = self.active_ && self.state_.maintenanceBootStartedMs != 0
                                          ? static_cast<uint32_t>(millis() - self.state_.maintenanceBootStartedMs)
                                          : 0;
    obj["maintenanceBootTimeoutMs"] = MainRuntimePolicy::MaintenanceBootTimeoutMs;

    const QuietCommittedState quietCommitted = self.quiet_.getCommittedState();
    const QuietDesiredState& quietDesired = self.quiet_.getDesiredState();
    const QuietPresentationState& quietPresentation = self.quiet_.getPresentationState();

    JsonObject quietObj = obj["quiet"].to<JsonObject>();
    JsonObject desiredObj = quietObj["desired"].to<JsonObject>();
    desiredObj["muteOwner"] = quietOwnerName(quietDesired.muteOwner);
    desiredObj["muteOwnerRaw"] = static_cast<uint8_t>(quietDesired.muteOwner);
    desiredObj["mutePending"] = quietDesired.mutePending;
    desiredObj["mute"] = quietDesired.mute;
    desiredObj["volumeOwner"] = quietOwnerName(quietDesired.volumeOwner);
    desiredObj["volumeOwnerRaw"] = static_cast<uint8_t>(quietDesired.volumeOwner);
    desiredObj["volumePending"] = quietDesired.volumePending;
    desiredObj["volume"] = quietDesired.volume;
    desiredObj["muteVolume"] = quietDesired.muteVolume;

    JsonObject committedObj = quietObj["committed"].to<JsonObject>();
    committedObj["connected"] = quietCommitted.connected;
    committedObj["hasDisplayState"] = quietCommitted.hasDisplayState;
    committedObj["muted"] = quietCommitted.muted;
    committedObj["mainVolume"] = quietCommitted.mainVolume;
    committedObj["muteVolume"] = quietCommitted.muteVolume;

    JsonObject presentationObj = quietObj["presentation"].to<JsonObject>();
    presentationObj["activeMuteOwner"] = quietOwnerName(quietPresentation.activeMuteOwner);
    presentationObj["activeMuteOwnerRaw"] = static_cast<uint8_t>(quietPresentation.activeMuteOwner);
    presentationObj["activeVolumeOwner"] = quietOwnerName(quietPresentation.activeVolumeOwner);
    presentationObj["activeVolumeOwnerRaw"] = static_cast<uint8_t>(quietPresentation.activeVolumeOwner);
    presentationObj["speedVolZeroActive"] = quietPresentation.speedVolZeroActive;
    presentationObj["voiceSuppressed"] = quietPresentation.voiceSuppressed;
    presentationObj["voiceAllowVolZeroBypass"] = quietPresentation.voiceAllowVolZeroBypass;
    presentationObj["effectiveMuted"] = quietPresentation.effectiveMuted;
}

void MaintenanceRuntime::configureWebApi() {
    wifi_.setMaintenanceDependencies(ble_, display_, preview_, battery_, events_, health_);
    wifi_.setObdDependencies(&obd_, &speed_);
    wifi_.setGpsRuntime(&gps_);
    wifiOrchestrator_.ensureCallbacksConfigured();
    if (!statusCallbackConfigured_) {
        wifi_.appendStatusCallback(appendStatus, this);
        statusCallbackConfigured_ = true;
    }
}

void MaintenanceRuntime::initializeTouchAndDisplayControls() {
    Serial.println("Initializing touch handler...");
    if (touch_.begin(17, 18, AXS_TOUCH_ADDR, -1)) {
        Serial.println("Touch handler initialized successfully");
    } else {
        Serial.println("[Touch] WARN: Touch handler failed to initialize - continuing anyway");
    }

    pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);
    display_.setBrightness(settings_.get().brightness);
    Serial.printf("[Settings] Applied saved brightness: %d\n", settings_.get().brightness);
}

bool MaintenanceRuntime::preservedPanicEvidencePresent(esp_reset_reason_t resetReason) const {
    const bool crashReset = resetReason == ESP_RST_PANIC || resetReason == ESP_RST_INT_WDT ||
                            resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT;
    fs::FS* internalFs = storage_.getLittleFS();
    return crashReset || (internalFs && (internalFs->exists("/panic.txt") || internalFs->exists("/panic.prev.txt")));
}

void MaintenanceRuntime::initializeStorageAndProfiles() {
    Serial.println("[Setup] Mounting storage...");
    if (storage_.begin()) {
        Serial.printf("[Setup] Storage ready: %s\n", storage_.statusText().c_str());
        profiles_.begin(storage_.getFilesystem(), storage_.getLittleFS());
        devices_.begin(storage_.getFilesystem(), storage_.getLittleFS());
        Serial.println("[Setup] Maintenance boot: skipping audio buffer/voice init");

        if (settings_.checkAndRestoreFromSD()) {
            display_.setBrightness(settings_.get().brightness);
        }

        const String storedFallback = settings_.loadLastV1AddressFallback();
        const String degradedFallback = normalizeV1DeviceAddress(storedFallback);
        if (storedFallback.length() > 0 && degradedFallback.length() == 0) {
            settings_.clearLastV1AddressFallback();
        }

        const String settingsFallback = normalizeV1DeviceAddress(settings_.get().lastV1Address);
        const String restoredLastKnownV1 = degradedFallback.length() > 0 ? degradedFallback : settingsFallback;
        if (restoredLastKnownV1.length() > 0) {
            settings_.setLastV1Address(restoredLastKnownV1);
            if (devices_.isReady() && devices_.upsertDevice(restoredLastKnownV1) && degradedFallback.length() > 0) {
                settings_.clearLastV1AddressFallback();
            }
        }
        settings_.validateProfileReferences(profiles_);
    } else {
        Serial.println("[Setup] Storage unavailable - profiles will be disabled");
        const String storedFallback = settings_.loadLastV1AddressFallback();
        const String degradedFallback = normalizeV1DeviceAddress(storedFallback);
        if (degradedFallback.length() > 0) {
            settings_.setLastV1Address(degradedFallback);
            Serial.println("[Setup] Restored degraded V1 address fallback from NVS");
        } else if (storedFallback.length() > 0) {
            settings_.clearLastV1AddressFallback();
        }
    }

    const V1Settings& gpsSettings = settings_.get();
    gps_.begin(gpsSettings.gpsEnabled, gpsSettings.gpsBaud);
    if (gpsSettings.gpsEnabled) {
        Serial.printf("[GPS] module enabled baud=%lu rx=%d tx=%d en=not-driven\n",
                      static_cast<unsigned long>(gpsSettings.gpsBaud), 1, 5);
    }
}

void MaintenanceRuntime::start(uint32_t setupStartMs, esp_reset_reason_t resetReason) {
    if (active_) {
        return;
    }

    uint32_t stageStartedMs = millis();
    const auto logBootStage = [&](const char* stage) {
        const uint32_t nowMs = millis();
        Serial.printf("[Boot] stage=%s delta=%lu total=%lu\n", stage,
                      static_cast<unsigned long>(nowMs - stageStartedMs),
                      static_cast<unsigned long>(nowMs - setupStartMs));
        stageStartedMs = nowMs;
    };

    initializeStorageAndProfiles();

    const bool previousShutdownClean = readAndResetCleanShutdownMarker();
    Serial.println(previousShutdownClean ? "[Boot] Previous shutdown was clean"
                                         : "[Boot] Previous shutdown was UNCLEAN (no clean-shutdown marker)");
    const uint32_t bootId = nextBootId();
    HealthCounters::reset();
    (void)health_.begin(storage_, bootId, getRuntimeImageId(), resetReasonToString(resetReason),
                        previousShutdownClean, preservedPanicEvidencePresent(resetReason));
    logBootIdentity(bootId, resetReason);
    Serial.println("[MaintBoot] request consumed; entering maintenance boot");
    logBootStage("storage");

    Serial.printf("[MaintBoot] active bootId=%lu reset=%s timeoutMs=%lu maxSessionMs=%lu\n",
                  static_cast<unsigned long>(bootId), resetReasonToString(resetReason),
                  static_cast<unsigned long>(MainRuntimePolicy::MaintenanceBootTimeoutMs),
                  static_cast<unsigned long>(MainRuntimePolicy::MaintenanceBootMaxSessionMs));

    active_ = true;
    state_.maintenanceBootActive = true;
    wifi_.setMaintenanceBootMode(true);
    power_.setLifecycle(*this);

    logHeapSnapshot("pre_wifi");
    configureWebApi();
    initializeTouchAndDisplayControls();
    logBootStage("maintenance_touch");

    const uint32_t wifiStartMs = millis();
    const bool wifiStarted = MaintenanceWifiCoordinator::start(*this);
    Serial.printf("[MaintBoot] wifi_start ok=%s elapsedMs=%lu\n", wifiStarted ? "true" : "false",
                  static_cast<unsigned long>(millis() - wifiStartMs));
    logHeapSnapshot(wifiStarted ? "post_wifi" : "wifi_start_failed");
    logBootStage("maintenance_wifi");

    state_.bootReady = true;
    health_.ready(millis());
    const uint32_t sessionStartMs = millis();
    state_.maintenanceBootSessionStartedMs = sessionStartMs == 0 ? 1UL : sessionStartMs;
    state_.maintenanceLastUiActivityMs = 0;
    state_.maintenanceBootStartedMs = state_.maintenanceBootSessionStartedMs;
    Serial.printf("[MaintBoot] setup total: %lu ms\n", static_cast<unsigned long>(millis() - setupStartMs));
}

void MaintenanceRuntime::servicePowerDisplayOwnership(uint32_t nowMs) {
    if (power_.ownsDisplayPresentation()) {
        if (preview_.isRunning()) {
            preview_.cancel();
        }
        (void)preview_.consumeEnded();
        return;
    }

    if (!power_.consumeDisplayRestoreRequest()) {
        return;
    }

    const bool restoreBrightness = power_.consumeDisplayBrightnessRestoreRequest();
    if (preview_.isRunning()) {
        preview_.cancel();
    }
    (void)preview_.consumeEnded();
    display_.forceNextRedraw();

    const bool stationMode = wifi_.isConnected();
    const String ip = stationMode ? wifi_.getIPAddress() : wifi_.getAPIPAddress();
    display_.showMaintenanceMode(ip.c_str(), stationMode);
    if (restoreBrightness) {
        const uint8_t savedBrightness = settings_.get().brightness;
        display_.setBrightness(savedBrightness);
        Serial.printf("[Power] Restored display brightness=%u after shutdown abort\n",
                      static_cast<unsigned>(savedBrightness));
    }
    (void)nowMs;
}

void MaintenanceRuntime::restartNormal(const char* reason) {
    Serial.printf("[MaintBoot] %s -> rebooting normal runtime\n", reason);
    const bool persistenceSafe = completeLoggingForControlledRestart(events_, health_);
    if (persistenceSafe) {
        settings_.save();
        markCleanShutdown();
    } else {
        Serial.println("[MaintBoot] WARN: restart continuing without final persistence writes");
    }
    ESP.restart();
}

bool MaintenanceRuntime::startMaintenanceWifi() {
    return wifi_.startSetupMode(false);
}

void MaintenanceRuntime::processMaintenanceWifi() {
    wifi_.process();
}

WifiMaintenanceRecoveryResult MaintenanceRuntime::evaluateMaintenanceWifiRecovery(uint32_t nowMs) {
    WifiMaintenanceRecoveryInput recoveryInput;
    recoveryInput.maintenanceBootActive = true;
    recoveryInput.wifiServiceReachable = wifi_.isWifiServiceReachable();
    recoveryInput.nowMs = nowMs;
    return wifiRecovery_.evaluate(recoveryInput);
}

void MaintenanceRuntime::restartMaintenanceWifi(uint32_t attemptNumber) {
    Serial.printf("[MaintBoot] wifi service down - restart attempt %lu\n",
                  static_cast<unsigned long>(attemptNumber));
    const bool restarted = wifi_.startSetupMode(false);
    Serial.printf("[MaintBoot] wifi_restart ok=%s\n", restarted ? "true" : "false");
}

bool MaintenanceRuntime::maintenanceWifiActive() const {
    return wifi_.isWifiServiceActive();
}

void MaintenanceRuntime::stopMaintenanceWifi(const char* reason) {
    wifi_.stopSetupMode(true, reason);
}

void MaintenanceRuntime::tick(uint32_t nowMs) {
    if (!active_) {
        return;
    }

    audio_process_amp_timeout();
    power_.process(nowMs);
    servicePowerDisplayOwnership(nowMs);
    const bool powerPresentationOwned = power_.ownsDisplayPresentation();
    MaintenanceWifiCoordinator::service(*this, nowMs, powerPresentationOwned);

    settings_.serviceDeferredPersist(nowMs);
    settings_.serviceDeferredBackup(nowMs);

    const bool stationConnected = wifi_.isConnected();
    const String ip = stationConnected ? wifi_.getIPAddress() : wifi_.getAPIPAddress();
    const bool previewRunning = preview_.isRunning();
    if (!powerPresentationOwned && !previewRunning && (ip != shownIp_ || stationConnected != shownStation_)) {
        display_.showMaintenanceMode(ip.c_str(), stationConnected);
    }
    shownIp_ = ip;
    shownStation_ = stationConnected;

    if (!powerPresentationOwned && previewRunning) {
        preview_.update();
    }
    if (!powerPresentationOwned && preview_.consumeEnded()) {
        display_.showMaintenanceMode(ip.c_str(), stationConnected);
    }

    if (!idleHeapLogged_ && state_.maintenanceBootSessionStartedMs != 0 &&
        static_cast<uint32_t>(nowMs - state_.maintenanceBootSessionStartedMs) >= 5000UL) {
        idleHeapLogged_ = true;
        logHeapSnapshot("idle_5s");
    }

    const bool bootPressed = digitalRead(BOOT_BUTTON_GPIO) == LOW;
    if (powerPresentationOwned) {
        bootButtonPressStartMs_ = 0;
        exitRequestFired_ = false;
    } else if (bootPressed && bootButtonPressStartMs_ == 0) {
        bootButtonPressStartMs_ = nowMs == 0 ? 1 : nowMs;
        exitRequestFired_ = false;
    } else if (!bootPressed) {
        bootButtonPressStartMs_ = 0;
        exitRequestFired_ = false;
    } else if (!exitRequestFired_ && bootButtonPressStartMs_ != 0 &&
               static_cast<uint32_t>(nowMs - bootButtonPressStartMs_) >= kMaintenanceExitLongPressMs) {
        exitRequestFired_ = true;
        restartNormal("BOOT long-press exit");
    }

    if (wifi_.isUiActive(MainRuntimePolicy::MaintenanceUiActivityProbeMs)) {
        state_.maintenanceLastUiActivityMs = nowMs == 0 ? 1UL : nowMs;
    }

    MainRuntimePolicy::MaintenanceSessionInput sessionInput;
    sessionInput.nowMs = nowMs;
    sessionInput.sessionStartedMs = state_.maintenanceBootSessionStartedMs;
    sessionInput.lastUiActivityMs = state_.maintenanceLastUiActivityMs;
    sessionInput.sessionActive = state_.maintenanceBootSessionStartedMs != 0;
    const MainRuntimePolicy::MaintenanceSessionDecision session =
        MainRuntimePolicy::evaluateMaintenanceSession(sessionInput);
    state_.maintenanceBootStartedMs = session.deadlineAnchorMs;

    if (session.shouldReboot) {
        Serial.printf("[MaintBoot] timeout reason=%s sessionMs=%lu idleMs=%lu\n",
                      session.maxSessionReached ? "max_session" : "idle",
                      static_cast<unsigned long>(session.elapsedSinceStartMs),
                      static_cast<unsigned long>(session.elapsedSinceActivityMs));
        restartNormal("timeout");
    }

    vTaskDelay(pdMS_TO_TICKS(1));
}

void MaintenanceRuntime::stop() {
    if (!active_) {
        return;
    }
    MaintenanceWifiCoordinator::stop(*this, "maintenance_stop");
    active_ = false;
    state_.maintenanceBootActive = false;
    wifi_.setMaintenanceBootMode(false);
}

bool MaintenanceRuntime::preparePersistenceForShutdownPhase() {
    return preparePersistenceForShutdown(events_, health_, settings_);
}

void MaintenanceRuntime::stopMaintenanceWifiForShutdown() {
    Serial.println("[Battery] Stopping WiFi after shutdown flush...");
    wifi_.stopSetupMode(true, "poweroff");
    delay(100);
}

void MaintenanceRuntime::writeCleanShutdownMarker() {
    Serial.println("[Battery] Writing clean-shutdown marker...");
    markCleanShutdown();
}

void MaintenanceRuntime::resumePersistenceAfterAbortedShutdownPhase() {
    resumePersistenceAfterAbortedShutdown(events_);
}

void MaintenanceRuntime::resumeMaintenanceWifiAfterAbortedShutdown() {
    const bool restored = wifi_.startSetupMode(false);
    Serial.printf("[Battery] Maintenance WiFi restore ok=%s\n", restored ? "true" : "false");
}

void MaintenanceRuntime::prepareForShutdown() {
    RuntimeServiceLifecycleCoordinator::prepareMaintenance(*this);
}

void MaintenanceRuntime::resumeAfterAbortedShutdown() {
    RuntimeServiceLifecycleCoordinator::resumeMaintenance(*this);
}
