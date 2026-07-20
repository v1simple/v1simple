#include <Arduino.h>
#include <esp_heap_caps.h>

#include "main_loop_wiring.h"
#include "main_globals.h"
#include "main_internals.h"
#include "ble_client.h"
#include "settings.h"
#include "storage_manager.h"
#include "wifi_manager.h"
#include "perf_metrics.h"
#include "modules/perf/debug_macros.h"
#include "provider_callback_bindings.h"

#include "modules/auto_push/auto_push_module.h"
#include "modules/ble/ble_queue_module.h"
#include "modules/ble/connection_runtime_module.h"
#include "modules/ble/connection_state_cadence_module.h"
#include "modules/ble/connection_state_dispatch_module.h"
#include "modules/ble/connection_state_module.h"
#include "modules/display/display_orchestration_module.h"
#include "modules/display/display_pipeline_module.h"
#include "modules/display/display_preview_module.h"
#include "modules/obd/obd_settings_sync_module.h"
#include "modules/power/power_module.h"
#include "modules/system/loop_display_module.h"
#include "modules/system/loop_ingest_module.h"
#include "modules/system/loop_post_display_module.h"
#include "modules/system/loop_power_touch_module.h"
#include "modules/system/loop_pre_ingest_module.h"
#include "modules/system/loop_runtime_snapshot_module.h"
#include "modules/system/loop_settings_prep_module.h"
#include "modules/system/loop_tail_module.h"
#include "modules/system/loop_telemetry_module.h"
#include "modules/system/periodic_maintenance_module.h"
#include "modules/system/parsed_frame_event_module.h"
#include "modules/system/system_event_bus.h"
#include "modules/touch/tap_gesture_module.h"
#include "modules/touch/touch_ui_module.h"
#include "modules/wifi/wifi_priority_policy_module.h"

void configureLoopSettingsPrepModule() {
    LoopSettingsPrepModule::Providers loopSettingsPrepProviders;
    loopSettingsPrepProviders.runTapGesture =
        ProviderCallbackBindings::member<TapGestureModule, &TapGestureModule::process>;
    loopSettingsPrepProviders.tapGestureContext = &tapGestureModule;
    loopSettingsPrepProviders.readSettingsValues = [](void* ctx) -> LoopSettingsPrepValues {
        const V1Settings& settings = static_cast<SettingsManager*>(ctx)->get();
        LoopSettingsPrepValues values;
        values.enableWifi = settings.enableWifi;
        return values;
    };
    loopSettingsPrepProviders.settingsContext = &settingsManager;
    loopSettingsPrepModule.begin(loopSettingsPrepProviders);
}

void configureLoopRuntimeSnapshotModule() {
    LoopRuntimeSnapshotModule::Providers loopRuntimeSnapshotProviders;
    loopRuntimeSnapshotProviders.readBleConnected =
        ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::isConnected>;
    loopRuntimeSnapshotProviders.bleConnectedContext = &bleClient;
    loopRuntimeSnapshotProviders.readCanStartDma = [](void* ctx) -> bool {
        return static_cast<WiFiManager*>(ctx)->canStartSetupMode(nullptr, nullptr);
    };
    loopRuntimeSnapshotProviders.canStartDmaContext = &wifiManager;
    // Keep connection-state display transitions gated through the ended-but-
    // not-yet-restored interval as well as while preview frames are active.
    loopRuntimeSnapshotProviders.readDisplayPreviewRunning =
        ProviderCallbackBindings::member<DisplayPreviewModule, &DisplayPreviewModule::ownsPresentation>;
    loopRuntimeSnapshotProviders.displayPreviewContext = &displayPreviewModule;
    loopRuntimeSnapshotModule.begin(loopRuntimeSnapshotProviders);
}

void configureLoopPostDisplayModule() {
    LoopPostDisplayModule::Providers loopPostDisplayProviders;
    loopPostDisplayProviders.runAutoPush = ProviderCallbackBindings::member<AutoPushModule, &AutoPushModule::process>;
    loopPostDisplayProviders.autoPushContext = &autoPushModule;
    loopPostDisplayProviders.readDispatchNowMs = [](void*) -> uint32_t { return millis(); };
    loopPostDisplayProviders.readBleConnectedNow =
        ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::isConnected>;
    loopPostDisplayProviders.bleConnectedContext = &bleClient;
    loopPostDisplayProviders.runConnectionStateDispatch =
        ProviderCallbackBindings::memberDiscardReturn<ConnectionStateDispatchModule,
                                                      &ConnectionStateDispatchModule::process>;
    loopPostDisplayProviders.connectionDispatchContext = &connectionStateDispatchModule;
    loopPostDisplayModule.begin(loopPostDisplayProviders);
}

