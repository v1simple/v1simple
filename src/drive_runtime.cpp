#include "drive_runtime.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <NimBLEDevice.h>

#include "audio_beep.h"
#include "battery_manager.h"
#include "build_metadata.h"
#include "config.h"
#include "main_internals.h"
#include "modules/event_log/product_event_log.h"
#include "modules/health/health_journal.h"
#include "settings.h"
#include "storage_manager.h"
#include "v1_devices.h"
#include "v1_firmware_compat.h"
#include "v1_profiles.h"

namespace {
constexpr uint32_t kConnectionStateProcessMaxGapMs = 1000;
constexpr uint32_t kConnectedPersistenceDeferralMs = 10000;
constexpr uint32_t kFreshObservationWaitMs = 5000;

bool settingsSnapshotComplete(const V1DetectorSnapshot& snapshot) {
    const V1FirmwareCompat::Capabilities capabilities =
        V1FirmwareCompat::capabilities(snapshot.firmwareVersion);
    return snapshot.hasUserBytes &&
        (!capabilities.allVolume || snapshot.hasCurrentVolume) &&
        (!capabilities.modeObservation || snapshot.hasMode) &&
        (!capabilities.displayActive || snapshot.hasDisplayOn) &&
        (!capabilities.customSweeps || (snapshot.hasSweepSections && snapshot.hasMaxSweepIndex &&
                                         snapshot.hasSweepDefinitions));
}

ProductAlpState productAlpState(uint8_t heartbeatByte1) {
    switch (heartbeatByte1) {
    case 0x01:
        return ProductAlpState::TARGETED;
    case 0x03:
        return ProductAlpState::DLI;
    case 0x04:
        return ProductAlpState::LID;
    default:
        return ProductAlpState::UNKNOWN;
    }
}
} // namespace

DriveRuntime* DriveRuntime::callbackOwner_ = nullptr;

DriveRuntime::DriveRuntime(SettingsManager& settings, V1ProfileManager& profiles, V1DeviceStore& devices,
                           StorageManager& storage, BatteryManager& battery, ProductEventLog& events,
                           HealthJournal& health)
    : settings_(settings), profiles_(profiles), devices_(devices), storage_(storage), battery_(battery),
      events_(events), health_(health), display_(settings) {}

void DriveRuntime::logBootStage(const char* stage, uint32_t setupStartMs, uint32_t& stageStartedMs) const {
    const uint32_t nowMs = millis();
    Serial.printf("[Boot] stage=%s delta=%lu total=%lu\n", stage,
                  static_cast<unsigned long>(nowMs - stageStartedMs),
                  static_cast<unsigned long>(nowMs - setupStartMs));
    stageStartedMs = nowMs;
}

bool DriveRuntime::preservedPanicEvidencePresent(esp_reset_reason_t resetReason) const {
    const bool crashReset = resetReason == ESP_RST_PANIC || resetReason == ESP_RST_INT_WDT ||
                            resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT;
    fs::FS* internalFs = storage_.getLittleFS();
    return crashReset || (internalFs && (internalFs->exists("/panic.txt") || internalFs->exists("/panic.prev.txt")));
}

