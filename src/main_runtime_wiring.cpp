#include "main_runtime_wiring.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <algorithm>

#include "battery_manager.h"
#include "audio_beep.h"
#include "ble_client.h"
#include "config.h"
#include "display.h"
#include "display_driver.h"
#include "display_mode.h"
#include "display_preview_api.h"
#include "main_globals.h"
#include "main_internals.h"
#include "main_loop_wiring.h"
#include "main_runtime_state.h"
#include "packet_parser.h"
#include "perf_metrics.h"
#include "perf_sd_logger.h"
#include "provider_callback_bindings.h"
#include "settings.h"
#include "settings_runtime_sync.h"
#include "status_observability_payload.h"
#include "storage_manager.h"
#include "touch_handler.h"
#include "v1_profiles.h"
#include "wifi_manager.h"
#include "modules/alp/alp_event_latch.h"
#include "modules/alp/alp_runtime_module.h"
#include "modules/alert_persistence/alert_persistence_module.h"
#include "modules/auto_push/auto_push_module.h"
#include "modules/ble/ble_queue_module.h"
#include "modules/ble/connection_runtime_module.h"
#include "modules/ble/connection_state_cadence_module.h"
#include "modules/ble/connection_state_dispatch_module.h"
#include "modules/ble/connection_state_module.h"
#include "modules/display/display_orchestration_module.h"
#include "modules/display/display_pipeline_module.h"
#include "modules/display/display_preview_module.h"
#include "modules/display/display_restore_module.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/obd/obd_ble_client.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/perf/debug_macros.h"
#include "modules/power/power_module.h"
#include "modules/qualification/qualification_serial_module.h"
#include "modules/quiet/quiet_coordinator_module.h"
#include "modules/speed/speed_source_selector.h"
#include "modules/speed_mute/speed_mute_module.h"
#include "modules/system/connection_cycle_coordinator_module.h"
#include "modules/system/loop_connection_early_module.h"
#include "modules/system/system_event_bus.h"
#include "modules/touch/touch_ui_module.h"
#include "modules/voice/voice_module.h"
#include "modules/volume_fade/volume_fade_module.h"
#include "modules/wifi/wifi_auto_start_module.h"
#include "modules/wifi/wifi_orchestrator_module.h"
#include "modules/wifi/wifi_priority_policy_module.h"
#include "modules/wifi/wifi_process_cadence_module.h"
#include "modules/wifi/wifi_runtime_module.h"
#include "modules/wifi/wifi_visual_sync_module.h"

namespace {
bool wifiStatusObservabilityCallbackConfigured = false;

bool restoreConnectionDisplayOwner(void* context, uint32_t nowMs) {
    auto* pipeline = static_cast<DisplayPipelineModule*>(context);
    if (!pipeline) {
        return false;
    }

    // A connection edge can be admitted during BLE ingest, after the early
    // display phase sampled the previous state. Refresh the full context here
    // immediately before the authoritative render so same-loop dispatch and
    // overload-skipped early phases cannot render or flush stale BLE status.
    const bool v1Connected = bleClient.isConnected();
    const bool proxyConnected = bleClient.isProxyClientConnected();
    display.setBleContext({v1Connected, proxyConnected, bleClient.getConnectionRssi(), bleClient.getProxyClientRssi()});
    const unsigned long lastRxMs = bleQueueModule.getLastRxMillis();
    const bool receiving = lastRxMs != 0 && (nowMs - lastRxMs) < ConnectionRuntimeModule::Config{}.receivingHeartbeatMs;
    display.setBLEProxyStatus(v1Connected, proxyConnected, receiving);
    return pipeline->restoreCurrentOwner(nowMs);
}
} // namespace

static void requestMaintenanceBootRestart() {
    if (!requestMaintenanceBoot()) {
        SerialLog.println("[MaintBoot] ERROR: failed to persist maintenance boot request");
        return;
    }
    SerialLog.println("[MaintBoot] rebooting into maintenance mode");
    settingsManager.save();
    markCleanShutdown();
    delay(50);
    ESP.restart();
}

