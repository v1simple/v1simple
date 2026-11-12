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
#include "modules/quiet/quiet_coordinator_templates.h"
#include "persistent_bootstrap.h"
#include "modules/voice/runtime_voice_observation.h"
#include "settings.h"
#include "storage_manager.h"
#include "v1_devices.h"
#include "v1_firmware_compat.h"
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

DriveRuntime::DriveRuntime(SettingsManager& settings, RuntimePresentationSource& presentation, V1ProfileManager& profiles, V1DeviceStore& devices,
                           StorageManager& storage, BatteryManager& battery, ProductEventLog& events,
                           HealthJournal& health)
    : settings_(settings), presentation_(presentation), profiles_(profiles), devices_(devices), storage_(storage), battery_(battery),
      events_(events), health_(health),
      settingsOperationCoordinator_(settingsOperations_, autoPush_, ble_, makeSettingsOperationEffects()) {}

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

SettingsOperationCoordinator::Effects DriveRuntime::makeSettingsOperationEffects() {
    return {
        this,
        [](String& address, void* context) {
            return static_cast<DriveRuntime*>(context)->connectedV1Address(address);
        },
        [](uint32_t boundary, void* context) {
            return static_cast<DriveRuntime*>(context)->captureDetectorSnapshot(
                SnapshotPurpose::SettingsOperation, boundary);
        },
        [](const String& address, const V1DetectorSnapshot& snapshot, bool flush, void* context) {
            return static_cast<DriveRuntime*>(context)->persistDetectorSnapshot(address, snapshot, flush);
        },
        [](void* context) { return static_cast<DriveRuntime*>(context)->settings_.get().activeSlot; },
        [](int slot, void* context) {
            auto& self = *static_cast<DriveRuntime*>(context);
            self.displayMode_ = DisplayMode::IDLE;
            self.alertPersistence_.clearPersistence();
            self.presentProfileIndicator(slot);
        },
        [](void* context) { return static_cast<DriveRuntime*>(context)->requestMaintenanceBootRestart(); },
    };
}