static void refreshStorageDmaHeapCache(void*) {
    StorageManager::updateDmaHeapCache();
}

static uint32_t readCurrentFreeHeap(void*) {
    return ESP.getFreeHeap();
}

static uint32_t readCurrentLargestHeapBlock(void*) {
    return static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

static uint32_t readCachedFreeDmaHeap(void*) {
    return StorageManager::getCachedFreeDma();
}

static uint32_t readCachedLargestDmaHeap(void*) {
    return StorageManager::getCachedLargestDma();
}

static void recordHeapStatsSample(void*, uint32_t freeHeap, uint32_t largestHeapBlock, uint32_t cachedFreeDma,
                                  uint32_t cachedLargestDma) {
    perfRecordHeapStats(freeHeap, largestHeapBlock, cachedFreeDma, cachedLargestDma);
}

void configureLoopPowerTouchModule() {
    LoopPowerTouchModule::Providers loopPowerTouchProviders;
    loopPowerTouchProviders.timestampUs = [](void*) -> uint32_t { return PERF_TIMESTAMP_US(); };
    loopPowerTouchProviders.microsNow = [](void*) -> uint32_t { return micros(); };
    loopPowerTouchProviders.runPowerProcess = ProviderCallbackBindings::member<PowerModule, &PowerModule::process>;
    loopPowerTouchProviders.powerContext = &powerModule;
    loopPowerTouchProviders.runTouchUiProcess =
        ProviderCallbackBindings::member<TouchUiModule, &TouchUiModule::process>;
    loopPowerTouchProviders.touchUiContext = &touchUiModule;
    loopPowerTouchProviders.recordTouchUs = [](void*, uint32_t elapsedUs) { perfRecordTouchUs(elapsedUs); };
    loopPowerTouchProviders.recordLoopJitterUs = [](void*, uint32_t jitterUs) { perfRecordLoopJitterUs(jitterUs); };
    loopPowerTouchProviders.refreshDmaCache = refreshStorageDmaHeapCache;
    loopPowerTouchProviders.readFreeHeap = readCurrentFreeHeap;
    loopPowerTouchProviders.readLargestHeapBlock = readCurrentLargestHeapBlock;
    loopPowerTouchProviders.readCachedFreeDma = readCachedFreeDmaHeap;
    loopPowerTouchProviders.readCachedLargestDma = readCachedLargestDmaHeap;
    loopPowerTouchProviders.recordHeapStats = recordHeapStatsSample;
    loopPowerTouchModule.begin(loopPowerTouchProviders);
}

void configureLoopPreIngestModule() {
    LoopPreIngestModule::Providers loopPreIngestProviders;
    loopPreIngestProviders.openBootReadyGate = [](void*, uint32_t nowMs) {
        bleClient.setBootReady(true);
        SerialLog.printf("[Boot] Ready gate opened at %lu ms (timeout)\n", static_cast<unsigned long>(nowMs));
    };
    loopPreIngestProviders.runWifiPriorityApply = [](void* ctx, uint32_t nowMs) {
        static_cast<WifiPriorityPolicyModule*>(ctx)->apply(nowMs, bleClient, wifiManager);
    };
    loopPreIngestProviders.wifiPriorityContext = &wifiPriorityPolicyModule;
    loopPreIngestModule.begin(loopPreIngestProviders);
}

void configureConnectionRuntimeModule() {
    ConnectionRuntimeModule::Providers connectionRuntimeProviders;
    connectionRuntimeProviders.isBleConnected =
        ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::isConnected>;
    connectionRuntimeProviders.isBackpressured =
        ProviderCallbackBindings::member<BleQueueModule, &BleQueueModule::isBackpressured>;
    connectionRuntimeProviders.getLastRxMillis =
        ProviderCallbackBindings::member<BleQueueModule, &BleQueueModule::getLastRxMillis>;
    connectionRuntimeProviders.bleContext = &bleClient;
    connectionRuntimeProviders.queueContext = &bleQueueModule;
    connectionRuntimeModule.begin(connectionRuntimeProviders);
}

void configureConnectionStateDispatchModule() {
    ConnectionStateDispatchModule::Providers connectionStateDispatchProviders;
    connectionStateDispatchProviders.runCadence =
        ProviderCallbackBindings::member<ConnectionStateCadenceModule, &ConnectionStateCadenceModule::process>;
    connectionStateDispatchProviders.cadenceContext = &connectionStateCadenceModule;
    connectionStateDispatchProviders.runConnectionStateProcess =
        ProviderCallbackBindings::memberDiscardReturn<ConnectionStateModule, &ConnectionStateModule::process>;
    connectionStateDispatchProviders.connectionStateContext = &connectionStateModule;
    connectionStateDispatchProviders.recordDecision = [](void*, const ConnectionStateDispatchDecision& decision) {
        PERF_INC(connectionDispatchRuns);
        if (decision.cadence.displayUpdateDue) {
            PERF_INC(connectionCadenceDisplayDue);
        }
        if (decision.cadence.holdScanDwell) {
            PERF_INC(connectionCadenceHoldScanDwell);
        }
        if (decision.elapsedSinceLastProcessMs > 0) {
            PERF_MAX(connectionStateProcessGapMaxMs, decision.elapsedSinceLastProcessMs);
        }
        if (decision.watchdogForced) {
            PERF_INC(connectionStateWatchdogForces);
        }
        if (decision.ranConnectionStateProcess) {
            PERF_INC(connectionStateProcessRuns);
        }
    };
    connectionStateDispatchModule.begin(connectionStateDispatchProviders);
}

void configurePeriodicMaintenanceModule() {
    obdSettingsSyncModule.begin(&settingsManager, &obdRuntimeModule);

    PeriodicMaintenanceModule::Providers periodicMaintenanceProviders;
    periodicMaintenanceProviders.timestampUs = [](void*) -> uint32_t { return PERF_TIMESTAMP_US(); };
    periodicMaintenanceProviders.runPerfReport = [](void*) { perfMetricsCheckReport(); };
    periodicMaintenanceProviders.recordPerfReportUs = [](void*, uint32_t elapsedUs) {
        perfRecordPerfReportUs(elapsedUs);
    };
    periodicMaintenanceProviders.runObdSettingsSync =
        ProviderCallbackBindings::member<ObdSettingsSyncModule, &ObdSettingsSyncModule::process>;
    periodicMaintenanceProviders.obdSettingsSyncContext = &obdSettingsSyncModule;
    periodicMaintenanceProviders.runDeferredSettingsPersist =
        ProviderCallbackBindings::member<SettingsManager, &SettingsManager::serviceDeferredPersist>;
    periodicMaintenanceProviders.deferredSettingsPersistContext = &settingsManager;
    periodicMaintenanceProviders.runDeferredSettingsBackup =
        ProviderCallbackBindings::member<SettingsManager, &SettingsManager::serviceDeferredBackup>;
    periodicMaintenanceProviders.deferredSettingsBackupContext = &settingsManager;
    periodicMaintenanceProviders.runDeferredBleBondBackup =
        ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::serviceDeferredBondBackup>;
    periodicMaintenanceProviders.deferredBleBondBackupContext = &bleClient;
    periodicMaintenanceProviders.runStoreSave = [](void*, uint32_t nowMs) { processV1DeviceStoreSave(nowMs); };
    periodicMaintenanceModule.begin(periodicMaintenanceProviders);
}

void configureLoopTailModule() {
    LoopTailModule::Providers loopTailProviders;
    loopTailProviders.perfTimestampUs = [](void*) -> uint32_t { return PERF_TIMESTAMP_US(); };
    loopTailProviders.loopMicrosUs = [](void*) -> uint32_t { return micros(); };
    loopTailProviders.runBleDrain = ProviderCallbackBindings::member<BleQueueModule, &BleQueueModule::process>;
    loopTailProviders.bleDrainContext = &bleQueueModule;
    loopTailProviders.recordBleDrainUs = [](void*, uint32_t elapsedUs) { perfRecordBleDrainUs(elapsedUs); };
    loopTailProviders.recordLoopJitterUs = [](void*, uint32_t jitterUs) { perfRecordLoopJitterUs(jitterUs); };
    loopTailProviders.yieldOneTick = [](void*) { vTaskDelay(pdMS_TO_TICKS(1)); };
    loopTailModule.begin(loopTailProviders);
}

void configureLoopTelemetryModule() {
    LoopTelemetryModule::Providers loopTelemetryProviders;
    loopTelemetryProviders.microsNow = [](void*) -> uint32_t { return micros(); };
    loopTelemetryProviders.refreshDmaCache = refreshStorageDmaHeapCache;
    loopTelemetryProviders.readFreeHeap = readCurrentFreeHeap;
    loopTelemetryProviders.readLargestHeapBlock = readCurrentLargestHeapBlock;
    loopTelemetryProviders.readCachedFreeDma = readCachedFreeDmaHeap;
    loopTelemetryProviders.readCachedLargestDma = readCachedLargestDmaHeap;
    loopTelemetryProviders.recordHeapStats = recordHeapStatsSample;
    loopTelemetryModule.begin(loopTelemetryProviders);
}

void configureLoopIngestModule() {
    LoopIngestModule::Providers loopIngestProviders;
    loopIngestProviders.timestampUs = [](void*) -> uint32_t { return PERF_TIMESTAMP_US(); };
    loopIngestProviders.runBleProcess = ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::process>;
    loopIngestProviders.bleProcessContext = &bleClient;
    loopIngestProviders.recordBleProcessUs = [](void* ctx, uint32_t elapsedUs) {
        perfRecordBleProcessUs(elapsedUs);
        static_cast<V1BLEClient*>(ctx)->noteBleProcessDuration(elapsedUs);
    };
    loopIngestProviders.bleProcessPerfContext = &bleClient;
    loopIngestProviders.runBleDrain = ProviderCallbackBindings::member<BleQueueModule, &BleQueueModule::process>;
    loopIngestProviders.bleDrainContext = &bleQueueModule;
    loopIngestProviders.recordBleDrainUs = [](void*, uint32_t elapsedUs) { perfRecordBleDrainUs(elapsedUs); };
    loopIngestProviders.readBleBackpressure =
        ProviderCallbackBindings::member<BleQueueModule, &BleQueueModule::isBackpressured>;
    loopIngestProviders.bleBackpressureContext = &bleQueueModule;
    loopIngestModule.begin(loopIngestProviders);
}

void configureLoopDisplayModule() {
    LoopDisplayModule::Providers loopDisplayProviders;
    loopDisplayProviders.readDisplayNowMs = [](void*) -> uint32_t { return millis(); };
    loopDisplayProviders.collectParsedSignal = [](void* ctx) -> ParsedFrameSignal {
        BleQueueModule* queue = static_cast<BleQueueModule*>(ctx);
        return ParsedFrameEventModule::collect(queue->consumeParsedFlag(), queue->getLastParsedTimestamp(),
                                               systemEventBus);
    };
    loopDisplayProviders.parsedSignalContext = &bleQueueModule;
    loopDisplayProviders.runParsedFrame =
        ProviderCallbackBindings::member<DisplayOrchestrationModule, &DisplayOrchestrationModule::processParsedFrame>;
    loopDisplayProviders.parsedFrameContext = &displayOrchestrationModule;
    loopDisplayProviders.runLightweightRefresh =
        ProviderCallbackBindings::member<DisplayOrchestrationModule,
                                         &DisplayOrchestrationModule::processLightweightRefresh>;
    loopDisplayProviders.lightweightRefreshContext = &displayOrchestrationModule;
    loopDisplayProviders.runDisplayPipeline =
        ProviderCallbackBindings::member<DisplayPipelineModule, &DisplayPipelineModule::handleParsed>;
    loopDisplayProviders.displayPipelineContext = &displayPipelineModule;
    loopDisplayProviders.runBlinkRefresh =
        ProviderCallbackBindings::member<DisplayPipelineModule, &DisplayPipelineModule::refreshBlinkTick>;
    loopDisplayProviders.blinkRefreshContext = &displayPipelineModule;
    loopDisplayProviders.timestampUs = [](void*) -> uint32_t { return PERF_TIMESTAMP_US(); };
    loopDisplayProviders.recordDispPipeUs = [](void* ctx, uint32_t elapsedUs) {
        perfRecordDispPipeUs(elapsedUs);
        static_cast<V1BLEClient*>(ctx)->noteDisplayPipelineDuration(elapsedUs);
    };
    loopDisplayProviders.dispPipePerfContext = &bleClient;
    loopDisplayProviders.recordNotifyToDisplayMs = [](void*, uint32_t elapsedMs) {
        perfRecordNotifyToDisplayMs(elapsedMs);
    };
    loopDisplayModule.begin(loopDisplayProviders);
}