void showInitialScanningScreen() {
    if (mainRuntimeState.initialScanningScreenShown) {
        return;
    }
    display.showScanning();
    display.drawProfileIndicator(settingsManager.get().activeSlot);
    mainRuntimeState.initialScanningScreenShown = true;
    connectionStateCadenceModule.onScanningScreenShown(millis());
}

static WifiOrchestrator& getWifiOrchestrator() {
    static WifiOrchestrator orchestrator(wifiManager, bleClient, parser, storageManager, autoPushModule);
    return orchestrator;
}

void configureWifiRuntimeModule() {
    wifiManager.setObdDependencies(&obdRuntimeModule, &speedSourceSelector);
    wifiManager.setAlpRuntime(&alpRuntimeModule);
    wifiManager.setGpsRuntime(&gpsRuntimeModule);
    getWifiOrchestrator().ensureCallbacksConfigured();
    if (!wifiStatusObservabilityCallbackConfigured) {
        wifiManager.appendStatusCallback(
            [](JsonObject obj, void* /*ctx*/) {
                obj["maintenanceBoot"] = mainRuntimeState.maintenanceBootActive;
                obj["maintenanceBootUptimeMs"] =
                    mainRuntimeState.maintenanceBootActive && mainRuntimeState.maintenanceBootStartedMs != 0
                        ? static_cast<uint32_t>(millis() - mainRuntimeState.maintenanceBootStartedMs)
                        : 0;
                obj["maintenanceBootTimeoutMs"] = MainRuntimePolicy::MaintenanceBootTimeoutMs;
                StatusObservabilityPayload::WifiStatusSnapshot wifiStatus;
                wifiStatus.apLastTransitionReasonCode = perfGetWifiApLastTransitionReason();
                wifiStatus.apLastTransitionReason =
                    perfWifiApTransitionReasonName(wifiStatus.apLastTransitionReasonCode);
                wifiStatus.lowDmaCooldownRemainingMs = wifiManager.lowDmaCooldownRemainingMs();
                wifiStatus.autoStart = wifiAutoStartModule.getLastDecision();

                StatusObservabilityPayload::appendStatusObservability(obj, wifiStatus);

                const QuietCommittedState quietCommitted = quietCoordinatorModule.getCommittedState();
                const QuietDesiredState& quietDesired = quietCoordinatorModule.getDesiredState();
                const QuietPresentationState& quietPresentation = quietCoordinatorModule.getPresentationState();

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
            },
            nullptr);
        wifiStatusObservabilityCallbackConfigured = true;
    }

    WifiRuntimeModule::Providers wifiRuntimeProviders;
    wifiRuntimeProviders.runWifiAutoStartProcess =
        [](void* ctx, uint32_t nowMs, uint32_t v1ConnectedAtMs, bool enableWifi, bool bleConnected, bool canStartDma,
           bool wifiAutoStartAllowed, bool& wifiManualStartIntentLatched, bool& wifiAutoStartDone) {
            static_cast<WifiAutoStartModule*>(ctx)->process(
                nowMs, v1ConnectedAtMs, enableWifi, bleConnected, canStartDma, wifiAutoStartAllowed,
                wifiManualStartIntentLatched, wifiAutoStartDone,
                [](bool autoStarted, void* /*ctx*/) { return wifiManager.startSetupMode(autoStarted); }, nullptr);
        };
    wifiRuntimeProviders.wifiAutoStartContext = &wifiAutoStartModule;
    wifiRuntimeProviders.shouldRunWifiProcessingPolicy = [](void* ctx) {
        return isWifiProcessingEnabledPolicy(*static_cast<WiFiManager*>(ctx));
    };
    wifiRuntimeProviders.wifiPolicyContext = &wifiManager;
    wifiRuntimeProviders.readWifiLifecyclePending =
        ProviderCallbackBindings::member<WiFiManager, &WiFiManager::hasPendingLifecycleWork>;
    wifiRuntimeProviders.wifiLifecycleContext = &wifiManager;
    wifiRuntimeProviders.perfTimestampUs = [](void*) -> uint32_t { return PERF_TIMESTAMP_US(); };
    wifiRuntimeProviders.runWifiCadence =
        ProviderCallbackBindings::member<WifiProcessCadenceModule, &WifiProcessCadenceModule::process>;
    wifiRuntimeProviders.wifiCadenceContext = &wifiProcessCadenceModule;
    wifiRuntimeProviders.setWifiTransitionAdmission = [](void* ctx, bool allowTransitionWork) {
        static_cast<WiFiManager*>(ctx)->setBoundaryTransitionAdmission(allowTransitionWork);
    };
    wifiRuntimeProviders.wifiTransitionAdmissionContext = &wifiManager;
    wifiRuntimeProviders.runWifiManagerProcess = ProviderCallbackBindings::member<WiFiManager, &WiFiManager::process>;
    wifiRuntimeProviders.wifiManagerProcessContext = &wifiManager;
    wifiRuntimeProviders.recordWifiProcessUs = [](void*, uint32_t elapsedUs) { perfRecordWifiProcessUs(elapsedUs); };
    wifiRuntimeProviders.readWifiServiceActive =
        ProviderCallbackBindings::member<WiFiManager, &WiFiManager::isWifiServiceActive>;
    wifiRuntimeProviders.wifiServiceContext = &wifiManager;
    wifiRuntimeProviders.readWifiConnected = ProviderCallbackBindings::member<WiFiManager, &WiFiManager::isConnected>;
    wifiRuntimeProviders.wifiConnectedContext = &wifiManager;
    wifiRuntimeProviders.readVisualNowMs = [](void*) -> uint32_t { return millis(); };
    wifiRuntimeProviders.runWifiVisualSync = [](void* ctx, uint32_t nowMs, bool wifiVisualActiveNow,
                                                bool displayPreviewRunning, bool bootSplashHoldActive) {
        static bool prevWifiVisualActive = false;
        static bool prevStaConnected = false;
        const bool stateChanged = (wifiVisualActiveNow != prevWifiVisualActive);
        prevWifiVisualActive = wifiVisualActiveNow;

        // Track STA connection independently so the icon color updates
        // immediately when STA connects, even if wifiVisualActiveNow
        // was already true (AP was running).
        const bool staConnected = wifiManager.isConnected();
        const bool staChanged = (staConnected != prevStaConnected);
        prevStaConnected = staConnected;

        static_cast<WifiVisualSyncModule*>(ctx)->process(
            nowMs, wifiVisualActiveNow, displayPreviewRunning, bootSplashHoldActive,
            [](void* /*ctx*/) {
                display.drawWiFiIndicator();
                const int leftColWidth = 64;
                const int leftColHeight = 96;
                display.flushRegion(0, SCREEN_HEIGHT - leftColHeight, leftColWidth, leftColHeight);
            },
            nullptr);

        // Force full DISPLAY_FLUSH on next pipeline run when WiFi icon
        // visibility or color transitions.  The small flushRegion above
        // handles periodic color refreshes, but the icon's initial
        // appearance requires a full flush to reliably reach the
        // AXS15231B panel.
        if (stateChanged || staChanged) {
            display.forceNextRedraw();
        }
    };
    wifiRuntimeProviders.wifiVisualSyncContext = &wifiVisualSyncModule;
    wifiRuntimeModule.begin(wifiRuntimeProviders);
}

