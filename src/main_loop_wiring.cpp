#include <Arduino.h>

#include "main_loop_wiring.h"
#include "main_globals.h"
#include "main_internals.h"
#include "ble_client.h"
#include "settings.h"
#include "storage_manager.h"
#include "wifi_manager.h"
#include "provider_callback_bindings.h"

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
#include "modules/system/loop_power_touch_module.h"
#include "modules/system/loop_runtime_snapshot_module.h"
#include "modules/system/loop_tail_module.h"
#include "modules/system/periodic_maintenance_module.h"
#include "modules/system/parsed_frame_event_module.h"
#include "modules/system/system_event_bus.h"
#include "modules/touch/touch_ui_module.h"

void configureLoopRuntimeSnapshotModule() {
    LoopRuntimeSnapshotModule::Providers loopRuntimeSnapshotProviders;
    loopRuntimeSnapshotProviders.readBleConnected =
        ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::isConnected>;
    loopRuntimeSnapshotProviders.bleConnectedContext = &bleClient;
    // Keep connection-state display transitions gated through the ended-but-
    // not-yet-restored interval as well as while preview frames are active.
    loopRuntimeSnapshotProviders.readDisplayPreviewRunning =
        ProviderCallbackBindings::member<DisplayPreviewModule, &DisplayPreviewModule::ownsPresentation>;
    loopRuntimeSnapshotProviders.displayPreviewContext = &displayPreviewModule;
    loopRuntimeSnapshotModule.begin(loopRuntimeSnapshotProviders);
}

void configureLoopPowerTouchModule() {
    LoopPowerTouchModule::Providers loopPowerTouchProviders;
    loopPowerTouchProviders.runPowerProcess = ProviderCallbackBindings::member<PowerModule, &PowerModule::process>;
    loopPowerTouchProviders.powerContext = &powerModule;
    loopPowerTouchProviders.readPresentationSuppressed =
        ProviderCallbackBindings::member<PowerModule, &PowerModule::ownsDisplayPresentation>;
    loopPowerTouchProviders.presentationContext = &powerModule;
    loopPowerTouchProviders.runTouchUiProcess =
        ProviderCallbackBindings::member<TouchUiModule, &TouchUiModule::process>;
    loopPowerTouchProviders.touchUiContext = &touchUiModule;
    loopPowerTouchModule.begin(loopPowerTouchProviders);
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
    connectionStateDispatchModule.begin(connectionStateDispatchProviders);
}

void configurePeriodicMaintenanceModule() {
    obdSettingsSyncModule.begin(&settingsManager, &obdRuntimeModule);

    PeriodicMaintenanceModule::Providers periodicMaintenanceProviders;
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
    loopTailProviders.loopMicrosUs = [](void*) -> uint32_t { return micros(); };
    loopTailProviders.runBleDrain = ProviderCallbackBindings::member<BleQueueModule, &BleQueueModule::process>;
    loopTailProviders.bleDrainContext = &bleQueueModule;
    loopTailProviders.yieldOneTick = [](void*) { vTaskDelay(pdMS_TO_TICKS(1)); };
    const bool loopTailConfigured = loopTailModule.begin(loopTailProviders);
    configASSERT(loopTailConfigured);
}

void configureLoopIngestModule() {
    LoopIngestModule::Providers loopIngestProviders;
    loopIngestProviders.runBleProcess = ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::process>;
    loopIngestProviders.bleProcessContext = &bleClient;
    loopIngestProviders.runBleDrain = ProviderCallbackBindings::member<BleQueueModule, &BleQueueModule::process>;
    loopIngestProviders.bleDrainContext = &bleQueueModule;
    loopIngestProviders.readBleBackpressure =
        ProviderCallbackBindings::member<BleQueueModule, &BleQueueModule::isBackpressured>;
    loopIngestProviders.bleBackpressureContext = &bleQueueModule;
    const bool loopIngestConfigured = loopIngestModule.begin(loopIngestProviders);
    configASSERT(loopIngestConfigured);
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
    loopDisplayModule.begin(loopDisplayProviders);
}
