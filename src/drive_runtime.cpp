#include "drive_runtime.h"

#include <algorithm>
#include <cstring>
#include <NimBLEDevice.h>

#include "audio_beep.h"
#include "battery_manager.h"
#include "build_metadata.h"
#include "config.h"
#include "main_internals.h"
#include "modules/event_log/product_event_log.h"
#include "modules/health/health_journal.h"
#include "modules/system/parsed_frame_event_module.h"
#include "settings.h"
#include "storage_manager.h"
#include "v1_devices.h"
#include "v1_profiles.h"

namespace {
constexpr uint32_t kConnectionStateProcessMaxGapMs = 1000;
constexpr uint32_t kConnectedPersistenceDeferralMs = 10000;

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
        profiles_.begin(storage_.getFilesystem(), storage_.getLittleFS());
        devices_.begin(storage_.getFilesystem(), storage_.getLittleFS());
        audio_init_buffers();
        audio_init_sd(storage_);

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

void DriveRuntime::requestMaintenanceBootRestart() {
    if (!requestMaintenanceBoot()) {
        Serial.println("[MaintBoot] ERROR: failed to persist maintenance boot request");
        return;
    }
    Serial.println("[MaintBoot] rebooting into maintenance mode");
    const bool persistenceSafe = completeLoggingForControlledRestart(events_, health_);
    if (persistenceSafe) {
        settings_.save();
        markCleanShutdown();
    } else {
        Serial.println("[MaintBoot] WARN: restart continuing without final persistence writes");
    }
    delay(50);
    ESP.restart();
}

void DriveRuntime::initializeTouchAndUi() {
    autoPush_.begin(&settings_, &profiles_, &ble_, &display_, &quiet_);

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

    TapGestureModule::WifiCallbacks tapCallbacks{};
    tapCallbacks.isWifiActive = [](void*) { return false; };
    tapCallbacks.stopWifi = [](void*) {};
    tapCallbacks.requestMaintenanceBoot = [](void* context) {
        static_cast<DriveRuntime*>(context)->requestMaintenanceBootRestart();
    };
    tapCallbacks.requestMaintenanceBootCtx = this;
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
    displayRestore_.begin(&display_, &parser_, &ble_, &preview_, &displayPipeline_);
    displayOrchestration_.begin(&display_, &ble_, &bleQueue_, &preview_, &displayRestore_, &parser_, &settings_,
                                &volumeFade_, &speedMute_, &quiet_, &displayPipeline_);
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

    if (!settings_.get().enableWifi) {
        Serial.println("[WiFi] Master disabled in settings — startup and loop processing skipped");
    } else {
        Serial.println("Setup complete - BLE scanning, WiFi off; BOOT long-press requests maintenance reboot");
    }
    logBootStage("wifi", setupStartMs, stageStartedMs);
    Serial.printf("[Boot] setup total: %lu ms\n", static_cast<unsigned long>(millis() - setupStartMs));
}

void DriveRuntime::start(uint32_t setupStartMs, uint32_t stageStartedMs, esp_reset_reason_t resetReason) {
    if (active_) {
        return;
    }
    state_.maintenanceBootActive = false;
    initializeStorageAndProfiles();

    const bool previousShutdownClean = readAndResetCleanShutdownMarker();
    Serial.println(previousShutdownClean ? "[Boot] Previous shutdown was clean"
                                         : "[Boot] Previous shutdown was UNCLEAN (no clean-shutdown marker)");
    const uint32_t bootId = nextBootId();
    HealthCounters::reset();
    (void)health_.begin(storage_, bootId, getRuntimeImageId(), resetReasonToString(resetReason),
                        previousShutdownClean, preservedPanicEvidencePresent(resetReason));
    (void)events_.begin(bootId, storage_);
    logBootStage("storage", setupStartMs, stageStartedMs);

    power_.setLifecycle(*this);
    initializeBle(setupStartMs, stageStartedMs);
    initializeTouchAndUi();
    logBootStage("ui_modules", setupStartMs, stageStartedMs);
    logBootIdentity(bootId, resetReason, settings_);
    Serial.println(settings_.get().enableWifi ? "[WiFi] Off in normal runtime - BOOT long-press reboots to maintenance"
                                              : "[WiFi] Master disabled - startup and loop processing skipped");

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

void DriveRuntime::presentConnectionState(uint32_t nowMs, const ConnectionRuntimeSnapshot& connection) {
    DisplayOrchestrationEarlyContext earlyContext;
    earlyContext.nowMs = nowMs;
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
    return touchUi_.process(nowMs, digitalRead(BOOT_BUTTON_GPIO) == LOW);
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
    AlertData v1Priority;
    const bool v1LiveAlert = parser_.hasAlerts() && parser_.getRenderablePriorityAlert(v1Priority);
    const bool alpLiveAlert = alp_.ownsLaserDisplay() && alp_.currentEvent().active;
    return (v1LiveAlert || alpLiveAlert) && touchUi_.preemptForLiveAlert();
}

void DriveRuntime::processTapGesture(uint32_t nowMs) {
    tapGesture_.process(nowMs);
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
    const CycleContext cycleContext{
        nowMs,
        state_.bootReady,
        bleConnectedNow,
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

DriveRuntime::DisplayEdges DriveRuntime::consumeDisplayEdges() {
    DisplayEdges edges;
    edges.nowMs = millis();
    edges.parsed = ParsedFrameEventModule::collect(bleQueue_.consumeParsedFlag(), systemEvents_);
    return edges;
}

void DriveRuntime::presentDisplay(const DisplayEdges& edges, bool overloadThisLoop) {
    DisplayOrchestrationParsedContext parsedContext;
    parsedContext.nowMs = edges.nowMs;
    parsedContext.parsedReady = edges.parsed.parsedReady;
    parsedContext.bootSplashHoldActive = state_.bootSplashHoldActive;
    const DisplayOrchestrationParsedResult parsedResult = displayOrchestration_.processParsedFrame(parsedContext);
    bool pipelineRan = false;
    if (parsedResult.runDisplayPipeline) {
        displayPipeline_.handleParsed(edges.nowMs);
        pipelineRan = true;
    }

    DisplayOrchestrationRefreshContext refreshContext;
    refreshContext.nowMs = edges.nowMs;
    refreshContext.bootSplashHoldActive = state_.bootSplashHoldActive;
    refreshContext.overloadLateThisLoop = overloadThisLoop;
    refreshContext.pipelineRanThisLoop = pipelineRan;
    const DisplayOrchestrationRefreshResult refresh = displayOrchestration_.processLightweightRefresh(refreshContext);
    if (refresh.runBlinkRefresh) {
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

void DriveRuntime::processPeriodicMaintenance(uint32_t nowMs, bool bleConnected, bool bleBackpressure,
                                              bool loopOverloaded, bool forceTailBleDrainPending) {
    const bool hardPressure = bleBackpressure || loopOverloaded || forceTailBleDrainPending;
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
    resumePersistenceAfterAbortedShutdown(events_);
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
                            uint32_t sessionGeneration, uint32_t callbackMillis) {
    if (callbackOwner_) {
        callbackOwner_->bleQueue_.onNotify(data, length, charUuid, sessionGeneration, callbackMillis);
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

void DriveRuntime::onV1Connected() {
    if (!callbackOwner_) {
        return;
    }
    auto& self = *callbackOwner_;
    const V1Settings& settings = self.settings_.get();
    const int activeSlot = std::max(0, std::min(2, settings.activeSlot));
    int selectedSlot = activeSlot;
    uint8_t defaultProfile = 0;
    bool addressFromLink = false;
    String address;
    NimBLEAddress connected = self.ble_.getConnectedAddress();
    if (!connected.isNull()) {
        address = normalizeV1DeviceAddress(String(connected.toString().c_str()));
        addressFromLink = address.length() > 0;
    }
    if (address.length() == 0) {
        address = normalizeV1DeviceAddress(settings.lastV1Address);
    }
    if (address.length() > 0 && self.devices_.isReady()) {
        self.devices_.touchDeviceInMemory(address);
        defaultProfile = self.devices_.getDeviceDefaultProfile(address);
        if (defaultProfile >= 1 && defaultProfile <= 3) {
            selectedSlot = static_cast<int>(defaultProfile) - 1;
        }
    }
    if (addressFromLink) {
        self.settings_.setLastV1Address(address);
        if (!self.devices_.isReady()) {
            self.settings_.requestLastV1AddressFallbackPersist(address);
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