static void configureLoopConnectionEarlyModule() {
    LoopConnectionEarlyModule::Providers loopConnectionEarlyProviders;
    loopConnectionEarlyProviders.runConnectionRuntime =
        ProviderCallbackBindings::member<ConnectionRuntimeModule, &ConnectionRuntimeModule::process>;
    loopConnectionEarlyProviders.connectionRuntimeContext = &connectionRuntimeModule;
    loopConnectionEarlyProviders.showInitialScanning = [](void*) { showInitialScanningScreen(); };
    loopConnectionEarlyProviders.readProxyConnected =
        ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::isProxyClientConnected>;
    loopConnectionEarlyProviders.proxyConnectedContext = &bleClient;
    loopConnectionEarlyProviders.readConnectionRssi =
        ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::getConnectionRssi>;
    loopConnectionEarlyProviders.connectionRssiContext = &bleClient;
    loopConnectionEarlyProviders.readProxyRssi =
        ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::getProxyClientRssi>;
    loopConnectionEarlyProviders.proxyRssiContext = &bleClient;
    loopConnectionEarlyProviders.runDisplayEarly =
        ProviderCallbackBindings::member<DisplayOrchestrationModule, &DisplayOrchestrationModule::processEarly>;
    loopConnectionEarlyProviders.displayEarlyContext = &displayOrchestrationModule;
    loopConnectionEarlyModule.begin(loopConnectionEarlyProviders);
}

