#include "main_runtime_wiring.h"

#include <Arduino.h>
#include <algorithm>

#include "battery_manager.h"
#include "audio_beep.h"
#include "ble_client.h"
#include "config.h"
#include "display.h"
#include "display_driver.h"
#include "display_mode.h"
#include "main_globals.h"
#include "main_internals.h"
#include "main_loop_wiring.h"
#include "main_runtime_state.h"
#include "packet_parser.h"
#include "provider_callback_bindings.h"
#include "settings.h"
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
#include "modules/event_log/product_event_log.h"
#include "modules/health/health_journal.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/obd/obd_ble_client.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/power/power_module.h"
#include "modules/quiet/quiet_coordinator_module.h"
#include "modules/speed/speed_source_selector.h"
#include "modules/speed_mute/speed_mute_module.h"
#include "modules/system/connection_cycle_coordinator_module.h"
#include "modules/system/loop_connection_early_module.h"
#include "modules/system/system_event_bus.h"
#include "modules/touch/touch_ui_module.h"
#include "modules/voice/voice_module.h"
#include "modules/volume_fade/volume_fade_module.h"

namespace {
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
        Serial.println("[MaintBoot] ERROR: failed to persist maintenance boot request");
        return;
    }
    Serial.println("[MaintBoot] rebooting into maintenance mode");
    const bool persistenceSafe = completeLoggingForControlledRestart();
    if (persistenceSafe) {
        settingsManager.save();
        markCleanShutdown();
    } else {
        Serial.println("[MaintBoot] WARN: restart continuing without final persistence writes");
    }
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
    if (!bleQueueModule.begin(&bleClient, &parser, &v1ProfileManager, &displayPreviewModule, &powerModule)) {
        fatalBootError("BLE queue init failed", true);
    }
    configureConnectionRuntimeModule();
    connectionStateModule.begin(&bleClient, &parser, &display, &powerModule, &bleQueueModule, &alertPersistenceModule);
    connectionStateModule.setDisplayOwnerRestoreCallback(restoreConnectionDisplayOwner, &displayPipelineModule);
    configureConnectionStateDispatchModule();
    configurePeriodicMaintenanceModule();
    configureLoopTailModule();
    configureLoopIngestModule();
    displayRestoreModule.begin(&display, &parser, &bleClient, &displayPreviewModule, &displayPipelineModule);
    displayOrchestrationModule.begin(&display, &bleClient, &bleQueueModule, &displayPreviewModule,
                                     &displayRestoreModule, &parser, &settingsManager, &volumeFadeModule,
                                     &speedMuteModule, &quietCoordinatorModule, &displayPipelineModule);
}

static void configureSystemLoopPhaseModules() {
    configureLoopDisplayModule();
    configureLoopConnectionEarlyModule();
    configureLoopPowerTouchModule();
    configureLoopRuntimeSnapshotModule();
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
    alpRuntimeModule.begin(settingsManager.get().alpEnabled);
    alpRuntimeModule.setEventBus(&systemEventBus);
}

static void configureRuntimeCoreModules() {
    configureRuntimeSensorModules();
}

void configureRuntimeModules() {
    parser.setAlertTableObserver(
        [](const AlertData* alerts, size_t count, uint8_t priorityIndex, uint32_t nowMs, void*) {
            productEventLog.observeV1Table(alerts, count, priorityIndex, nowMs);
        });
    configureRuntimeCoreModules();
}