void DriveRuntime::initializeStorageAndProfiles() {
    const PersistentBootstrapEffects effects{
        this,
        [](void* context) {
            auto& self = *static_cast<DriveRuntime*>(context);
            audio_init_buffers();
            audio_init_sd(self.storage_);
        },
        [](void* context) {
            auto& self = *static_cast<DriveRuntime*>(context);
            self.display_.updateColorTheme(makeDisplayStyleView(self.settings_));
            self.display_.setBrightness(self.settings_.get().brightness);
        },
    };
    PersistentBootstrap::run(storage_, settings_, profiles_, devices_, effects);

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
        fatalBootError(display_, presentation_, "BLE pre-init failed", true);
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
        fatalBootError(display_, presentation_, "BLE scan failed", true);
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
    if (!settingsOperationCoordinator_.acknowledgePendingMaintenanceReturn()) {
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
    autoPush_.begin(&settings_, &profiles_, &ble_, &parser_,
                    [](int slot, void* context) { static_cast<DriveRuntime*>(context)->presentProfileIndicator(slot); },
                    this, &quiet_);

    TouchUiModule::Callbacks touchCallbacks{};
    touchCallbacks.requestMaintenanceBoot = [](void* context) {
        static_cast<DriveRuntime*>(context)->requestMaintenanceBootRestart();
    };
    touchCallbacks.requestMaintenanceBootCtx = this;
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
    touchUi_.begin(&display_, &touch_, &settings_, touchCallbacks, presentation_);

    TapGestureModule::Callbacks tapCallbacks{};
    tapCallbacks.beginProfileCycle = [](int newSlot, void* context) {
        return static_cast<DriveRuntime*>(context)->beginTripleTapProfileCycle(newSlot);
    };
    tapCallbacks.beginProfileCycleContext = this;
    tapCallbacks.presentProfile = [](int slot, void* context) {
        static_cast<DriveRuntime*>(context)->presentProfileIndicator(slot);
    };
    tapCallbacks.presentProfileContext = this;
    tapGesture_.begin(&touch_, &settings_, &ble_, &parser_, &autoPush_, &alertPersistence_,
                      &displayMode_, &quiet_, tapCallbacks);
}

bool DriveRuntime::restoreConnectionDisplayOwner(void* context, uint32_t nowMs) {
    auto& self = *static_cast<DriveRuntime*>(context);
    const bool v1Connected = self.ble_.isConnected();
    const bool proxyConnected = self.ble_.isProxyClientConnected();
    const DisplayBleContext bleContext{v1Connected, proxyConnected, self.ble_.getConnectionRssi(), self.ble_.getProxyClientRssi()};
    const uint32_t sampledAtMs = static_cast<uint32_t>(millis());
    const uint32_t lastRxMs = self.bleQueue_.getLastRxMillis();
    const bool receiving =
        lastRxMs != 0 && (nowMs - lastRxMs) < ConnectionRuntimeModule::Config{}.receivingHeartbeatMs;
    self.presentation_.noteBleSample(bleContext, receiving, sampledAtMs);
    const auto inputs = self.presentation_.captureForPaint();
    self.display_.refreshBleIndicator(inputs.context());
    return self.displayPipeline_.restoreCurrentOwner(nowMs);
}

void DriveRuntime::initializeRuntimeModules() {
    voice_.begin(&settings_);
    audio_set_volume(settings_.get().voiceVolume);
    volumeFade_.begin(&settings_);
    quiet_.begin(&ble_, &parser_);
    voicePlayback_.begin(parser_.alertLifetime());
    autoPush_.setInTheBoxApplyCallback(persistInTheBoxApply, this);

    DisplayPipelineDependencies pipelineDependencies;
    pipelineDependencies.displayMode = &displayMode_;
    pipelineDependencies.display = &display_;
    pipelineDependencies.presentation = &presentation_;
    pipelineDependencies.parser = &parser_;
    pipelineDependencies.settings = &settings_;
    pipelineDependencies.ble = &ble_;
    pipelineDependencies.alertPersistence = &alertPersistence_;
    pipelineDependencies.speedMute = &speedMute_;
    pipelineDependencies.quiet = &quiet_;
    pipelineDependencies.alp = &alp_;
    pipelineDependencies.alpLatch = &alpEventLatch_;
    pipelineDependencies.speedSelector = &speed_;
    displayPipeline_.begin(pipelineDependencies);

    systemEvents_.reset();
    if (!bleQueue_.begin(&ble_, &parser_, &preview_, &power_)) {
        fatalBootError(display_, presentation_, "BLE queue init failed", true);
    }
    connectionRuntime_.begin(ble_, bleQueue_);
    connectionState_.begin(&ble_, &parser_, &display_, &power_, &bleQueue_, &alertPersistence_, presentation_);
    connectionState_.setDisplayOwnerRestoreCallback(restoreConnectionDisplayOwner, this);
    connectionDispatch_.begin(connectionCadence_, connectionState_);
    obdSettingsSync_.begin(&settings_, &obd_);
    displayRestore_.begin(preview_, displayPipeline_);
    displayOrchestration_.begin(&display_, &ble_, &preview_, &displayRestore_, &parser_,
                                &volumeFade_, &speedMute_, &quiet_, presentation_);
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
    observedSettingsRecoveryRevision_ = settings_.recoveryRevision();
}

void DriveRuntime::syncRecoveredSettings() {
    const uint32_t revision = settings_.recoveryRevision();
    if (revision == observedSettingsRecoveryRevision_) return;
    const V1Settings& recovered = settings_.get();
    // These owners were initialized from provisional defaults when NVS was
    // unavailable. Adopt the recovered configuration before runtime-to-settings
    // synchronization can mistake an empty OBD address for a user change.
    gps_.setEnabled(false);
    gps_.begin(recovered.gpsEnabled, recovered.gpsBaud);
    obd_.begin(&obdBle_, recovered.obdEnabled, recovered.obdSavedAddress.c_str(),
               recovered.obdSavedAddrType, recovered.obdMinRssi);
    obdSettingsSync_.begin(&settings_, &obd_);
    speed_.syncEnabledInputs(recovered.obdEnabled, recovered.gpsEnabled);
    alp_.begin(recovered.alpEnabled);
    const bool applyBrightness = !power_.ownsDisplayPresentation();
    const bool slidersActive = touchUi_.syncRecoveredSliderSettings(applyBrightness);
    if (!slidersActive) audio_set_volume(recovered.voiceVolume);
    display_.updateColorTheme(makeDisplayStyleView(settings_));
    display_.forceNextRedraw();
    if (applyBrightness && !slidersActive) display_.setBrightness(recovered.brightness);
    ble_.setProxyRuntimeEnabled(recovered.proxyBLE, recovered.proxyName.c_str());
    connectionCycle_.requestRecoveredSettingsAdmission();
    observedSettingsRecoveryRevision_ = revision;
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
    settingsOperationCoordinator_.beginNormalBoot();
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
    {
        const auto inputs = presentation_.captureForPaint();
        display_.showScanning(inputs.context());
    }
    presentProfileIndicator(settings_.get().activeSlot);
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

void DriveRuntime::processPower(uint32_t /*nowMs*/) {
    power_.process();
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
    const bool slidersRestored = touchUi_.restorePresentationIfOwned();
    if (!slidersRestored && !displayPipeline_.restoreCurrentOwner(nowMs)) {
        {
            const auto inputs = presentation_.captureForPaint();
            display_.showScanning(inputs.context());
        }
        presentProfileIndicator(settings_.get().activeSlot);
    }
    if (restoreBrightness && !slidersRestored) {
        const uint8_t brightness = settings_.localSliderBrightness();
        display_.setBrightness(brightness);
        Serial.printf("[Power] Restored display brightness=%u after shutdown abort\n",
                      static_cast<unsigned>(brightness));
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
    // A completed recovery write still owns the command slot until polled.
    // Sliders, preview and power warnings suppress connection rendering only.
    connectionState_.servicePendingAlertRecovery(millis());
    // Observe phone takeover even when sliders/preview suppress parsed frames.
    quiet_.syncProxyVolumeOwnership(&volumeFade_);
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

bool DriveRuntime::processParsedPolicies(const DisplayEdges& edges) {
    quiet_.setInTheBoxSuspended(autoPush_.isActive() ||
                                settingsOperations_.requiresExclusiveDetectorAdmission());
    DisplayOrchestrationParsedContext parsedContext;
    parsedContext.nowMs = edges.nowMs;
    parsedContext.parsedReady = edges.parsedReady;
    parsedContext.bootSplashHoldActive = state_.bootSplashHoldActive;
    const bool present = displayOrchestration_.processParsedFrame(parsedContext);
    voicePlayback_.servicePendingCancellation();
    return present;
}

void DriveRuntime::admitPreparedVoice(const RenderFrame& frame, uint32_t nowMs) {
    const V1Settings& settings = settings_.get();
    const VoiceSettings voiceSettings{settings.voiceAlertMode, settings.voiceDirectionEnabled,
                                       settings.announceBogeyCount};
    voicePlayback_.onParsedPresentation(
        makeV1VoiceObservation(frame, parser_.alertLifetime(), ble_.isProxyClientConnected()),
        voiceSettings, nowMs);
}

bool DriveRuntime::presentParsedDisplay(uint32_t nowMs) {
    RenderFrame frame;
    if (!displayPipeline_.prepareParsed(nowMs, frame)) {
        return false;
    }
    admitPreparedVoice(frame, nowMs);
    displayPipeline_.presentPrepared(frame, millis());
    return true;
}

void DriveRuntime::presentDisplay(const DisplayEdges& edges, bool overloadThisLoop) {
    const bool pipelineRan = processParsedPolicies(edges) && presentParsedDisplay(edges.nowMs);

    DisplayOrchestrationRefreshContext refreshContext;
    refreshContext.nowMs = edges.nowMs;
    refreshContext.bootSplashHoldActive = state_.bootSplashHoldActive;
    refreshContext.overloadLateThisLoop = overloadThisLoop;
    refreshContext.pipelineRanThisLoop = pipelineRan;
    refreshContext.v1PersistenceRefreshDue = displayPipeline_.v1PersistenceRefreshDue(edges.nowMs);
    if (displayOrchestration_.processLightweightRefresh(refreshContext)) {
        displayPipeline_.refreshBlinkTick(edges.nowMs);
    }
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
    syncRecoveredSettings();
    settingsOperationCoordinator_.process(nowMs);
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

    if (admitPersistence) {
        profiles_.serviceRecovery(nowMs);
        if (!devices_.catalogReadable() && storage_.isSDCard()) {
            StorageManager::SDTryLock lock(storage_.getSDMutex(), /*checkDmaHeap=*/true);
            if (lock) devices_.serviceRecovery(nowMs);
        } else {
            devices_.serviceRecovery(nowMs);
        }
        settings_.serviceDeferredPersist(nowMs);
        settings_.serviceDeferredBackup(nowMs);
    }
    syncRecoveredSettings();
    // A provisional runtime address is not a user edit. Its setter must not
    // become the caller that finishes recovery and then overwrites the pairing
    // with a snapshot captured before the runtime adopted recovered settings.
    if (!settings_.recoveryPending()) obdSettingsSync_.process(nowMs);
    if (!hardPressure) serviceDeferredConnectionAutoPush();
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
    // Tail parsing and non-display owners can change suppression without a
    // presentation edge. Recheck before yielding to the audio worker, even
    // when sliders or a warning own the screen. Never admit new speech here.
    if (voicePlayback_.hasPendingPlayback()) {
        if (!state_.bootSplashHoldActive) {
            quiet_.setInTheBoxSuspended(autoPush_.isActive() ||
                                        settingsOperations_.requiresExclusiveDetectorAdmission());
            quiet_.processInTheBox(millis());
        }
        voicePlayback_.servicePendingCancellation();
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
    // Wait for competing writers to release storage before the final snapshot
    // write. Physical shutdown remains non-vetoing; failures withhold only the
    // clean marker and leave pending state available if the hardware tail aborts.
    if (!preparePersistenceForShutdown(events_, health_, settings_)) return false;
    if (!devices_.hasPendingSave()) return true;
    bool devicesCommitted = false;
    if (storage_.isSDCard()) {
        StorageManager::SDTryLock lock(storage_.getSDMutex(), /*checkDmaHeap=*/false);
        devicesCommitted = lock && devices_.flushPendingSave();
    } else {
        devicesCommitted = devices_.flushPendingSave();
    }
    if (!devicesCommitted) {
        Serial.println("[Battery] ERROR: Final detector snapshot commit failed; shutdown remains unclean");
    }
    return devicesCommitted;
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
        auto& self = *callbackOwner_;
        String address;
        const bool known = self.connectedV1Address(address);
        self.quiet_.beginDetectorVolumeSession(known ? address.c_str() : nullptr, &self.volumeFade_);
        self.connectionState_.handleSessionOpened(sessionGeneration);
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

V1DetectorSnapshot DriveRuntime::captureDetectorSnapshot(SnapshotPurpose purpose,
                                                        uint32_t ingressBoundary) const {
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
            const uint32_t ingress = definitions.ingressSequences[index];
            if (purpose == SnapshotPurpose::StableConnection) {
                // Follow-up rows must belong to their specific read request.
                // Resolve its live transport boundary for each admitted row.
                const uint32_t boundary = ble_.sessionSweepDefinitionsIngressBoundary();
                snapshot.hasSweepDefinitions = ingress != 0 && boundary != 0 &&
                                               static_cast<int32_t>(ingress - boundary) > 0;
            } else {
                snapshot.hasSweepDefinitions = afterBoundary(ingress);
            }
        }
        if (snapshot.hasSweepDefinitions) snapshot.sweepDefinitions = definitions.definitions;
    }
    return snapshot;
}

SettingsOperationCoordinator::SnapshotPersistResult DriveRuntime::persistDetectorSnapshot(const String& address,
                                           const V1DetectorSnapshot& snapshot,
                                           bool flushImmediately) {
    using Result = SettingsOperationCoordinator::SnapshotPersistResult;
    if (!devices_.recordSnapshotInMemory(address, snapshot)) return Result::StoreUnavailable;
    if (!flushImmediately) return Result::Ok;
    if (storage_.isSDCard()) {
        StorageManager::SDTryLock lock(storage_.getSDMutex(), /*checkDmaHeap=*/false);
        return lock && devices_.flushPendingSave() ? Result::Ok : Result::FlushFailed;
    }
    return devices_.flushPendingSave() ? Result::Ok : Result::FlushFailed;
}

bool DriveRuntime::beginTripleTapProfileCycle(int newSlot) {
    if (newSlot < 0 || newSlot > 2) return false;
    const V1Settings& current = settings_.get();
    if (!ble_.isConnected() || !current.autoPushEnabled) {
        if (!settings_.setActiveSlot(newSlot,
                SettingsPersistMode::ImmediateNvsDeferredBackup).success) return false;
        displayMode_ = DisplayMode::IDLE;
        alertPersistence_.clearPersistence();
        presentProfileIndicator(newSlot);
        Serial.printf("PROFILE CHANGE: local slot %d (no detector apply)\n", newSlot);
        return true;
    }
    return settingsOperationCoordinator_.beginSlotApply(newSlot);
}

bool DriveRuntime::persistInTheBoxApply(const V1InTheBoxSettings& settings, void* context) {
    auto& self = *static_cast<DriveRuntime*>(context);
    String address;
    if (!self.connectedV1Address(address)) return false;
    if (self.storage_.isSDCard()) {
        StorageManager::SDTryLock lock(self.storage_.getSDMutex(), /*checkDmaHeap=*/false);
        return lock && self.devices_.setDeviceInTheBox(address, settings).committed();
    }
    return self.devices_.setDeviceInTheBox(address, settings).committed();
}

void DriveRuntime::onV1Connected() {
    if (!callbackOwner_) {
        return;
    }
    auto& self = *callbackOwner_;
    String appSettingsAddress;
    V1InTheBoxSettings appSettings;
    if (self.connectedV1Address(appSettingsAddress)) {
        // Failure or an unknown detector leaves automatic audio actions off.
        (void)self.devices_.getInTheBoxForAddressChecked(appSettingsAddress, appSettings);
    }
    self.quiet_.setInTheBoxSettings(appSettings, self.ble_.sessionGeneration());
    self.pendingConnectionAutoPushSession_ = 0;
    if (self.settingsOperationCoordinator_.onStableConnection() !=
        SettingsOperationCoordinator::StableAdmission::AllowOrdinaryAutoPush) return;
    self.processStableDetector(false);
}

void DriveRuntime::serviceDeferredConnectionAutoPush() {
    if (pendingConnectionAutoPushSession_ == 0) return;
    if (!ble_.isConnected() || pendingConnectionAutoPushSession_ != ble_.sessionGeneration()) {
        pendingConnectionAutoPushSession_ = 0;
        return;
    }
    if (settings_.recoveryPending()) return;
    pendingConnectionAutoPushSession_ = 0;
    if (settingsOperationCoordinator_.allowsOrdinaryAutoPush()) {
        processStableDetector(true);
    }
}

void DriveRuntime::processStableDetector(bool requireLiveAddress) {
    const bool settingsReady = !settings_.recoveryPending();
    uint8_t defaultProfile = 0;
    String linkAddress;
    NimBLEAddress connected = ble_.getConnectedAddress();
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
        // The saved fallback has no authority until recovery finishes. A
        // deferred write also needs an identified live detector session.
        if (!settingsReady || requireLiveAddress) return;
        const V1Settings& settings = settings_.get();
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
    if (!settingsReady) {
        pendingConnectionAutoPushSession_ = ble_.sessionGeneration();
    }
    if (profileAddress->length() > 0) {
        const V1DeviceDefaultProfileResult lookup =
            devices_.getDeviceDefaultProfileChecked(*profileAddress);
        if (lookup.status == V1DeviceDefaultProfileStatus::Unavailable ||
            !devices_.touchDeviceInMemory(*profileAddress)) {
            Serial.println("[AutoPush] NOT_QUEUED reason=device_catalog_unavailable");
            return;
        }
        if (lookup.status == V1DeviceDefaultProfileStatus::Found) {
            defaultProfile = lookup.profile;
        }
    } else if (settings_.get().autoPushEnabled) {
        Serial.println("[AutoPush] NOT_QUEUED reason=device_identity_unavailable");
        return;
    }
    if (settingsReady && linkAddress.length() > 0) {
        settings_.setLastV1Address(linkAddress);
        if (!devices_.isReady()) {
            settings_.requestLastV1AddressFallbackPersist(linkAddress);
        }
    }

    // The stable callback runs only after the bounded connect-followup read.
    // Copy the detector observation into the address-keyed store before
    // Auto-Push can write any desired profile state back to the V1.
    const V1DetectorSnapshot snapshot = captureDetectorSnapshot(SnapshotPurpose::StableConnection);
    autoPush_.setPreApplySnapshot(snapshot);

    if (linkAddress.length() > 0 && devices_.isReady()) {
        if (!devices_.recordSnapshotInMemory(linkAddress, snapshot)) {
            Serial.println("[AutoPush] NOT_QUEUED reason=device_snapshot_unavailable");
            return;
        }
    }

    if (!settingsReady) {
        Serial.println("[AutoPush] DEFERRED reason=settings_recovery_pending");
        return;
    }
    const V1Settings& settings = settings_.get();
    const int activeSlot = std::max(0, std::min(2, settings.activeSlot));
    const int selectedSlot = defaultProfile == 0 ? activeSlot : static_cast<int>(defaultProfile) - 1;
    const AutoPushSlot& slot = settings_.getSlot(selectedSlot);
    Serial.printf("[AutoPush] onV1Connected autoPush=%s activeSlot=%d selectedSlot=%d defaultProfile=%u mode=%d\n",
                  settings.autoPushEnabled ? "on" : "off", activeSlot, selectedSlot,
                  static_cast<unsigned>(defaultProfile), static_cast<int>(slot.mode));
    if (!settings.autoPushEnabled) {
        return;
    }
    display_.setProfileIndicatorSlot(selectedSlot, static_cast<uint32_t>(millis()));
    (void)autoPush_.queueSlotPush(selectedSlot, false, false);
}

void DriveRuntime::observeAlertTable(const AlertData* alerts, size_t count, uint8_t priorityIndex,
                                     uint32_t nowMs, void* context) {
    static_cast<DriveRuntime*>(context)->events_.observeV1Table(alerts, count, priorityIndex, nowMs);
}

void DriveRuntime::presentProfileIndicator(int slot) {
    display_.setProfileIndicatorSlot(slot, static_cast<uint32_t>(millis()));
    const auto inputs = presentation_.captureForPaint();
    display_.drawProfileIndicator(inputs.context());
}