void configureTouchUiModule() {
    getWifiOrchestrator().ensureCallbacksConfigured();

    TouchUiModule::Callbacks touchCbs{
        .isWifiSetupActive = [](void* /*ctx*/) { return wifiManager.isWifiServiceActive(); },
        .stopWifiSetup = [](void* /*ctx*/) { wifiManager.stopSetupMode(true); },
        .requestMaintenanceBoot = [](void* /*ctx*/) { requestMaintenanceBootRestart(); },
        .drawWifiIndicator = [](void* /*ctx*/) { display.drawWiFiIndicator(); },
        .restoreDisplay =
            [](void* /*ctx*/) {
                if (mainRuntimeState.bootSplashHoldActive) {
                    return;
                }
                displayPipelineModule.restoreCurrentOwner(millis());
            },
        .readObdStatus = [](uint32_t nowMs, void* /*ctx*/) { return obdRuntimeModule.snapshot(nowMs); },
        .requestObdManualPairScan = [](uint32_t nowMs,
                                       void* /*ctx*/) { return obdRuntimeModule.requestManualPairScan(nowMs); },
        .isObdPairGestureSafe = [](uint32_t nowMs,
                                   void* /*ctx*/) { return displayPipelineModule.allowsObdPairGesture(nowMs); }};
    touchUiModule.begin(&display, &touchHandler, &settingsManager, touchCbs);
}

void configureAlertDisplayPipeline() {
    // Initialize alert/audio/display pipeline dependencies before WiFi starts.
    alertPersistenceModule.begin(&bleClient, &parser, &display, &settingsManager);
    voiceModule.begin(&settingsManager, &bleClient);
    audio_set_volume(settingsManager.get().voiceVolume);
    volumeFadeModule.begin(&settingsManager);
    quietCoordinatorModule.begin(&bleClient, &parser);
    DisplayPipelineDependencies displayPipelineDeps;
    displayPipelineDeps.displayMode = &displayMode;
    displayPipelineDeps.display = &display;
    displayPipelineDeps.parser = &parser;
    displayPipelineDeps.settings = &settingsManager;
    displayPipelineDeps.ble = &bleClient;
    displayPipelineDeps.alertPersistence = &alertPersistenceModule;
    displayPipelineDeps.voice = &voiceModule;
    displayPipelineDeps.speedMute = &speedMuteModule;
    displayPipelineDeps.quiet = &quietCoordinatorModule;
    displayPipelineDeps.alp = &alpRuntimeModule;
    displayPipelineDeps.alpLatch = &alpEventLatch;
    displayPipelineDeps.speedSelector = &speedSourceSelector;
    displayPipelineModule.begin(displayPipelineDeps);
}

static void configureSystemLoopCoreModules() {
    systemEventBus.reset();
    if (!bleQueueModule.begin(&bleClient, &parser, &v1ProfileManager, &displayPreviewModule, &powerModule,
                              &systemEventBus)) {
        fatalBootError("BLE queue init failed", true);
    }
    configureConnectionRuntimeModule();
    connectionStateModule.begin(&bleClient, &parser, &display, &powerModule, &bleQueueModule, &alertPersistenceModule,
                                &systemEventBus);
    connectionStateModule.setDisplayOwnerRestoreCallback(restoreConnectionDisplayOwner, &displayPipelineModule);
    configureConnectionStateDispatchModule();
    configurePeriodicMaintenanceModule();
    configureLoopTailModule();
    configureLoopTelemetryModule();
    configureLoopIngestModule();
    displayRestoreModule.begin(&display, &parser, &bleClient, &displayPreviewModule, &displayPipelineModule);
    displayOrchestrationModule.begin(&display, &bleClient, &bleQueueModule, &displayPreviewModule,
                                     &displayRestoreModule, &parser, &settingsManager, &volumeFadeModule,
                                     &speedMuteModule, &quietCoordinatorModule);
}