void DriveRuntime::initializeStorageAndProfiles() {
    Serial.println("[Setup] Mounting storage...");
    if (storage_.begin()) {
        Serial.printf("[Setup] Storage ready: %s\n", storage_.statusText().c_str());
        profiles_.begin(storage_);
        devices_.begin(storage_.getFilesystem(), storage_.getLittleFS());
        audio_init_buffers();
        audio_init_sd(storage_);

        if (settings_.checkAndRestoreFromSD()) {
            display_.updateColorTheme();
            display_.setBrightness(settings_.get().brightness);
        }
        settings_.migrateAutoPushProfilesToV2();

        const bool deleteResolved = settings_.resolvePendingV1DeviceDelete(devices_);
        if (!deleteResolved) {
            Serial.println("[Setup] WARN: pending V1 device deletion blocks fallback bootstrap");
        }
        const String storedFallback = deleteResolved ? settings_.loadLastV1AddressFallback() : String();
        const String degradedFallback = normalizeV1DeviceAddress(storedFallback);
        if (storedFallback.length() > 0 && degradedFallback.length() == 0) {
            settings_.clearLastV1AddressFallback();
        }
        const String settingsFallback = deleteResolved
                                            ? normalizeV1DeviceAddress(settings_.get().lastV1Address)
                                            : String();
        const String restoredLastKnownV1 = degradedFallback.length() > 0 ? degradedFallback : settingsFallback;
        if (restoredLastKnownV1.length() > 0) {
            settings_.setLastV1Address(restoredLastKnownV1);
            if (devices_.isReady() &&
                devices_.bootstrapDevice(restoredLastKnownV1, degradedFallback.length() > 0) &&
                degradedFallback.length() > 0) {
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

void DriveRuntime::initializeBle(uint32_t setupStartMs, uint32_t& stageStartedMs) {
    const V1Settings& preInitSettings = settings_.get();
    Serial.printf("[BootTiming] checkpoint=ble_preinit_begin total=%lu\n",
                  static_cast<unsigned long>(millis() - setupStartMs));
    const uint32_t preInitStartedMs = millis();
    if (!ble_.initBLE(storage_, preInitSettings.proxyBLE, preInitSettings.proxyName.c_str())) {
        Serial.println("BLE pre-initialization failed!");
        fatalBootError(display_, "BLE pre-init failed", true);
    }
    Serial.printf("[BootTiming] ble_preinit_ms=%lu\n", static_cast<unsigned long>(millis() - preInitStartedMs));
    logBootStage("ble_preinit", setupStartMs, stageStartedMs);

    callbackOwner_ = this;
    ble_.onDataReceived(onV1Data);
    ble_.onV1SessionOpened(onV1SessionOpened);
    ble_.onV1SessionClosed(onV1SessionClosed);
    ble_.onV1ConnectImmediate(onV1ConnectImmediate);
    ble_.onV1Connected(onV1Connected);
    Serial.printf("[BootTiming] checkpoint=ble_callbacks_registered total=%lu\n",
                  static_cast<unsigned long>(millis() - setupStartMs));

    const V1Settings& scanSettings = settings_.get();
    Serial.printf("Starting BLE scan for V1 (proxy: %s)\n", scanSettings.proxyBLE ? "enabled" : "disabled");
    Serial.printf("[BootTiming] checkpoint=ble_scan_begin total=%lu\n",
                  static_cast<unsigned long>(millis() - setupStartMs));
    const uint32_t scanStartedMs = millis();
    if (!ble_.begin(storage_, scanSettings.proxyBLE, scanSettings.proxyName.c_str())) {
        Serial.println("BLE scan failed to start!");
        fatalBootError(display_, "BLE scan failed", true);
    }
    Serial.printf("[BootTiming] ble_scan_start_ms=%lu\n", static_cast<unsigned long>(millis() - scanStartedMs));
}

bool DriveRuntime::requestMaintenanceBootRestart() {
    // A just-connected detector snapshot is intentionally staged in memory so
    // normal driving avoids SD writes during the first ten seconds. A user can
    // request maintenance sooner than that, so this controlled restart is the
    // final durability boundary. Never set the one-shot boot flag or clean
    // marker unless the pending catalog is durably promoted first.
    if (devices_.hasPendingSave()) {
        bool snapshotSaved = false;
        if (storage_.isSDCard()) {
            StorageManager::SDTryLock sdLock(storage_.getSDMutex(), /*checkDmaHeap=*/false);
            snapshotSaved = sdLock && devices_.flushPendingSave();
        } else {
            snapshotSaved = devices_.flushPendingSave();
        }
        if (!snapshotSaved) {
            Serial.println("[MaintBoot] ERROR: pending detector snapshot save failed; restart cancelled");
            return false;
        }
    }
    if (!requestMaintenanceBoot()) {
        Serial.println("[MaintBoot] ERROR: failed to persist maintenance boot request");
        return false;
    }
    if (settingsOperations_.isTerminal() &&
        settingsOperations_.snapshot().returnToMaintenance &&
        !settingsOperations_.acknowledgeReturnToMaintenance()) {
        // The one-shot boot request was committed first so a failed request
        // cannot consume the durable return intent. Roll it back when that
        // intent cannot be acknowledged, or an unrelated later reboot could
        // enter maintenance even though this restart was cancelled.
        if (readAndClearMaintenanceBootRequest()) {
            Serial.println("[MaintBoot] ERROR: failed to consume operation return request; boot request rolled back; "
                           "restart cancelled");
        } else {
            Serial.println("[MaintBoot] ERROR: failed to consume operation return request and boot-request rollback "
                           "failed; restart cancelled with maintenance boot possibly armed");
        }
        return false;
    }
    Serial.println("[MaintBoot] rebooting into maintenance mode");
    const bool persistenceSafe = completeLoggingForControlledRestart(events_, health_);
    if (persistenceSafe) {
        if (settings_.save()) {
            markCleanShutdown();
        } else {
            Serial.println("[MaintBoot] WARN: settings save failed; restart continuing unclean");
        }
    } else {
        Serial.println("[MaintBoot] WARN: restart continuing without final persistence writes");
    }
    delay(50);
    ESP.restart();
    return true;
}

bool DriveRuntime::usbMaintenanceAllowed() const {
    return active_ && state_.bootReady && !usbTailBusy_ && !power_.ownsDisplayPresentation() &&
           !parser_.hasAlerts() && !alp_.isAlertActive() && !state_.alpSignalActive &&
           !bleQueue_.isBackpressured();
}

void DriveRuntime::requestUsbMaintenanceBoot() {
    if (usbMaintenanceAllowed()) (void)requestMaintenanceBootRestart();
}

void DriveRuntime::initializeTouchAndUi() {
    autoPush_.begin(&settings_, &profiles_, &ble_, &parser_, &display_, &quiet_);

    TouchUiModule::Callbacks touchCallbacks{};
    touchCallbacks.isWifiSetupActive = [](void*) { return false; };
    touchCallbacks.stopWifiSetup = [](void*) {};
    touchCallbacks.requestMaintenanceBoot = [](void* context) {
        static_cast<DriveRuntime*>(context)->requestMaintenanceBootRestart();
    };
    touchCallbacks.requestMaintenanceBootCtx = this;
    touchCallbacks.drawWifiIndicator = [](void* context) {
        static_cast<DriveRuntime*>(context)->display_.drawWiFiIndicator();
    };
    touchCallbacks.drawWifiIndicatorCtx = this;
    touchCallbacks.restoreDisplay = [](void* context) {
        auto& self = *static_cast<DriveRuntime*>(context);
        if (!self.state_.bootSplashHoldActive) {
            self.displayPipeline_.restoreCurrentOwner(millis());
        }
    };
    touchCallbacks.restoreDisplayCtx = this;
    touchCallbacks.readObdStatus = [](uint32_t nowMs, void* context) {
        return static_cast<DriveRuntime*>(context)->obd_.snapshot(nowMs);
    };
    touchCallbacks.readObdStatusCtx = this;
    touchCallbacks.requestObdManualPairScan = [](uint32_t nowMs, void* context) {
        return static_cast<DriveRuntime*>(context)->obd_.requestManualPairScan(nowMs);
    };
    touchCallbacks.requestObdManualPairScanCtx = this;
    touchCallbacks.isObdPairGestureSafe = [](uint32_t nowMs, void* context) {
        return static_cast<DriveRuntime*>(context)->displayPipeline_.allowsObdPairGesture(nowMs);
    };
    touchCallbacks.isObdPairGestureSafeCtx = this;
    touchUi_.begin(&display_, &touch_, &settings_, touchCallbacks);

    TapGestureModule::Callbacks tapCallbacks{};
    tapCallbacks.beginProfileCycle = [](int newSlot, void* context) {
        return static_cast<DriveRuntime*>(context)->beginTripleTapProfileCycle(newSlot);
    };
    tapCallbacks.beginProfileCycleContext = this;
    tapGesture_.begin(&touch_, &settings_, &display_, &ble_, &parser_, &autoPush_, &alertPersistence_,
                      &displayMode_, &quiet_, tapCallbacks);
}

bool DriveRuntime::restoreConnectionDisplayOwner(void* context, uint32_t nowMs) {
    auto& self = *static_cast<DriveRuntime*>(context);
    const bool v1Connected = self.ble_.isConnected();
    const bool proxyConnected = self.ble_.isProxyClientConnected();
    self.display_.setBleContext(
        {v1Connected, proxyConnected, self.ble_.getConnectionRssi(), self.ble_.getProxyClientRssi()});
    const uint32_t lastRxMs = self.bleQueue_.getLastRxMillis();
    const bool receiving =
        lastRxMs != 0 && (nowMs - lastRxMs) < ConnectionRuntimeModule::Config{}.receivingHeartbeatMs;
    self.display_.setBLEProxyStatus(v1Connected, proxyConnected, receiving);
    return self.displayPipeline_.restoreCurrentOwner(nowMs);
}

void DriveRuntime::initializeRuntimeModules() {
    alertPersistence_.begin(&ble_, &parser_, &display_, &settings_);
    voice_.begin(&settings_, &ble_);
    audio_set_volume(settings_.get().voiceVolume);
    volumeFade_.begin(&settings_);
    quiet_.begin(&ble_, &parser_);

    DisplayPipelineDependencies pipelineDependencies;
    pipelineDependencies.displayMode = &displayMode_;
    pipelineDependencies.display = &display_;
    pipelineDependencies.parser = &parser_;
    pipelineDependencies.settings = &settings_;
    pipelineDependencies.ble = &ble_;
    pipelineDependencies.alertPersistence = &alertPersistence_;
    pipelineDependencies.voice = &voice_;
    pipelineDependencies.speedMute = &speedMute_;
    pipelineDependencies.quiet = &quiet_;
    pipelineDependencies.alp = &alp_;
    pipelineDependencies.alpLatch = &alpEventLatch_;
    pipelineDependencies.speedSelector = &speed_;
    displayPipeline_.begin(pipelineDependencies);

    systemEvents_.reset();
    if (!bleQueue_.begin(&ble_, &parser_, &profiles_, &preview_, &power_)) {
        fatalBootError(display_, "BLE queue init failed", true);
    }
    connectionRuntime_.begin(ble_, bleQueue_);
    connectionState_.begin(&ble_, &parser_, &display_, &power_, &bleQueue_, &alertPersistence_);
    connectionState_.setDisplayOwnerRestoreCallback(restoreConnectionDisplayOwner, this);
    connectionDispatch_.begin(connectionCadence_, connectionState_);
    obdSettingsSync_.begin(&settings_, &obd_);
    displayRestore_.begin(preview_, displayPipeline_);
    displayOrchestration_.begin(&display_, &ble_, &preview_, &displayRestore_, &parser_,
                                &volumeFade_, &speedMute_, &quiet_);
    connectionCycle_.begin(*this);

    speed_.begin(&obd_, settings_.get().obdEnabled, &gps_, settings_.get().gpsEnabled);
    obd_.begin(&obdBle_, settings_.get().obdEnabled, settings_.get().obdSavedAddress.c_str(),
               settings_.get().obdSavedAddrType, settings_.get().obdMinRssi);
    speedMute_.begin(settings_.get().speedMuteEnabled, settings_.get().speedMuteThresholdMph,
                     settings_.get().speedMuteHysteresisMph, settings_.get().speedMuteVolume,
                     settings_.get().speedMuteVoice);
    alp_.begin(settings_.get().alpEnabled);
    alp_.setEventBus(&systemEvents_);
    parser_.setAlertTableObserver(observeAlertTable, this);
}

void DriveRuntime::finalizeBoot(uint32_t setupStartMs, uint32_t& stageStartedMs) {
    state_.bootReady = true;
    ble_.setBootReady(true);
    health_.ready(millis());
    Serial.printf("[Boot] Ready gate opened at %lu ms\n", static_cast<unsigned long>(millis()));

    const uint32_t absorbStartedMs = millis();
    ble_.process(settings_);
    Serial.printf("[BootTiming] ble_absorb_ms=%lu\n", static_cast<unsigned long>(millis() - absorbStartedMs));
    Serial.println("BLE scan active from setup path");
    logBootStage("core_pipeline", setupStartMs, stageStartedMs);

    Serial.println("Setup complete - BLE scanning, WiFi off; BOOT long-press requests maintenance reboot");
    logBootStage("wifi", setupStartMs, stageStartedMs);
    Serial.printf("[Boot] setup total: %lu ms\n", static_cast<unsigned long>(millis() - setupStartMs));
}

void DriveRuntime::start(uint32_t setupStartMs, uint32_t stageStartedMs, esp_reset_reason_t resetReason) {
    if (active_) {
        return;
    }
    state_.maintenanceBootActive = false;
    (void)settingsOperations_.beginNormalBoot();
    initializeStorageAndProfiles();

    const bool previousShutdownClean = readAndResetCleanShutdownMarker();
    Serial.println(previousShutdownClean ? "[Boot] Previous shutdown was clean"
                                         : "[Boot] Previous shutdown was UNCLEAN (no clean-shutdown marker)");
    const uint32_t bootId = nextBootId();
    bootId_ = bootId;
    HealthCounters::reset();
    (void)health_.begin(storage_, bootId, getRuntimeImageId(), resetReasonToString(resetReason),
                        previousShutdownClean, preservedPanicEvidencePresent(resetReason));
    (void)events_.begin(bootId, storage_);
    logBootStage("storage", setupStartMs, stageStartedMs);

    power_.setLifecycle(*this);
    initializeBle(setupStartMs, stageStartedMs);
    initializeTouchAndUi();
    logBootStage("ui_modules", setupStartMs, stageStartedMs);
    logBootIdentity(bootId, resetReason);
    lastDisplayConfigurationLogMs_ = millis() - 1000u;
    logDisplayConfiguration(millis());
    Serial.println("[WiFi] Off in normal runtime - BOOT long-press reboots to maintenance");

    Serial.println("Initializing touch handler...");
    if (touch_.begin(17, 18, AXS_TOUCH_ADDR, -1)) {
        Serial.println("Touch handler initialized successfully");
    } else {
        Serial.println("[Touch] WARN: Touch handler failed to initialize - continuing anyway");
    }
    pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);
    display_.setBrightness(settings_.get().brightness);
    Serial.printf("[Settings] Applied saved brightness: %d\n", settings_.get().brightness);
    logBootStage("touch", setupStartMs, stageStartedMs);

    initializeRuntimeModules();
    active_ = true;
    finalizeBoot(setupStartMs, stageStartedMs);
}

void DriveRuntime::showInitialScanningScreen() {
    if (state_.initialScanningScreenShown) {
        return;
    }
    display_.showScanning();
    display_.drawProfileIndicator(settings_.get().activeSlot);
    state_.initialScanningScreenShown = true;
    connectionCadence_.onScanningScreenShown(millis());
}

void DriveRuntime::resetConnectionCadence() {
    connectionCadence_.reset();
}

DriveLoopTiming DriveRuntime::beginDriveLoop() {
    DriveLoopTiming timing;
    timing.loopStartUs = micros();
    audio_process_amp_timeout();
    timing.nowMs = millis();
    return timing;
}

ConnectionRuntimeSnapshot DriveRuntime::processConnectionRuntime(uint32_t nowMs) {
    return connectionRuntime_.process(nowMs, micros(), state_.lastLoopUs, state_.bootSplashHoldActive,
                                      state_.bootSplashHoldUntilMs, state_.initialScanningScreenShown);
}

void DriveRuntime::acceptConnectionSnapshot(const ConnectionRuntimeSnapshot& connection) {
    state_.bootSplashHoldActive = connection.bootSplashHoldActive;
    state_.initialScanningScreenShown = connection.initialScanningScreenShown;
}

void DriveRuntime::markInitialScanningScreenHandled() {
    state_.initialScanningScreenShown = true;
}

bool DriveRuntime::powerOwnsPresentation() const {
    return power_.ownsDisplayPresentation();
}

void DriveRuntime::presentConnectionState(const ConnectionRuntimeSnapshot& connection) {
    DisplayOrchestrationEarlyContext earlyContext;
    earlyContext.bootSplashHoldActive = state_.bootSplashHoldActive;
    earlyContext.overloadThisLoop = connection.overloaded;
    earlyContext.bleContext = {connection.connected, ble_.isProxyClientConnected(), ble_.getConnectionRssi(),
                               ble_.getProxyClientRssi()};
    earlyContext.bleReceiving = connection.receiving;
    displayOrchestration_.processEarly(earlyContext);
}

void DriveRuntime::processPower(uint32_t nowMs) {
    power_.process(nowMs);
}

bool DriveRuntime::processTouch(uint32_t nowMs) {
    const bool inSettings = touchUi_.process(nowMs, digitalRead(BOOT_BUTTON_GPIO) == LOW);
    if (inSettings) {
        tapGesture_.suspendForPresentationOwner();
    }
    return inSettings;
}

void DriveRuntime::servicePowerDisplayOwnership(uint32_t nowMs) {
    if (power_.ownsDisplayPresentation()) {
        if (preview_.isRunning()) {
            preview_.cancel();
        }
        (void)preview_.consumeEnded();
        touchUi_.suspendForPresentationOwner();
        tapGesture_.suspendForPresentationOwner();
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
    if (!touchUi_.restorePresentationIfOwned() && !displayPipeline_.restoreCurrentOwner(nowMs)) {
        display_.showScanning();
        display_.drawProfileIndicator(settings_.get().activeSlot);
    }
    if (restoreBrightness) {
        const uint8_t savedBrightness = settings_.get().brightness;
        display_.setBrightness(savedBrightness);
        Serial.printf("[Power] Restored display brightness=%u after shutdown abort\n",
                      static_cast<unsigned>(savedBrightness));
    }
}

bool DriveRuntime::preemptSettingsForLiveAlert() {
    return hasLiveAlertPresentation() && touchUi_.preemptForLiveAlert();
}

void DriveRuntime::processTapGesture(uint32_t nowMs) {
    const bool alpLiveAlert = alp_.ownsLaserDisplay() && alp_.currentEvent().active;
    tapGesture_.process(nowMs, !alpLiveAlert);
}

void DriveRuntime::openBootReadyGate(uint32_t nowMs) {
    if (!state_.bootReady && nowMs >= state_.bootReadyDeadlineMs) {
        state_.bootReady = true;
        ble_.setBootReady(true);
        Serial.printf("[Boot] Ready gate opened at %lu ms (timeout)\n", static_cast<unsigned long>(nowMs));
    }
}

void DriveRuntime::processBleRuntime() {
    ble_.process(settings_);
}

void DriveRuntime::processBleQueue() {
    bleQueue_.process();
}

bool DriveRuntime::bleQueueBackpressured() const {
    return bleQueue_.isBackpressured();
}

void DriveRuntime::observeAlpProductState(uint32_t nowMs) {
    const AlpStatus status = alp_.snapshot();
    AlpProductObservation observation{};
    observation.connected = alp_.isEnabled() && status.uartActive && status.lastHeartbeatMs != 0 &&
                            static_cast<uint32_t>(nowMs - status.lastHeartbeatMs) <=
                                AlpRuntimeModule::HEARTBEAT_TIMEOUT_MS;
    observation.active = status.hasLaserEvent;
    observation.state = productAlpState(status.lastHbByte1);
    observation.direction = static_cast<uint8_t>(status.laserDirection);
    observation.gun = static_cast<uint8_t>(status.lastGun);
    observation.detectGeneration = status.detectGeneration;
    memcpy(observation.detectRaw, status.detectRaw, sizeof(observation.detectRaw));
    events_.observeAlp(observation, nowMs);
}

void DriveRuntime::processConnectionCycle(uint32_t nowMs, bool bleConnectedNow) {
    const ObdRuntimeStatus obdStatus = obd_.snapshot(nowMs);
    const V1Settings& currentSettings = settings_.get();
    const bool v1GattReady = bleConnectedNow && ble_.getBLEState() == BLEState::CONNECTED;
    const CycleContext cycleContext{
        nowMs,
        state_.bootReady,
        v1GattReady,
        currentSettings.autoPushEnabled,
        ble_.consumeVerifyPushMatchEdge(),
        ble_.lastV1ConnectionEventMs(),
        obdStatus.enabled,
        obdStatus.savedAddressValid,
        obdStatus.connected,
        obdStatus.state,
        obdStatus.speedValid,
        currentSettings.proxyBLE && ble_.isProxyEnabled(),
        ble_.isProxyAdvertising(),
        ble_.isProxyClientConnected(),
        ble_.hasProxyClientConnectedThisBoot(),
        currentSettings.obdScanWindowMs,
        currentSettings.obdRetryIntervalMs,
        currentSettings.proxyOpenWindowMs,
        currentSettings.v1SettleQuietMs,
        currentSettings.v1SettleFallbackMs,
        currentSettings.cycleTeardownAckTimeoutMs,
    };
    connectionCycle_.update(cycleContext);
    ObdBleArbitrationRequest arbitration = connectionCycle_.arbitrationRequest();
    if (obdStatus.manualScanPending) {
        arbitration = ObdBleArbitrationRequest::PREEMPT_PROXY_FOR_MANUAL_SCAN;
    }
    const bool proxyModeEnabled = currentSettings.proxyBLE && ble_.isProxyEnabled();
    ble_.setConnectionCycleProxyPolicy(proxyModeEnabled && connectionCycle_.proxyAdvertisingAllowed(),
                                       proxyModeEnabled && connectionCycle_.proxyKeepConnectionAllowed());
    ble_.setObdBleArbitrationRequest(arbitration);
}

void DriveRuntime::processObd(uint32_t nowMs, bool bleConnectedNow) {
    if (ble_.isProxyClientConnected()) {
        obd_.stopActiveScan();
        obd_.cancelPendingConnect();
    }
    const ObdConnectionState stateBefore = obd_.getState();
    const bool retryAllowed = connectionCycle_.obdRetryAllowed(nowMs);
    const ObdBleContext context{
        state_.bootReady,
        bleConnectedNow,
        !ble_.isScanning(),
        ble_.isConnectBurstSettling(),
        ble_.isProxyAdvertising(),
        ble_.isProxyClientConnected(),
        ble_.isConnectInProgress(),
        connectionCycle_.obdScanAllowed(),
        connectionCycle_.obdConnectAllowed(),
        retryAllowed,
    };
    obd_.update(nowMs, context);
    const ObdConnectionState stateAfter = obd_.getState();
    if (retryAllowed && (stateBefore == ObdConnectionState::DISCONNECTED || stateBefore == ObdConnectionState::ECU_IDLE) &&
        stateAfter == ObdConnectionState::CONNECTING) {
        connectionCycle_.recordObdRetryAttempt(nowMs);
    }
}

void DriveRuntime::processAlp(uint32_t nowMs) {
    alp_.process(nowMs);
}

void DriveRuntime::processAlpPresentationAndPower(uint32_t nowMs) {
    const bool alpLiveAlert = alp_.ownsLaserDisplay() && alp_.currentEvent().active;
    if (alpLiveAlert && preview_.isRunning()) {
        preview_.cancel();
    }
    const AlpStatus alpStatus = alp_.snapshot();
    const bool alpSignalActive =
        alp_.isEnabled() && alpStatus.uartActive && alpStatus.lastHeartbeatMs != 0 &&
        static_cast<uint32_t>(nowMs - alpStatus.lastHeartbeatMs) <= AlpRuntimeModule::HEARTBEAT_TIMEOUT_MS;
    if (alpSignalActive != state_.alpSignalActive) {
        power_.onAlpSignalChange(alpSignalActive);
        state_.alpSignalActive = alpSignalActive;
    }
}

void DriveRuntime::processGps(uint32_t nowMs) {
    gps_.update(nowMs);
}

void DriveRuntime::processSpeed(uint32_t nowMs) {
    speed_.update(nowMs);
}

void DriveRuntime::processSpeedAlert(uint32_t nowMs) {
    const V1Settings& settings = settings_.get();
    speedMute_.syncSettings(settings.speedMuteEnabled, settings.speedMuteThresholdMph,
                            settings.speedMuteHysteresisMph, settings.speedMuteVolume,
                            settings.speedMuteVoice);
    const SpeedSelection speed = speed_.selectedSpeed();
    speedMute_.update(speed.speedMph, speed.valid, nowMs);
}

bool DriveRuntime::hasLiveAlertPresentation() const {
    AlertData v1Priority;
    const bool v1LiveAlert = parser_.hasAlerts() && parser_.getRenderablePriorityAlert(v1Priority);
    const bool alpLiveAlert = alp_.ownsLaserDisplay() && alp_.currentEvent().active;
    return v1LiveAlert || alpLiveAlert;
}

bool DriveRuntime::powerAlertPresentationNeedsRestore() const {
    return power_.criticalBatteryWarningNeedsRestore();
}

void DriveRuntime::servicePowerAlertPresentation(uint32_t nowMs, bool liveAlertPresentation) {
    if (!power_.ownsDisplayPresentation()) {
        return;
    }
    if (liveAlertPresentation) {
        if (!power_.criticalBatteryWarningNeedsRestore() && displayPipeline_.restoreCurrentOwner(nowMs)) {
            power_.noteCriticalBatteryWarningPreempted();
        }
        return;
    }
    power_.restoreCriticalBatteryWarning();
}

DriveRuntime::DisplayEdges DriveRuntime::consumeDisplayEdges() {
    DisplayEdges edges;
    edges.nowMs = millis();
    if (displayPipeline_.consumeAlpPresentationRefreshDue(edges.nowMs)) {
        systemEvents_.publishAlpStateChanged();
    }
    edges.parsedReady = consumeDisplayRefreshEdge(bleQueue_.consumeParsedFlag(), systemEvents_);
    return edges;
}

void DriveRuntime::presentDisplay(const DisplayEdges& edges, bool overloadThisLoop) {
    DisplayOrchestrationParsedContext parsedContext;
    parsedContext.nowMs = edges.nowMs;
    parsedContext.parsedReady = edges.parsedReady;
    parsedContext.bootSplashHoldActive = state_.bootSplashHoldActive;
    bool pipelineRan = false;
    if (displayOrchestration_.processParsedFrame(parsedContext)) {
        displayPipeline_.handleParsed(edges.nowMs);
        pipelineRan = true;
    }

    DisplayOrchestrationRefreshContext refreshContext;
    refreshContext.nowMs = edges.nowMs;
    refreshContext.bootSplashHoldActive = state_.bootSplashHoldActive;
    refreshContext.overloadLateThisLoop = overloadThisLoop;
    refreshContext.pipelineRanThisLoop = pipelineRan;
    if (displayOrchestration_.processLightweightRefresh(refreshContext)) {
        displayPipeline_.refreshBlinkTick(edges.nowMs);
    }
    autoPush_.process();
}

DriveLoopDispatch DriveRuntime::processConnectionDispatch(bool powerPresentationOwned) {
    DriveLoopDispatch dispatch;
    const bool previewOwnsPresentation = preview_.ownsPresentation();
    dispatch.nowMs = millis();
    dispatch.bleConnected = ble_.isConnected();
    ConnectionStateDispatchContext dispatchContext;
    dispatchContext.nowMs = dispatch.nowMs;
    dispatchContext.displayUpdateIntervalMs = DISPLAY_UPDATE_MS;
    dispatchContext.scanScreenDwellMs = state_.activeScanScreenDwellMs;
    dispatchContext.bleConnectedNow = dispatch.bleConnected;
    dispatchContext.bootSplashHoldActive = state_.bootSplashHoldActive;
    dispatchContext.displayPreviewRunning = previewOwnsPresentation || powerPresentationOwned;
    dispatchContext.maxProcessGapMs = kConnectionStateProcessMaxGapMs;
    connectionDispatch_.process(dispatchContext);
    return dispatch;
}

void DriveRuntime::finishSettingsRecapture(
    const V1SettingsOperationStore::Snapshot& operation, uint32_t nowMs) {
    String address;
    if (!connectedV1Address(address) || !settingsOperations_.targetMatches(address.c_str())) return;
    V1DetectorSnapshot snapshot = captureDetectorSnapshot(settingsRecaptureIngressBoundary_);
    const bool complete = settingsSnapshotComplete(snapshot);
    const uint32_t settingsSessionGeneration = ble_.sessionGeneration();
    if (settingsFreshObservationGate_.shouldWait(
            complete, nowMs, kFreshObservationWaitMs, settingsSessionGeneration)) return;
    snapshot.captureTimedOut = snapshot.captureTimedOut || !complete;

    if (!devices_.recordSnapshotInMemory(address, snapshot)) {
        (void)settingsOperations_.finish(V1SettingsOperationStore::State::Failed,
            V1SettingsOperationStore::Reason::SnapshotStoreUnavailable);
        return;
    }
    bool flushed = false;
    if (storage_.isSDCard()) {
        StorageManager::SDTryLock lock(storage_.getSDMutex(), /*checkDmaHeap=*/false);
        flushed = lock && devices_.flushPendingSave();
    } else {
        flushed = devices_.flushPendingSave();
    }
    if (!flushed) {
        (void)settingsOperations_.finish(V1SettingsOperationStore::State::Failed,
            V1SettingsOperationStore::Reason::SnapshotPersistFailed);
        return;
    }

    V1SettingsOperationStore::State terminal = V1SettingsOperationStore::State::Succeeded;
    V1SettingsOperationStore::Reason reason = V1SettingsOperationStore::Reason::None;
    bool preserveInterruptedFactoryTruth = false;
    if (operation.kind == V1SettingsOperationStore::Kind::FactoryReset) {
        if (!V1SettingsOperationStore::hasDurableFactoryResetSend(operation)) {
            // A reset may have reached the detector before power loss, but no
            // durable record proves it. Fresh factory-looking values cannot be
            // promoted into evidence of that destructive send.
            terminal = V1SettingsOperationStore::State::Partial;
            reason = V1SettingsOperationStore::Reason::Interrupted;
            preserveInterruptedFactoryTruth = true;
        } else {
            auto components = operation.components;
            auto& user = components[static_cast<size_t>(V1SettingsOperationStore::Component::UserSettings)];
            const bool userDefaultsMatch = V1SettingsOperationStore::factoryUserDefaultsMatch(
                snapshot.userBytes, snapshot.hasUserBytes);
            if (userDefaultsMatch) {
                user.verified = true;
                user.outcome = V1SettingsOperationStore::ComponentOutcome::Verified;
                user.reason = V1SettingsOperationStore::ComponentReason::None;
            } else if (snapshot.hasUserBytes) {
                user.verified = false;
                user.outcome = V1SettingsOperationStore::ComponentOutcome::Mismatch;
                user.reason = V1SettingsOperationStore::ComponentReason::FactoryUserDefaultsMismatch;
            }
            if (!settingsOperations_.updateComponents(components, 0)) return;
            terminal = V1SettingsOperationStore::State::Partial;
            reason = snapshot.hasUserBytes && !userDefaultsMatch
                ? V1SettingsOperationStore::Reason::FactoryResetDefaultsMismatch
                : V1SettingsOperationStore::Reason::FactoryResetScopeUnverified;
        }
    } else if (operation.reason == V1SettingsOperationStore::Reason::ApplyFailed) {
        terminal = V1SettingsOperationStore::State::Failed;
        reason = operation.reason;
    } else if (operation.reason == V1SettingsOperationStore::Reason::ApplyPartial ||
               operation.reason == V1SettingsOperationStore::Reason::Interrupted) {
        terminal = V1SettingsOperationStore::State::Partial;
        reason = operation.reason;
    }
    if (!complete && terminal != V1SettingsOperationStore::State::Failed &&
        !preserveInterruptedFactoryTruth) {
        terminal = V1SettingsOperationStore::State::Partial;
        reason = V1SettingsOperationStore::Reason::RecaptureTimedOut;
    }
    (void)settingsOperations_.finish(terminal, reason);
}

void DriveRuntime::processSettingsOperation(uint32_t nowMs) {
    if (settingsWrongDetectorDisconnectPending_) {
        settingsWrongDetectorDisconnectPending_ = false;
        ble_.disconnect();
        return;
    }
    const V1SettingsOperationStore::Snapshot initial = settingsOperations_.snapshot();
    if (!initial.available || !initial.valid) return;
    if (initial.operationId != observedSettingsOperationId_) {
        observedSettingsOperationId_ = initial.operationId;
        observedSettingsOperationState_ = initial.state;
        settingsOperationStartedMs_ = nowMs;
        settingsOperationStateStartedMs_ = nowMs;
        settingsOperationNextActionMs_ = 0;
        settingsRecaptureIngressBoundary_ = 0;
        settingsRecaptureStarted_ = false;
        settingsFreshObservationGate_.reset();
        factoryResetSendLatch_.reset();
        factoryResetSummaryPersistedThisBoot_ = false;
        factoryResetSentComponents_ = {};
        settingsWrongDetectorDisconnectPending_ = false;
        settingsReturnRetry_.clear();
    } else if (initial.state != observedSettingsOperationState_) {
        observedSettingsOperationState_ = initial.state;
        settingsOperationStateStartedMs_ = nowMs;
        settingsOperationNextActionMs_ = 0;
        if (initial.state == V1SettingsOperationStore::State::Recapturing) {
            settingsRecaptureIngressBoundary_ = 0;
            settingsRecaptureStarted_ = false;
            settingsFreshObservationGate_.reset();
        }
    }

    if (settingsOperations_.isTerminal()) {
        if (initial.returnToMaintenance && settingsReturnRetry_.due(nowMs)) {
            settingsReturnRetry_.defer(nowMs);
            requestMaintenanceBootRestart();
        }
        return;
    }

    constexpr uint32_t kDetectorWaitTimeoutMs = 60000u;
    constexpr uint32_t kOperationTimeoutMs = 90000u;
    if ((initial.state == V1SettingsOperationStore::State::WaitingForDetector ||
         initial.state == V1SettingsOperationStore::State::Preparing) &&
        static_cast<uint32_t>(nowMs - settingsOperationStartedMs_) >= kDetectorWaitTimeoutMs) {
        (void)settingsOperations_.finish(V1SettingsOperationStore::State::Failed,
            V1SettingsOperationStore::Reason::DetectorTimeout);
        return;
    }
    if (static_cast<uint32_t>(nowMs - settingsOperationStartedMs_) >= kOperationTimeoutMs) {
        const auto terminal = initial.state == V1SettingsOperationStore::State::Recapturing
            ? V1SettingsOperationStore::State::Partial
            : V1SettingsOperationStore::State::Failed;
        (void)settingsOperations_.finish(terminal,
            initial.state == V1SettingsOperationStore::State::Recapturing
                ? V1SettingsOperationStore::Reason::RecaptureTimedOut
                : V1SettingsOperationStore::Reason::DetectorTimeout);
        return;
    }

    String address;
    const bool matchingDetector = connectedV1Address(address) &&
        settingsOperations_.targetMatches(address.c_str());
    const uint32_t settingsSessionGeneration = ble_.sessionGeneration();
    if (matchingDetector &&
        settingsFreshObservationGate_.observeSession(settingsSessionGeneration) &&
        (initial.state == V1SettingsOperationStore::State::Preparing ||
         initial.state == V1SettingsOperationStore::State::Recapturing)) {
        // A recapture started for an earlier BLE session cannot establish
        // freshness for this one. Either the new connection follow-up will
        // complete first, or this state starts a fresh explicit recapture.
        settingsRecaptureIngressBoundary_ = 0;
        settingsRecaptureStarted_ = false;
    }
    if ((initial.state == V1SettingsOperationStore::State::WaitingForDetector ||
         initial.state == V1SettingsOperationStore::State::Preparing) &&
        matchingDetector && settingsFreshObservationGate_.completeFor(settingsSessionGeneration)) {
        const uint32_t requiredBoundary = V1SettingsOperationStore::requiredPreApplyIngressBoundary(
            initial.state, settingsRecaptureIngressBoundary_);
        const V1DetectorSnapshot snapshot = captureDetectorSnapshot(requiredBoundary);
        const bool complete = settingsSnapshotComplete(snapshot);
        if (settingsFreshObservationGate_.shouldWait(
                complete, nowMs, kFreshObservationWaitMs, settingsSessionGeneration)) return;
        if (!persistDetectorSnapshot(address, snapshot, false)) {
            (void)settingsOperations_.finish(V1SettingsOperationStore::State::Failed,
                V1SettingsOperationStore::Reason::SnapshotStoreUnavailable);
            return;
        }
        if (initial.kind == V1SettingsOperationStore::Kind::FactoryReset) {
            auto components = initial.components;
            for (auto& component : components) {
                component.requested = true;
                component.outcome = V1SettingsOperationStore::ComponentOutcome::Pending;
                component.reason = V1SettingsOperationStore::ComponentReason::None;
            }
            if (!settingsOperations_.updateComponents(components, 0)) return;
            if (!settingsOperations_.markRunning(0)) return;
            attemptFactoryReset(settingsOperations_.snapshot());
        } else {
            queueSettingsApply(initial, snapshot);
        }
        return;
    }

    if (initial.state == V1SettingsOperationStore::State::Preparing && matchingDetector &&
        !settingsRecaptureStarted_) {
        settingsRecaptureIngressBoundary_ = ble_.latestV1NotificationIngressSequence();
        if (ble_.beginSettingsRecapture()) {
            settingsFreshObservationGate_.beginCapture(settingsSessionGeneration);
            settingsRecaptureStarted_ = true;
            settingsOperationStateStartedMs_ = nowMs;
        }
        return;
    }

    if (initial.state == V1SettingsOperationStore::State::Running) {
        if (initial.kind == V1SettingsOperationStore::Kind::FactoryReset) {
            if (matchingDetector &&
                (settingsOperationNextActionMs_ == 0 ||
                 static_cast<int32_t>(nowMs - settingsOperationNextActionMs_) >= 0)) {
                settingsOperationNextActionMs_ = nowMs + 50u;
                attemptFactoryReset(initial);
            }
            return;
        }
        const AutoPushModule::ExecutionSummary summary = autoPush_.executionSummary();
        if (summary.operationId == 0 ||
            (initial.executorOperationId != 0 && summary.operationId != initial.executorOperationId)) return;
        bool changed = initial.executorOperationId != summary.operationId;
        for (size_t index = 0; index < V1SettingsOperationStore::kComponentCount && !changed; ++index) {
            const auto& left = initial.components[index];
            const auto& right = summary.components[index];
            changed = left.requested != right.requested || left.sent != right.sent ||
                      left.verified != right.verified || left.outcome != right.outcome ||
                      left.reason != right.reason;
        }
        if (changed && !settingsOperations_.updateComponents(summary.components, summary.operationId)) return;
        if (summary.active || summary.result == AutoPushModule::PublicResult::Queued ||
            summary.result == AutoPushModule::PublicResult::InProgress) return;
        V1SettingsOperationStore::Reason reason = V1SettingsOperationStore::Reason::ApplyFailed;
        if (summary.result == AutoPushModule::PublicResult::Succeeded) {
            reason = V1SettingsOperationStore::Reason::None;
        } else if (summary.result == AutoPushModule::PublicResult::Partial) {
            reason = V1SettingsOperationStore::Reason::ApplyPartial;
        }
        (void)settingsOperations_.markRecapturing(reason);
        return;
    }

    if (initial.state == V1SettingsOperationStore::State::Recapturing) {
        if (!matchingDetector) return;
        if (!settingsRecaptureStarted_ &&
            !settingsFreshObservationGate_.completeFor(settingsSessionGeneration)) {
            settingsRecaptureIngressBoundary_ = ble_.latestV1NotificationIngressSequence();
            if (ble_.beginSettingsRecapture()) {
                settingsFreshObservationGate_.beginCapture(settingsSessionGeneration);
                settingsRecaptureStarted_ = true;
                settingsOperationStateStartedMs_ = nowMs;
            }
            return;
        }
        if (settingsFreshObservationGate_.completeFor(settingsSessionGeneration)) {
            finishSettingsRecapture(initial, nowMs);
        }
    }
}

void DriveRuntime::processPeriodicMaintenance(uint32_t nowMs, bool bleConnected, bool bleBackpressure,
                                              bool loopOverloaded, bool forceTailBleDrainPending) {
    processSettingsOperation(nowMs);
    const bool hardPressure = bleBackpressure || loopOverloaded || forceTailBleDrainPending;
    usbTailBusy_ = hardPressure;
    if (!bleConnected) {
        connectedPersistenceWindowAnchored_ = false;
        connectedPersistenceWindowStartedMs_ = 0;
    } else if (!connectedPersistenceWindowAnchored_) {
        connectedPersistenceWindowAnchored_ = true;
        connectedPersistenceWindowStartedMs_ = nowMs;
    }
    const bool connectedPersistenceDue =
        connectedPersistenceWindowAnchored_ &&
        static_cast<uint32_t>(nowMs - connectedPersistenceWindowStartedMs_) >= kConnectedPersistenceDeferralMs;
    const bool admitPersistence = !hardPressure && (!bleConnected || connectedPersistenceDue);

    obdSettingsSync_.process(nowMs);
    if (admitPersistence) {
        settings_.serviceDeferredPersist(nowMs);
        settings_.serviceDeferredBackup(nowMs);
    }
    if (!hardPressure) {
        ble_.serviceDeferredBondBackup(nowMs);
    }
    if (admitPersistence) {
        processV1DeviceStoreSave(nowMs, storage_, devices_);
    }
    if (bleConnected && admitPersistence) {
        connectedPersistenceWindowStartedMs_ = nowMs;
    }
    if (!hardPressure) {
        logDisplayConfiguration(millis());
    }
}

void DriveRuntime::logDisplayConfiguration(uint32_t nowMs) {
    if (static_cast<uint32_t>(nowMs - lastDisplayConfigurationLogMs_) < 1000u) {
        return;
    }
    lastDisplayConfigurationLogMs_ = nowMs;
    const V1Settings& settings = settings_.get();
    const unsigned activeSlot = V1Settings::normalizeAutoPushSlotIndex(settings.activeSlot);
    char line[192];
    const int length = snprintf(
        line, sizeof(line),
        "CFG bootId=%lu uptimeMs=%lu revision=%lu activeSlot=%u stealthEnabled=%u "
        "priorityArrowOnly=%u alertPersistenceSeconds=%u\n",
        static_cast<unsigned long>(bootId_), static_cast<unsigned long>(nowMs),
        static_cast<unsigned long>(settings_.displayConfigurationRevision()), activeSlot,
        static_cast<unsigned>(settings.stealthEnabled),
        static_cast<unsigned>(settings_.getSlotPriorityArrowOnly(activeSlot)),
        static_cast<unsigned>(settings_.getSlotAlertPersistSec(activeSlot)));
    // Serial already has a zero TX timeout. Skip unavailable space instead of
    // delaying alert work; the collector must retain missing evidence as unknown.
    if (length > 0 && static_cast<size_t>(length) < sizeof(line) && Serial.availableForWrite() >= length) {
        Serial.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(length));
    }
}

uint32_t DriveRuntime::finishLoop(bool bleBackpressure, uint32_t loopStartUs, bool forceBleDrain) {
    if (bleBackpressure || forceBleDrain) {
        bleQueue_.process();
    }
    vTaskDelay(pdMS_TO_TICKS(1));
    return static_cast<uint32_t>(micros() - loopStartUs);
}

void DriveRuntime::finishDriveLoop(bool bleBackpressure, uint32_t loopStartUs, bool forceBleDrain) {
    state_.lastLoopUs = finishLoop(bleBackpressure, loopStartUs, forceBleDrain);
}

void DriveRuntime::tick() {
    DriveLoopCoordinator::tick(*this);
}

bool DriveRuntime::preparePersistenceForShutdownPhase() {
    return preparePersistenceForShutdown(events_, health_, settings_);
}

void DriveRuntime::disconnectDriveBleForShutdown() {
    Serial.println("[Battery] Disconnecting BLE peripherals before shutdown...");
    ble_.disconnect();
}

void DriveRuntime::disconnectDriveObdForShutdown() {
    if (!obd_.disconnectForShutdown(100)) {
        Serial.println("[Battery] WARN: OBD transport disconnect did not acknowledge before shutdown");
    }
}

void DriveRuntime::stopDriveBleScanForShutdown() {
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan && scan->isScanning()) {
        scan->stop();
    }
}

void DriveRuntime::settleDriveShutdownTransport() {
    delay(50);
}

void DriveRuntime::writeCleanShutdownMarker() {
    Serial.println("[Battery] Writing clean-shutdown marker...");
    markCleanShutdown();
}

void DriveRuntime::resumePersistenceAfterAbortedShutdownPhase() {
    resumePersistenceAfterAbortedShutdown(events_, health_);
}

void DriveRuntime::resumeDriveBleAfterAbortedShutdown() {
    ble_.startScanning();
}

void DriveRuntime::prepareForShutdown() {
    RuntimeServiceLifecycleCoordinator::prepareDrive(*this);
}

void DriveRuntime::resumeAfterAbortedShutdown() {
    RuntimeServiceLifecycleCoordinator::resumeDrive(*this);
}

void DriveRuntime::stop() {
    if (!active_) {
        return;
    }
    ble_.disconnect();
    obd_.disconnectForShutdown(100);
    active_ = false;
    if (callbackOwner_ == this) {
        callbackOwner_ = nullptr;
    }
}

void DriveRuntime::onV1Data(const uint8_t* data, size_t length, uint16_t charUuid,
                            uint32_t sessionGeneration, uint32_t callbackMillis,
                            uint32_t ingressSequence) {
    if (callbackOwner_) {
        callbackOwner_->bleQueue_.onNotify(data, length, charUuid, sessionGeneration, callbackMillis,
                                           ingressSequence);
    }
}

void DriveRuntime::onV1ConnectImmediate() {
    if (!callbackOwner_) {
        return;
    }
    auto& self = *callbackOwner_;
    self.state_.v1ConnectedAtMs = millis();
    self.connectionState_.handleConnected(self.state_.v1ConnectedAtMs, self.ble_.sessionGeneration());
    self.events_.observeV1Link(true, self.state_.v1ConnectedAtMs);
}

void DriveRuntime::onV1SessionOpened(uint32_t sessionGeneration) {
    if (callbackOwner_) {
        callbackOwner_->connectionState_.handleSessionOpened(sessionGeneration);
    }
}

void DriveRuntime::onV1SessionClosed(uint32_t sessionGeneration) {
    if (!callbackOwner_) {
        return;
    }
    const uint32_t nowMs = millis();
    callbackOwner_->events_.observeV1Link(false, nowMs);
    callbackOwner_->connectionState_.handleSessionClosed(nowMs, sessionGeneration);
}

bool DriveRuntime::connectedV1Address(String& address) const {
    address = String();
    const NimBLEAddress connected = ble_.getConnectedAddress();
    if (connected.isNull()) return false;
    try {
        const std::string raw = connected.toString();
        if (raw.size() != 17u) return false;
        String staged(raw.c_str());
        if (staged.length() != raw.size() ||
            std::memcmp(staged.c_str(), raw.data(), raw.size()) != 0) return false;
        address = normalizeV1DeviceAddress(staged);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return address.length() == 17u && V1SettingsOperationStore::isCanonicalAddress(address.c_str());
}

V1DetectorSnapshot DriveRuntime::captureDetectorSnapshot(uint32_t ingressBoundary) const {
    const auto afterBoundary = [ingressBoundary](uint32_t ingress) {
        return ingress != 0 && (ingressBoundary == 0 ||
               static_cast<int32_t>(ingress - ingressBoundary) > 0);
    };
    V1DetectorSnapshot snapshot;
    snapshot.available = true;
    snapshot.capturedBootId = bootId_;
    snapshot.capturedUptimeMs = static_cast<uint32_t>(millis());
    snapshot.sessionGeneration = ble_.sessionGeneration();
    snapshot.captureTimedOut = ble_.settingsCaptureTimedOut();
    snapshot.firmwareVersion = ble_.v1FirmwareVersion();
    snapshot.hasFirmwareVersion = snapshot.firmwareVersion != 0;
    snapshot.hasUserBytes = ble_.sessionUserBytesRevision() > 0 &&
                            afterBoundary(ble_.sessionUserBytesIngressSequence()) &&
                            ble_.copySessionUserBytes(snapshot.userBytes.data());

    const V1DisplayOnObservation& display = parser_.displayOnObservation();
    snapshot.hasDisplayOn = display.available && display.revision > 0 && afterBoundary(display.ingressSequence);
    snapshot.displayOn = display.value;
    const V1BluetoothIndicatorObservation& bluetooth = parser_.bluetoothIndicatorObservation();
    snapshot.hasBluetoothIndicator = bluetooth.available && bluetooth.revision > 0 &&
                                     afterBoundary(bluetooth.ingressSequence);
    snapshot.bluetoothIndicator = bluetooth.state;
    const V1ModeObservation& mode = parser_.modeObservation();
    snapshot.hasMode = mode.available && mode.revision > 0 && afterBoundary(mode.ingressSequence);
    snapshot.mode = mode.value;

    const V1AllVolumeObservation& volume = parser_.allVolumeObservation();
    const bool capturedVolume = ble_.hasSessionAllVolume() && volume.available &&
                                volume.revision > 0 && afterBoundary(volume.ingressSequence);
    snapshot.hasCurrentVolume = capturedVolume;
    snapshot.currentMainVolume = volume.currentMain;
    snapshot.currentMutedVolume = volume.currentMuted;
    snapshot.hasSavedVolume = capturedVolume;
    snapshot.savedMainVolume = volume.savedMain;
    snapshot.savedMutedVolume = volume.savedMuted;

    const V1SweepSectionsObservation& sections = parser_.sweepSectionsObservation();
    snapshot.hasSweepSections = ble_.hasSessionSweepSectionsCapture() && sections.available &&
                                sections.complete && afterBoundary(sections.ingressSequence);
    if (snapshot.hasSweepSections) {
        snapshot.sweepSectionCount = sections.count;
        snapshot.sweepSections = sections.sections;
    }
    const V1SweepMaxObservation& maxSweep = parser_.sweepMaxObservation();
    snapshot.hasMaxSweepIndex = ble_.hasSessionSweepMaxCapture() && maxSweep.available &&
                                !maxSweep.poisoned && afterBoundary(maxSweep.ingressSequence);
    snapshot.maxSweepIndex = maxSweep.maxIndex;
    const V1SweepDefinitionsObservation& definitions = parser_.sweepDefinitionsObservation();
    if (snapshot.hasMaxSweepIndex && ble_.hasSessionSweepDefinitionsCapture() &&
        afterBoundary(definitions.ingressSequence)) {
        const uint64_t required = snapshot.maxSweepIndex == 63
                                      ? UINT64_MAX
                                      : ((uint64_t{1} << (snapshot.maxSweepIndex + 1u)) - 1u);
        snapshot.hasSweepDefinitions = !definitions.poisoned && definitions.presentMask == required;
        for (uint8_t index = 0; snapshot.hasSweepDefinitions && index <= snapshot.maxSweepIndex; ++index) {
            snapshot.hasSweepDefinitions = afterBoundary(definitions.ingressSequences[index]);
        }
        if (snapshot.hasSweepDefinitions) snapshot.sweepDefinitions = definitions.definitions;
    }
    return snapshot;
}

bool DriveRuntime::persistDetectorSnapshot(const String& address,
                                           const V1DetectorSnapshot& snapshot,
                                           bool flushImmediately) {
    if (!devices_.recordSnapshotInMemory(address, snapshot)) return false;
    if (!flushImmediately) return true;
    if (storage_.isSDCard()) {
        StorageManager::SDTryLock lock(storage_.getSDMutex(), /*checkDmaHeap=*/false);
        return lock && devices_.flushPendingSave();
    }
    return devices_.flushPendingSave();
}

bool DriveRuntime::beginTripleTapProfileCycle(int newSlot) {
    if (newSlot < 0 || newSlot > 2) return false;
    const V1Settings& current = settings_.get();
    if (!ble_.isConnected() || !current.autoPushEnabled) {
        if (!settings_.setActiveSlot(newSlot,
                SettingsPersistMode::ImmediateNvsDeferredBackup).success) return false;
        displayMode_ = DisplayMode::IDLE;
        alertPersistence_.clearPersistence();
        display_.drawProfileIndicator(newSlot);
        Serial.printf("PROFILE CHANGE: local slot %d (no detector apply)\n", newSlot);
        return true;
    }
    if (autoPush_.isActive() || settingsOperations_.isActive()) return false;
    String address;
    if (!connectedV1Address(address)) return false;
    const auto started = settingsOperations_.startApply(
        newSlot, address.c_str(), V1SettingsOperationStore::Source::TripleTap,
        false, false);
    if (started.status != V1SettingsOperationStore::StartStatus::Started) return false;
    if (!settingsOperations_.markPreparing()) {
        (void)settingsOperations_.finish(V1SettingsOperationStore::State::Failed,
                                         V1SettingsOperationStore::Reason::StorageUnavailable);
        return false;
    }
    observedSettingsOperationId_ = 0;
    return true;
}

void DriveRuntime::queueSettingsApply(const V1SettingsOperationStore::Snapshot& operation,
                                      const V1DetectorSnapshot& preApply) {
    autoPush_.setPreApplySnapshot(preApply);
    if (!settingsOperations_.markRunning(0)) return;
    AutoPushModule::QueueResult queued = AutoPushModule::QueueResult::STAGING_UNAVAILABLE;
    if (operation.kind == V1SettingsOperationStore::Kind::ApplySlot) {
        const bool activate = operation.source == V1SettingsOperationStore::Source::TripleTap;
        queued = autoPush_.queueSlotPush(operation.slot, activate, false);
    } else {
        AutoPushModule::PushNowRequest request;
        request.slotIndex = settings_.get().activeSlot;
        request.hasProfileOverride = true;
        request.profileName = operation.profileName;
        queued = autoPush_.queuePushNow(request);
    }
    if (queued != AutoPushModule::QueueResult::QUEUED) {
        (void)settingsOperations_.finish(V1SettingsOperationStore::State::Failed,
                                         AutoPushModule::durableReasonForQueueResult(queued));
        return;
    }
    if (operation.source == V1SettingsOperationStore::Source::TripleTap) {
        displayMode_ = DisplayMode::IDLE;
        alertPersistence_.clearPersistence();
        display_.drawProfileIndicator(operation.slot);
    }
    const AutoPushModule::ExecutionSummary summary = autoPush_.executionSummary();
    (void)settingsOperations_.updateComponents(summary.components, summary.operationId);
}

void DriveRuntime::attemptFactoryReset(const V1SettingsOperationStore::Snapshot& operation) {
    if (operation.kind != V1SettingsOperationStore::Kind::FactoryReset || !ble_.isConnected()) return;
    if (factoryResetSendLatch_.sent()) {
        if (!factoryResetSummaryPersistedThisBoot_) {
            if (!settingsOperations_.updateComponents(factoryResetSentComponents_, 0)) return;
            factoryResetSummaryPersistedThisBoot_ = true;
        }
        (void)settingsOperations_.markRecapturing(
            V1SettingsOperationStore::Reason::FactoryResetScopeUnverified);
        return;
    }
    auto components = operation.components;
    const SendResult result = ble_.factoryResetDetector();
    if (result == SendResult::NOT_YET) return;
    if (result != SendResult::SENT) {
        for (size_t index = 0; index < V1SettingsOperationStore::kComponentCount; ++index) {
            components[index].outcome = index == static_cast<size_t>(V1SettingsOperationStore::Component::FactoryReset)
                ? V1SettingsOperationStore::ComponentOutcome::WriteFailed
                : V1SettingsOperationStore::ComponentOutcome::Blocked;
            components[index].reason = V1SettingsOperationStore::ComponentReason::FactoryResetWriteFailed;
        }
        (void)settingsOperations_.updateComponents(components, 0);
        (void)settingsOperations_.finish(V1SettingsOperationStore::State::Failed,
            V1SettingsOperationStore::Reason::FactoryResetSendFailed);
        return;
    }
    for (size_t index = 0; index < V1SettingsOperationStore::kComponentCount; ++index) {
        components[index].sent = true;
        components[index].verified = false;
        components[index].outcome = V1SettingsOperationStore::ComponentOutcome::Sent;
        components[index].reason = index == static_cast<size_t>(V1SettingsOperationStore::Component::FactoryReset)
            ? V1SettingsOperationStore::ComponentReason::None
            : V1SettingsOperationStore::ComponentReason::FactoryDefaultUnverified;
    }
    // Latch before either persistence write. A returned SENT must never lead
    // to a second destructive frame merely because NVS later failed.
    factoryResetSendLatch_.noteSent();
    factoryResetSentComponents_ = components;
    if (!settingsOperations_.updateComponents(components, 0)) return;
    factoryResetSummaryPersistedThisBoot_ = true;
    (void)settingsOperations_.markRecapturing(
        V1SettingsOperationStore::Reason::FactoryResetScopeUnverified);
}

bool DriveRuntime::handleSettingsOperationStableConnection() {
    if (!settingsOperations_.requiresExclusiveDetectorAdmission()) return false;
    String address;
    if (!connectedV1Address(address) || !settingsOperations_.targetMatches(address.c_str())) {
        // A target-bound job owns detector mutation admission until it finishes
        // and any durable return-to-maintenance intent has been acknowledged.
        // Ordinary Auto-Push must not mutate another V1 in that interval.
        settingsWrongDetectorDisconnectPending_ = ble_.isConnected();
        return true;
    }
    const V1SettingsOperationStore::Snapshot operation = settingsOperations_.snapshot();
    if (operation.state == V1SettingsOperationStore::State::Recapturing) {
        settingsFreshObservationGate_.noteFollowupComplete(
            static_cast<uint32_t>(millis()), ble_.sessionGeneration());
        return true;
    }
    if (operation.state != V1SettingsOperationStore::State::WaitingForDetector &&
        operation.state != V1SettingsOperationStore::State::Preparing) return true;
    settingsFreshObservationGate_.noteFollowupComplete(
        static_cast<uint32_t>(millis()), ble_.sessionGeneration());
    return true;
}

void DriveRuntime::onV1Connected() {
    if (!callbackOwner_) {
        return;
    }
    auto& self = *callbackOwner_;
    if (self.handleSettingsOperationStableConnection()) return;
    const V1Settings& settings = self.settings_.get();
    const int activeSlot = std::max(0, std::min(2, settings.activeSlot));
    int selectedSlot = activeSlot;
    uint8_t defaultProfile = 0;
    String linkAddress;
    NimBLEAddress connected = self.ble_.getConnectedAddress();
    if (!connected.isNull()) {
        bool exactLiveIdentity = false;
        try {
            const std::string rawAddress = connected.toString();
            if (rawAddress.size() == 17u) {
                String staged(rawAddress.c_str());
                if (staged.length() == rawAddress.size() &&
                    std::memcmp(staged.c_str(), rawAddress.data(), rawAddress.size()) == 0) {
                    String normalized = normalizeV1DeviceAddress(staged);
                    if (normalized.length() == 17u) {
                        linkAddress = std::move(normalized);
                        exactLiveIdentity = linkAddress.length() == 17u;
                    }
                }
            }
        } catch (const std::bad_alloc&) {
            exactLiveIdentity = false;
        }
        if (!exactLiveIdentity) {
            Serial.println("[AutoPush] NOT_QUEUED reason=device_identity_unavailable");
            return;
        }
    }

    String fallbackAddress;
    const String* profileAddress = &linkAddress;
    if (connected.isNull()) {
        profileAddress = &fallbackAddress;
        if (settings.lastV1Address.length() != 0) {
            if (settings.lastV1Address.length() != 17u) {
                Serial.println("[AutoPush] NOT_QUEUED reason=device_identity_unavailable");
                return;
            }
            fallbackAddress = normalizeV1DeviceAddress(settings.lastV1Address);
            if (fallbackAddress.length() != 17u) {
                Serial.println("[AutoPush] NOT_QUEUED reason=device_identity_unavailable");
                return;
            }
        }
    }
    if (profileAddress->length() > 0) {
        const V1DeviceDefaultProfileResult lookup =
            self.devices_.getDeviceDefaultProfileChecked(*profileAddress);
        if (lookup.status == V1DeviceDefaultProfileStatus::Unavailable ||
            !self.devices_.touchDeviceInMemory(*profileAddress)) {
            Serial.println("[AutoPush] NOT_QUEUED reason=device_catalog_unavailable");
            return;
        }
        if (lookup.status == V1DeviceDefaultProfileStatus::Found) {
            defaultProfile = lookup.profile;
            selectedSlot = static_cast<int>(defaultProfile) - 1;
        }
    } else if (settings.autoPushEnabled) {
        Serial.println("[AutoPush] NOT_QUEUED reason=device_identity_unavailable");
        return;
    }
    if (linkAddress.length() > 0) {
        self.settings_.setLastV1Address(linkAddress);
        if (!self.devices_.isReady()) {
            self.settings_.requestLastV1AddressFallbackPersist(linkAddress);
        }
    }

    // The stable callback runs only after the bounded connect-followup read.
    // Copy the detector observation into the address-keyed store before
    // Auto-Push can write any desired profile state back to the V1.
    V1DetectorSnapshot snapshot;
    snapshot.available = true;
    snapshot.capturedBootId = self.bootId_;
    snapshot.capturedUptimeMs = static_cast<uint32_t>(millis());
    snapshot.sessionGeneration = self.ble_.sessionGeneration();
    snapshot.captureTimedOut = self.ble_.settingsCaptureTimedOut();
    snapshot.firmwareVersion = self.ble_.v1FirmwareVersion();
    snapshot.hasFirmwareVersion = snapshot.firmwareVersion != 0;
    snapshot.hasUserBytes = self.ble_.sessionUserBytesRevision() > 0 &&
                            self.ble_.sessionUserBytesIngressSequence() > 0 &&
                            self.ble_.copySessionUserBytes(snapshot.userBytes.data());

    const V1DisplayOnObservation& displayObservation = self.parser_.displayOnObservation();
    snapshot.hasDisplayOn = displayObservation.available && displayObservation.revision > 0 &&
                            displayObservation.ingressSequence > 0;
    snapshot.displayOn = displayObservation.value;
    const V1BluetoothIndicatorObservation& bluetoothObservation = self.parser_.bluetoothIndicatorObservation();
    snapshot.hasBluetoothIndicator = bluetoothObservation.available && bluetoothObservation.revision > 0 &&
                                     bluetoothObservation.ingressSequence > 0;
    snapshot.bluetoothIndicator = bluetoothObservation.state;

    const V1ModeObservation& modeObservation = self.parser_.modeObservation();
    snapshot.hasMode = modeObservation.available && modeObservation.revision > 0 &&
                       modeObservation.ingressSequence > 0;
    snapshot.mode = modeObservation.value;

    const V1AllVolumeObservation& allVolumeObservation = self.parser_.allVolumeObservation();
    const bool capturedAllVolume = self.ble_.hasSessionAllVolume() && allVolumeObservation.available &&
                                   allVolumeObservation.revision > 0 && allVolumeObservation.ingressSequence > 0;
    snapshot.hasCurrentVolume = capturedAllVolume;
    snapshot.currentMainVolume = allVolumeObservation.currentMain;
    snapshot.currentMutedVolume = allVolumeObservation.currentMuted;
    snapshot.hasSavedVolume = capturedAllVolume;
    snapshot.savedMainVolume = allVolumeObservation.savedMain;
    snapshot.savedMutedVolume = allVolumeObservation.savedMuted;
    const V1SweepSectionsObservation& sweepSections = self.parser_.sweepSectionsObservation();
    snapshot.hasSweepSections = self.ble_.hasSessionSweepSectionsCapture() &&
                                sweepSections.available && sweepSections.complete &&
                                sweepSections.ingressSequence > 0;
    if (snapshot.hasSweepSections) {
        snapshot.sweepSectionCount = sweepSections.count;
        snapshot.sweepSections = sweepSections.sections;
    }
    const V1SweepMaxObservation& sweepMax = self.parser_.sweepMaxObservation();
    snapshot.hasMaxSweepIndex = self.ble_.hasSessionSweepMaxCapture() &&
                                sweepMax.available && !sweepMax.poisoned &&
                                sweepMax.ingressSequence > 0;
    snapshot.maxSweepIndex = sweepMax.maxIndex;
    const V1SweepDefinitionsObservation& sweepDefinitions = self.parser_.sweepDefinitionsObservation();
    if (snapshot.hasMaxSweepIndex && self.ble_.hasSessionSweepDefinitionsCapture() &&
        sweepDefinitions.ingressSequence > 0) {
        const uint64_t required = snapshot.maxSweepIndex == 63
                                      ? UINT64_MAX
                                      : ((uint64_t{1} << (snapshot.maxSweepIndex + 1u)) - 1u);
        snapshot.hasSweepDefinitions = !sweepDefinitions.poisoned && sweepDefinitions.presentMask == required;
        for (uint8_t index = 0; snapshot.hasSweepDefinitions && index <= snapshot.maxSweepIndex; ++index) {
            const uint32_t ingress = sweepDefinitions.ingressSequences[index];
            const uint32_t boundary = self.ble_.sessionSweepDefinitionsIngressBoundary();
            snapshot.hasSweepDefinitions = ingress != 0 && boundary != 0 &&
                                           static_cast<int32_t>(ingress - boundary) > 0;
        }
        if (snapshot.hasSweepDefinitions) snapshot.sweepDefinitions = sweepDefinitions.definitions;
    }
    self.autoPush_.setPreApplySnapshot(snapshot);

    if (linkAddress.length() > 0 && self.devices_.isReady()) {
        if (!self.devices_.recordSnapshotInMemory(linkAddress, snapshot)) {
            Serial.println("[AutoPush] NOT_QUEUED reason=device_snapshot_unavailable");
            return;
        }
    }

    const AutoPushSlot& slot = self.settings_.getSlot(selectedSlot);
    Serial.printf("[AutoPush] onV1Connected autoPush=%s activeSlot=%d selectedSlot=%d defaultProfile=%u mode=%d\n",
                  settings.autoPushEnabled ? "on" : "off", activeSlot, selectedSlot,
                  static_cast<unsigned>(defaultProfile), static_cast<int>(slot.mode));
    if (!settings.autoPushEnabled) {
        return;
    }
    self.display_.setProfileIndicatorSlot(selectedSlot);
    (void)self.autoPush_.queueSlotPush(selectedSlot, false, false);
}

void DriveRuntime::observeAlertTable(const AlertData* alerts, size_t count, uint8_t priorityIndex,
                                     uint32_t nowMs, void* context) {
    static_cast<DriveRuntime*>(context)->events_.observeV1Table(alerts, count, priorityIndex, nowMs);
}