static void configureSystemLoopPhaseModules() {
    configureLoopDisplayModule();
    configureLoopConnectionEarlyModule();
    configureLoopPowerTouchModule();
    configureLoopPreIngestModule();
    configureLoopSettingsPrepModule();
    configureLoopRuntimeSnapshotModule();
    configureLoopPostDisplayModule();
}

void configureSystemLoopModules() {
    configureSystemLoopCoreModules();
    configureSystemLoopPhaseModules();

    ConnectionCycleCoordinatorModule::Providers cycleProviders;
    cycleProviders.stopObdScan =
        ProviderCallbackBindings::memberDiscardReturn<ObdRuntimeModule, &ObdRuntimeModule::stopActiveScan>;
    cycleProviders.stopObdScanContext = &obdRuntimeModule;
    cycleProviders.cancelObdConnect =
        ProviderCallbackBindings::memberDiscardReturn<ObdRuntimeModule, &ObdRuntimeModule::cancelPendingConnect>;
    cycleProviders.cancelObdConnectContext = &obdRuntimeModule;
    cycleProviders.stopProxyAdvertising =
        ProviderCallbackBindings::memberDiscardReturn<V1BLEClient, &V1BLEClient::stopProxyAdvertising>;
    cycleProviders.stopProxyAdvertisingContext = &bleClient;
    cycleProviders.disconnectProxyPhone =
        ProviderCallbackBindings::memberDiscardReturn<V1BLEClient, &V1BLEClient::disconnectProxyPhones>;
    cycleProviders.disconnectProxyPhoneContext = &bleClient;
    cycleProviders.isObdScanStopped =
        ProviderCallbackBindings::member<ObdRuntimeModule, &ObdRuntimeModule::isScanStopped>;
    cycleProviders.isObdScanStoppedContext = &obdRuntimeModule;
    cycleProviders.isObdConnectIdle =
        ProviderCallbackBindings::member<ObdRuntimeModule, &ObdRuntimeModule::isConnectIdle>;
    cycleProviders.isObdConnectIdleContext = &obdRuntimeModule;
    cycleProviders.isProxyFullyStopped =
        ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::isProxyFullyStopped>;
    cycleProviders.isProxyFullyStoppedContext = &bleClient;
    connectionCycleCoordinatorModule.begin(cycleProviders);
}

namespace {
bool qualificationModeOverrideActive = false;
bool qualificationModeSavedProxyBle = false;
bool qualificationModeSavedObdEnabled = false;

void syncQualificationModeRuntime() {
    const V1Settings& settings = settingsManager.get();
    bleClient.setProxyRuntimeEnabled(settings.proxyBLE, settings.proxyName.c_str());
    SettingsRuntimeSync::syncObdVehicleRuntimeSettings(settings, obdRuntimeModule, speedSourceSelector);
    connectionCycleCoordinatorModule.reset();
}

bool applyQualificationModeOverride(uint8_t rawMode) {
    const V1Settings& current = settingsManager.get();
    if (!qualificationModeOverrideActive) {
        qualificationModeSavedProxyBle = current.proxyBLE;
        qualificationModeSavedObdEnabled = current.obdEnabled;
        qualificationModeOverrideActive = true;
    }

    bool proxyBle = current.proxyBLE;
    bool obdEnabled = current.obdEnabled;
    switch (static_cast<QualificationSerialModule::Mode>(rawMode)) {
    case QualificationSerialModule::Mode::Proxy:
        proxyBle = true;
        obdEnabled = false;
        break;
    case QualificationSerialModule::Mode::Obd:
        proxyBle = false;
        obdEnabled = true;
        break;
    case QualificationSerialModule::Mode::V1Only:
        proxyBle = false;
        obdEnabled = false;
        break;
    case QualificationSerialModule::Mode::Current:
        break;
    default:
        return false;
    }

    settingsManager.applyVolatileQualificationMode(proxyBle, obdEnabled);
    if (!proxyBle) {
        bleClient.stopProxyAdvertising();
        bleClient.disconnectProxyPhones();
    }
    if (!obdEnabled) {
        obdRuntimeModule.stopActiveScan();
        obdRuntimeModule.cancelPendingConnect();
    }
    syncQualificationModeRuntime();
    return true;
}

void clearQualificationModeOverride() {
    if (!qualificationModeOverrideActive) {
        return;
    }
    settingsManager.applyVolatileQualificationMode(qualificationModeSavedProxyBle, qualificationModeSavedObdEnabled);
    syncQualificationModeRuntime();
    qualificationModeOverrideActive = false;
}
} // namespace

static void configureRuntimeSensorModules() {
    speedSourceSelector.begin(&obdRuntimeModule, settingsManager.get().obdEnabled, &gpsRuntimeModule,
                              settingsManager.get().gpsEnabled);
    obdRuntimeModule.begin(&obdBleClient, settingsManager.get().obdEnabled,
                           settingsManager.get().obdSavedAddress.c_str(), settingsManager.get().obdSavedAddrType,
                           settingsManager.get().obdMinRssi);
    speedMuteModule.begin(settingsManager.get().speedMuteEnabled, settingsManager.get().speedMuteThresholdMph,
                          settingsManager.get().speedMuteHysteresisMph, settingsManager.get().speedMuteVolume,
                          settingsManager.get().speedMuteVoice);

    // ALP (Active Laser Protection) — UART2 listener for gun identification.
    // When enabled, ALP can also own laser alerting via V1 profile-push policy.
    alpRuntimeModule.begin(settingsManager.get().alpEnabled, &alpSdLogger);
    alpRuntimeModule.setEventBus(&systemEventBus);
}

static void configureRuntimeCoreModules() {
    configureRuntimeSensorModules();
}

static void configureQualificationSerialModule() {
    QualificationSerialModule::Providers providers;
    providers.isPerfEnabled = [](void*) { return perfSdLogger.isEnabled(); };
    providers.perfCsvPath = [](void*) { return perfSdLogger.csvPath(); };
    providers.startPerfSession = [](void*) { perfSdLogger.startNewSession(); };
    providers.enqueueSnapshotNow = [](void*) { return perfMetricsEnqueueSnapshotNow(); };
    providers.tryDrainPerf = [](void*) { return perfSdLogger.tryDrainAndClose(); };
    providers.setSdCapturePaused = [](bool paused, void*) { perfMetricsSetSdCapturePaused(paused); };
    providers.startDisplayPreview = [](uint32_t durationMs, void*) { requestColorPreviewHold(durationMs); };
    providers.cancelDisplayPreview = [](void*) { cancelDisplayPreview(); };
    providers.applyQualificationMode = [](uint8_t mode, void*) { return applyQualificationModeOverride(mode); };
    providers.clearQualificationMode = [](void*) { clearQualificationModeOverride(); };
    providers.isStorageReady = [](void*) { return storageManager.isReady(); };
    providers.isSDCard = [](void*) { return storageManager.isSDCard(); };
    providers.filesystem = [](void*) { return storageManager.getFilesystem(); };
    providers.sdMutex = [](void*) { return storageManager.getSDMutex(); };
    providers.tryProxyEpochSnapshot = [](BleProxyEpochQualificationSnapshot& snapshot, void*) {
        return bleClient.trySnapshotProxyEpochQualification(snapshot);
    };
    providers.nowMs = [](void*) { return millis(); };
    qualificationSerialModule.begin(&Serial, providers);
}

void configureRuntimeModules() {
    configureRuntimeCoreModules();
    configureQualificationSerialModule();
}
