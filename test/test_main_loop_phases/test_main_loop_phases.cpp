#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/battery_manager.h"
#include "../mocks/ble_client.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/loop_connection_early_module.cpp"
#include "../../src/modules/system/loop_settings_prep_module.cpp"
#include "../../src/modules/system/loop_pre_ingest_module.cpp"
#include "../../src/modules/system/loop_ingest_module.cpp"
#include "../../src/modules/system/loop_display_module.cpp"
#include "../../src/modules/system/loop_post_display_module.cpp"
#include "../../src/modules/system/loop_runtime_snapshot_module.cpp"
#include "../../src/modules/system/loop_power_touch_module.cpp"
#include "../../src/modules/system/loop_tail_module.cpp"
#include "../../src/modules/system/periodic_maintenance_module.cpp"
#include "../../src/modules/wifi/wifi_runtime_module.cpp"

LoopConnectionEarlyModule loopConnectionEarlyModule;
LoopSettingsPrepModule loopSettingsPrepModule;
LoopPreIngestModule loopPreIngestModule;
LoopIngestModule loopIngestModule;
LoopDisplayModule loopDisplayModule;
LoopPostDisplayModule loopPostDisplayModule;
LoopRuntimeSnapshotModule loopRuntimeSnapshotModule;
WifiRuntimeModule wifiRuntimeModule;
PeriodicMaintenanceModule periodicMaintenanceModule;
LoopTailModule loopTailModule;
LoopPowerTouchModule loopPowerTouchModule;

#include "../../src/main_loop_phases.cpp"

namespace {

enum CallId {
    CALL_CONNECTION_RUNTIME = 1,
    CALL_SHOW_INITIAL_SCANNING,
    CALL_READ_PROXY_CONNECTED,
    CALL_READ_CONNECTION_RSSI,
    CALL_READ_PROXY_RSSI,
    CALL_DISPLAY_EARLY,
    CALL_TAP_GESTURE,
    CALL_READ_SETTINGS,
    CALL_OPEN_BOOT_READY,
    CALL_WIFI_PRIORITY_APPLY,
    CALL_BLE_PROCESS,
    CALL_BLE_DRAIN,
    CALL_COLLECT_PARSED_SIGNAL,
    CALL_RUN_PARSED_FRAME,
    CALL_RUN_LIGHTWEIGHT_REFRESH,
    CALL_DISPLAY_PIPELINE,
    CALL_AUTO_PUSH,
    CALL_READ_BLE_CONNECTED,
    CALL_READ_CAN_START_DMA,
    CALL_READ_DISPLAY_PREVIEW,
    CALL_WIFI_AUTO_START,
    CALL_WIFI_POLICY,
    CALL_WIFI_SET_TRANSITION_ADMISSION,
    CALL_WIFI_CADENCE,
    CALL_WIFI_MANAGER_PROCESS,
    CALL_READ_WIFI_SERVICE_ACTIVE,
    CALL_READ_WIFI_CONNECTED,
    CALL_READ_VISUAL_NOW,
    CALL_WIFI_VISUAL_SYNC,
    CALL_READ_DISPATCH_NOW,
    CALL_READ_FINAL_BLE_CONNECTED,
    CALL_CONNECTION_STATE_DISPATCH,
    CALL_PERF_REPORT,
    CALL_OBD_SETTINGS_SYNC,
    CALL_DEFERRED_SETTINGS_PERSIST,
    CALL_DEFERRED_SETTINGS_BACKUP,
    CALL_DEFERRED_BLE_BOND_BACKUP,
    CALL_STORE_SAVE,
    CALL_YIELD_ONE_TICK,
    CALL_POWER_PROCESS,
    CALL_TOUCH_UI_PROCESS,
    CALL_RECORD_LOOP_JITTER,
    CALL_REFRESH_DMA_CACHE,
    CALL_RECORD_HEAP_STATS,
};

int callLog[64];
size_t callLogCount = 0;

DisplayOrchestrationEarlyContext lastDisplayEarlyCtx;
DisplayOrchestrationParsedContext lastParsedCtx;
DisplayOrchestrationRefreshContext lastRefreshCtx;
ConnectionStateDispatchContext lastDispatchCtx;
WifiProcessCadenceContext lastWifiCadenceCtx;
uint32_t lastDisplayPipelineNowMs = 0;
uint32_t lastWifiVisualNowMs = 0;
bool lastWifiVisualActiveNow = false;
bool lastWifiDisplayPreviewRunning = false;
bool lastWifiBootSplashHoldActive = false;
bool lastWifiAllowTransitionWork = false;
bool observedTouchBootButton = false;
uint32_t observedLoopJitterUs = 0;
uint32_t observedFreeHeap = 0;
uint32_t observedLargestHeapBlock = 0;
uint32_t observedCachedFreeDma = 0;
uint32_t observedCachedLargestDma = 0;

void noteCall(int id) {
    if (callLogCount < (sizeof(callLog) / sizeof(callLog[0]))) {
        callLog[callLogCount++] = id;
    }
}

size_t countCalls(int id) {
    size_t count = 0;
    for (size_t index = 0; index < callLogCount; ++index) {
        if (callLog[index] == id) {
            ++count;
        }
    }
    return count;
}

void resetState() {
    callLogCount = 0;
    lastDisplayEarlyCtx = DisplayOrchestrationEarlyContext{};
    lastParsedCtx = DisplayOrchestrationParsedContext{};
    lastRefreshCtx = DisplayOrchestrationRefreshContext{};
    lastDispatchCtx = ConnectionStateDispatchContext{};
    lastWifiCadenceCtx = WifiProcessCadenceContext{};
    lastDisplayPipelineNowMs = 0;
    lastWifiVisualNowMs = 0;
    lastWifiVisualActiveNow = false;
    lastWifiDisplayPreviewRunning = false;
    lastWifiBootSplashHoldActive = false;
    lastWifiAllowTransitionWork = false;
    observedTouchBootButton = false;
    observedLoopJitterUs = 0;
    observedFreeHeap = 0;
    observedLargestHeapBlock = 0;
    observedCachedFreeDma = 0;
    observedCachedLargestDma = 0;
}

ConnectionRuntimeSnapshot runConnectionRuntime(
    void*,
    uint32_t,
    uint32_t,
    uint32_t,
    bool,
    uint32_t,
    bool) {
    noteCall(CALL_CONNECTION_RUNTIME);
    ConnectionRuntimeSnapshot snapshot;
    snapshot.connected = true;
    snapshot.receiving = true;
    snapshot.backpressured = false;
    snapshot.skipNonCore = false;
    snapshot.overloaded = false;
    snapshot.bootSplashHoldActive = true;
    snapshot.initialScanningScreenShown = false;
    snapshot.requestShowInitialScanning = true;
    return snapshot;
}

void showInitialScanning(void*) {
    noteCall(CALL_SHOW_INITIAL_SCANNING);
}

bool readProxyConnected(void*) {
    noteCall(CALL_READ_PROXY_CONNECTED);
    return true;
}

int readConnectionRssi(void*) {
    noteCall(CALL_READ_CONNECTION_RSSI);
    return -45;
}

int readProxyRssi(void*) {
    noteCall(CALL_READ_PROXY_RSSI);
    return -61;
}

void runDisplayEarly(void*, const DisplayOrchestrationEarlyContext& ctx) {
    noteCall(CALL_DISPLAY_EARLY);
    lastDisplayEarlyCtx = ctx;
}

void runTapGesture(void*, uint32_t) {
    noteCall(CALL_TAP_GESTURE);
}

LoopSettingsPrepValues readSettingsValues(void*) {
    noteCall(CALL_READ_SETTINGS);
    LoopSettingsPrepValues values;
    values.enableWifi = false;
    return values;
}

void openBootReadyGate(void*, uint32_t) {
    noteCall(CALL_OPEN_BOOT_READY);
}

void runWifiPriorityApply(void*, uint32_t) {
    noteCall(CALL_WIFI_PRIORITY_APPLY);
}

void runtimeBleProcess() {
    noteCall(CALL_BLE_PROCESS);
}

void runtimeBleDrain() {
    noteCall(CALL_BLE_DRAIN);
}

void providerBleProcess(void*) {
    runtimeBleProcess();
}

void providerBleDrain(void*) {
    runtimeBleDrain();
}

bool readBleBackpressure(void*) {
    return false;
}

uint32_t readDisplayNowMs(void*) {
    return 1500;
}

ParsedFrameSignal collectParsedSignal(void*) {
    noteCall(CALL_COLLECT_PARSED_SIGNAL);
    return ParsedFrameSignal{true, 1400};
}

DisplayOrchestrationParsedResult runParsedFrame(
    void*,
    const DisplayOrchestrationParsedContext& ctx) {
    noteCall(CALL_RUN_PARSED_FRAME);
    lastParsedCtx = ctx;
    return DisplayOrchestrationParsedResult{true};
}

DisplayOrchestrationRefreshResult runLightweightRefresh(
    void*,
    const DisplayOrchestrationRefreshContext& ctx) {
    noteCall(CALL_RUN_LIGHTWEIGHT_REFRESH);
    lastRefreshCtx = ctx;
    return DisplayOrchestrationRefreshResult{true};
}

void providerDisplayPipeline(void*, uint32_t nowMs) {
    noteCall(CALL_DISPLAY_PIPELINE);
    lastDisplayPipelineNowMs = nowMs;
}

void runAutoPush(void*) {
    noteCall(CALL_AUTO_PUSH);
}

bool readBleConnected(void*) {
    noteCall(CALL_READ_BLE_CONNECTED);
    return true;
}

bool readCanStartDma(void*) {
    noteCall(CALL_READ_CAN_START_DMA);
    return true;
}

bool readDisplayPreviewRunning(void*) {
    noteCall(CALL_READ_DISPLAY_PREVIEW);
    return true;
}

void runWifiAutoStartProcess(
    void*,
    uint32_t,
    uint32_t,
    bool,
    bool,
    bool,
    bool,
    bool& wifiManualStartIntentLatched,
    bool& wifiAutoStartDone) {
    noteCall(CALL_WIFI_AUTO_START);
    wifiManualStartIntentLatched = false;
    wifiAutoStartDone = true;
}

bool shouldRunWifiProcessingPolicy(void*) {
    noteCall(CALL_WIFI_POLICY);
    return true;
}

void setWifiTransitionAdmission(void*, bool allowTransitionWork) {
    noteCall(CALL_WIFI_SET_TRANSITION_ADMISSION);
    lastWifiAllowTransitionWork = allowTransitionWork;
}

WifiProcessCadenceDecision runWifiCadence(
    void*,
    const WifiProcessCadenceContext& ctx) {
    noteCall(CALL_WIFI_CADENCE);
    lastWifiCadenceCtx = ctx;
    return WifiProcessCadenceDecision{true};
}

void runtimeWifiManagerProcess() {
    noteCall(CALL_WIFI_MANAGER_PROCESS);
}

void providerWifiManagerProcess(void*) {
    runtimeWifiManagerProcess();
}

bool readWifiServiceActive(void*) {
    noteCall(CALL_READ_WIFI_SERVICE_ACTIVE);
    return false;
}

bool readWifiConnected(void*) {
    noteCall(CALL_READ_WIFI_CONNECTED);
    return true;
}

uint32_t readVisualNowMs(void*) {
    noteCall(CALL_READ_VISUAL_NOW);
    return 1700;
}

void runWifiVisualSync(void*, uint32_t nowMs, bool wifiVisualActiveNow, bool displayPreviewRunning, bool bootSplashHoldActive) {
    noteCall(CALL_WIFI_VISUAL_SYNC);
    lastWifiVisualNowMs = nowMs;
    lastWifiVisualActiveNow = wifiVisualActiveNow;
    lastWifiDisplayPreviewRunning = displayPreviewRunning;
    lastWifiBootSplashHoldActive = bootSplashHoldActive;
}

uint32_t readDispatchNowMs(void*) {
    noteCall(CALL_READ_DISPATCH_NOW);
    return 1600;
}

bool readFinalBleConnected(void*) {
    noteCall(CALL_READ_FINAL_BLE_CONNECTED);
    return false;
}

void runConnectionStateDispatch(void*, const ConnectionStateDispatchContext& ctx) {
    noteCall(CALL_CONNECTION_STATE_DISPATCH);
    lastDispatchCtx = ctx;
}

void runPerfReport(void*) {
    noteCall(CALL_PERF_REPORT);
}

void runTimeSave(void*, uint32_t) {}

void runObdSettingsSync(void*, uint32_t) {
    noteCall(CALL_OBD_SETTINGS_SYNC);
}

void runDeferredSettingsPersist(void*, uint32_t) {
    noteCall(CALL_DEFERRED_SETTINGS_PERSIST);
}

void runDeferredSettingsBackup(void*, uint32_t) {
    noteCall(CALL_DEFERRED_SETTINGS_BACKUP);
}

void runDeferredBleBondBackup(void*, uint32_t) {
    noteCall(CALL_DEFERRED_BLE_BOND_BACKUP);
}

void runStoreSave(void*, uint32_t) {
    noteCall(CALL_STORE_SAVE);
}

uint32_t perfTimestampUs(void*) {
    return 0;
}

void recordLoopDurationUs(void*, uint32_t) {}

uint32_t loopMicrosUs(void*) {
    return 3500;
}

void yieldOneTick(void*) {
    noteCall(CALL_YIELD_ONE_TICK);
}

void runPowerProcess(void*, uint32_t) {
    noteCall(CALL_POWER_PROCESS);
}

bool runTouchUiProcess(void*, uint32_t, bool bootButtonPressed) {
    noteCall(CALL_TOUCH_UI_PROCESS);
    observedTouchBootButton = bootButtonPressed;
    return true;
}

uint32_t microsNow(void*) {
    return 9000;
}

void recordLoopJitterUs(void*, uint32_t jitterUs) {
    noteCall(CALL_RECORD_LOOP_JITTER);
    observedLoopJitterUs = jitterUs;
}

void refreshDmaCache(void*) {
    noteCall(CALL_REFRESH_DMA_CACHE);
}

uint32_t readFreeHeap(void*) {
    return 111;
}

uint32_t readLargestHeapBlock(void*) {
    return 222;
}

uint32_t readCachedFreeDma(void*) {
    return 333;
}

uint32_t readCachedLargestDma(void*) {
    return 444;
}

void recordHeapStats(void*, uint32_t freeHeap, uint32_t largestHeapBlock, uint32_t cachedFreeDma, uint32_t cachedLargestDma) {
    noteCall(CALL_RECORD_HEAP_STATS);
    observedFreeHeap = freeHeap;
    observedLargestHeapBlock = largestHeapBlock;
    observedCachedFreeDma = cachedFreeDma;
    observedCachedLargestDma = cachedLargestDma;
}

void configureModules() {
    LoopConnectionEarlyModule::Providers connectionProviders;
    connectionProviders.runConnectionRuntime = runConnectionRuntime;
    connectionProviders.showInitialScanning = showInitialScanning;
    connectionProviders.readProxyConnected = readProxyConnected;
    connectionProviders.readConnectionRssi = readConnectionRssi;
    connectionProviders.readProxyRssi = readProxyRssi;
    connectionProviders.runDisplayEarly = runDisplayEarly;
    loopConnectionEarlyModule.begin(connectionProviders);

    LoopSettingsPrepModule::Providers settingsProviders;
    settingsProviders.runTapGesture = runTapGesture;
    settingsProviders.readSettingsValues = readSettingsValues;
    loopSettingsPrepModule.begin(settingsProviders);

    LoopPreIngestModule::Providers preIngestProviders;
    preIngestProviders.openBootReadyGate = openBootReadyGate;
    preIngestProviders.runWifiPriorityApply = runWifiPriorityApply;
    loopPreIngestModule.begin(preIngestProviders);

    LoopIngestModule::Providers ingestProviders;
    ingestProviders.runBleProcess = providerBleProcess;
    ingestProviders.runBleDrain = providerBleDrain;
    ingestProviders.readBleBackpressure = readBleBackpressure;
    loopIngestModule.begin(ingestProviders);

    LoopDisplayModule::Providers displayProviders;
    displayProviders.readDisplayNowMs = readDisplayNowMs;
    displayProviders.collectParsedSignal = collectParsedSignal;
    displayProviders.runParsedFrame = runParsedFrame;
    displayProviders.runLightweightRefresh = runLightweightRefresh;
    displayProviders.runDisplayPipeline = providerDisplayPipeline;
    loopDisplayModule.begin(displayProviders);

    LoopPostDisplayModule::Providers postDisplayProviders;
    postDisplayProviders.runAutoPush = runAutoPush;
    postDisplayProviders.readDispatchNowMs = readDispatchNowMs;
    postDisplayProviders.readBleConnectedNow = readFinalBleConnected;
    postDisplayProviders.runConnectionStateDispatch = runConnectionStateDispatch;
    loopPostDisplayModule.begin(postDisplayProviders);

    LoopRuntimeSnapshotModule::Providers runtimeSnapshotProviders;
    runtimeSnapshotProviders.readBleConnected = readBleConnected;
    runtimeSnapshotProviders.readCanStartDma = readCanStartDma;
    runtimeSnapshotProviders.readDisplayPreviewRunning = readDisplayPreviewRunning;
    loopRuntimeSnapshotModule.begin(runtimeSnapshotProviders);

    WifiRuntimeModule::Providers wifiProviders;
    wifiProviders.runWifiAutoStartProcess = runWifiAutoStartProcess;
    wifiProviders.shouldRunWifiProcessingPolicy = shouldRunWifiProcessingPolicy;
    wifiProviders.runWifiCadence = runWifiCadence;
    wifiProviders.setWifiTransitionAdmission = setWifiTransitionAdmission;
    wifiProviders.runWifiManagerProcess = providerWifiManagerProcess;
    wifiProviders.readWifiServiceActive = readWifiServiceActive;
    wifiProviders.readWifiConnected = readWifiConnected;
    wifiProviders.readVisualNowMs = readVisualNowMs;
    wifiProviders.runWifiVisualSync = runWifiVisualSync;
    wifiRuntimeModule.begin(wifiProviders);

    PeriodicMaintenanceModule::Providers maintenanceProviders;
    maintenanceProviders.runPerfReport = runPerfReport;
    maintenanceProviders.runObdSettingsSync = runObdSettingsSync;
    maintenanceProviders.runDeferredSettingsPersist = runDeferredSettingsPersist;
    maintenanceProviders.runDeferredSettingsBackup = runDeferredSettingsBackup;
    maintenanceProviders.runDeferredBleBondBackup = runDeferredBleBondBackup;
    maintenanceProviders.runStoreSave = runStoreSave;
    periodicMaintenanceModule.begin(maintenanceProviders);

    LoopTailModule::Providers tailProviders;
    tailProviders.perfTimestampUs = perfTimestampUs;
    tailProviders.loopMicrosUs = loopMicrosUs;
    tailProviders.runBleDrain = providerBleDrain;
    tailProviders.recordLoopJitterUs = recordLoopDurationUs;
    tailProviders.yieldOneTick = yieldOneTick;
    loopTailModule.begin(tailProviders);

    LoopPowerTouchModule::Providers powerTouchProviders;
    powerTouchProviders.runPowerProcess = runPowerProcess;
    powerTouchProviders.runTouchUiProcess = runTouchUiProcess;
    powerTouchProviders.microsNow = microsNow;
    powerTouchProviders.recordLoopJitterUs = recordLoopJitterUs;
    powerTouchProviders.refreshDmaCache = refreshDmaCache;
    powerTouchProviders.readFreeHeap = readFreeHeap;
    powerTouchProviders.readLargestHeapBlock = readLargestHeapBlock;
    powerTouchProviders.readCachedFreeDma = readCachedFreeDma;
    powerTouchProviders.readCachedLargestDma = readCachedLargestDma;
    powerTouchProviders.recordHeapStats = recordHeapStats;
    loopPowerTouchModule.begin(powerTouchProviders);
}

}  // namespace

void setUp() {
    resetState();
    configureModules();
}

void tearDown() {}

void test_main_loop_phases_preserve_expected_order_and_phase_contracts() {
    const LoopConnectionEarlyPhaseValues earlyValues =
        processLoopConnectionEarlyPhase(1000, 2000, 300, false, 0, false);

    TEST_ASSERT_TRUE(earlyValues.bootSplashHoldActive);
    TEST_ASSERT_TRUE(earlyValues.initialScanningScreenShown);
    TEST_ASSERT_TRUE(earlyValues.bleConnectedNow);
    TEST_ASSERT_TRUE(lastDisplayEarlyCtx.bootSplashHoldActive);
    TEST_ASSERT_TRUE(lastDisplayEarlyCtx.bleContext.v1Connected);
    TEST_ASSERT_TRUE(lastDisplayEarlyCtx.bleContext.proxyConnected);
    TEST_ASSERT_EQUAL(-45, lastDisplayEarlyCtx.bleContext.v1Rssi);
    TEST_ASSERT_EQUAL(-61, lastDisplayEarlyCtx.bleContext.proxyRssi);

    const LoopIngestPhaseValues ingestValues =
        processLoopIngestPhase(1000, false, 900, earlyValues.skipNonCoreThisLoop, earlyValues.overloadThisLoop);

    TEST_ASSERT_TRUE(ingestValues.bootReady);
    TEST_ASSERT_FALSE(ingestValues.bleBackpressure);
    TEST_ASSERT_FALSE(ingestValues.skipLateNonCoreThisLoop);
    TEST_ASSERT_FALSE(ingestValues.overloadLateThisLoop);
    TEST_ASSERT_FALSE(ingestValues.loopSettingsPrepValues.enableWifi);

    processLoopDisplayPreWifiPhase(
        1000,
        earlyValues.bootSplashHoldActive,
        ingestValues.overloadLateThisLoop);

    TEST_ASSERT_EQUAL(1500U, lastParsedCtx.nowMs);
    TEST_ASSERT_TRUE(lastParsedCtx.bootSplashHoldActive);
    TEST_ASSERT_EQUAL(1500U, lastRefreshCtx.nowMs);
    TEST_ASSERT_FALSE(lastRefreshCtx.overloadLateThisLoop);
    TEST_ASSERT_EQUAL(1500U, lastDisplayPipelineNowMs);

    const LoopWifiPhaseValues wifiValues =
        processLoopWifiPhase(
            1000,
            700,
            ingestValues.loopSettingsPrepValues.enableWifi,
            true,
            false,
            true,
            ingestValues.skipLateNonCoreThisLoop,
            ingestValues.bleBackpressure,
            ingestValues.overloadLateThisLoop,
            false,
            earlyValues.bootSplashHoldActive);

    TEST_ASSERT_TRUE(wifiValues.loopRuntimeSnapshotValues.bleConnected);
    TEST_ASSERT_FALSE(wifiValues.loopRuntimeSnapshotValues.canStartDma);
    TEST_ASSERT_EQUAL_UINT32(0U, countCalls(CALL_READ_CAN_START_DMA));
    TEST_ASSERT_TRUE(wifiValues.loopRuntimeSnapshotValues.displayPreviewRunning);
    TEST_ASSERT_TRUE(wifiValues.wifiAutoStartDone);
    TEST_ASSERT_FALSE(wifiValues.wifiManualStartIntentLatched);
    TEST_ASSERT_EQUAL(2000U, lastWifiCadenceCtx.minIntervalUs);
    TEST_ASSERT_EQUAL(1700U, lastWifiVisualNowMs);
    TEST_ASSERT_TRUE(lastWifiAllowTransitionWork);
    TEST_ASSERT_TRUE(lastWifiVisualActiveNow);
    TEST_ASSERT_TRUE(lastWifiDisplayPreviewRunning);
    TEST_ASSERT_TRUE(lastWifiBootSplashHoldActive);

    const LoopFinalizePhaseValues finalizeValues =
        processLoopFinalizePhase(
            1000,
            earlyValues.bootSplashHoldActive,
            wifiValues.loopRuntimeSnapshotValues.displayPreviewRunning,
            false,
            ingestValues.overloadLateThisLoop,
            250,
            4000,
            3000);

    TEST_ASSERT_EQUAL(1600UL, finalizeValues.dispatchNowMs);
    TEST_ASSERT_FALSE(finalizeValues.bleConnectedNow);
    TEST_ASSERT_EQUAL(500UL, finalizeValues.lastLoopUs);
    TEST_ASSERT_EQUAL(1600U, lastDispatchCtx.nowMs);
    TEST_ASSERT_TRUE(lastDispatchCtx.bootSplashHoldActive);
    TEST_ASSERT_TRUE(lastDispatchCtx.displayPreviewRunning);
    TEST_ASSERT_EQUAL(4000U, lastDispatchCtx.maxProcessGapMs);

    const int expectedOrder[] = {
        CALL_CONNECTION_RUNTIME,
        CALL_SHOW_INITIAL_SCANNING,
        CALL_READ_PROXY_CONNECTED,
        CALL_READ_CONNECTION_RSSI,
        CALL_READ_PROXY_RSSI,
        CALL_DISPLAY_EARLY,
        CALL_TAP_GESTURE,
        CALL_READ_SETTINGS,
        CALL_OPEN_BOOT_READY,
        CALL_WIFI_PRIORITY_APPLY,
        CALL_BLE_PROCESS,
        CALL_BLE_DRAIN,
        CALL_COLLECT_PARSED_SIGNAL,
        CALL_RUN_PARSED_FRAME,
        CALL_DISPLAY_PIPELINE,
        CALL_RUN_LIGHTWEIGHT_REFRESH,
        CALL_AUTO_PUSH,
        CALL_READ_BLE_CONNECTED,
        CALL_READ_DISPLAY_PREVIEW,
        CALL_WIFI_AUTO_START,
        CALL_WIFI_SET_TRANSITION_ADMISSION,
        CALL_WIFI_POLICY,
        CALL_WIFI_CADENCE,
        CALL_WIFI_MANAGER_PROCESS,
        CALL_READ_WIFI_SERVICE_ACTIVE,
        CALL_READ_WIFI_CONNECTED,
        CALL_READ_VISUAL_NOW,
        CALL_WIFI_VISUAL_SYNC,
        CALL_READ_DISPATCH_NOW,
        CALL_READ_FINAL_BLE_CONNECTED,
        CALL_CONNECTION_STATE_DISPATCH,
        CALL_PERF_REPORT,
        CALL_OBD_SETTINGS_SYNC,
        CALL_DEFERRED_SETTINGS_PERSIST,
        CALL_DEFERRED_SETTINGS_BACKUP,
        CALL_DEFERRED_BLE_BOND_BACKUP,
        CALL_STORE_SAVE,
        CALL_YIELD_ONE_TICK,
    };

    TEST_ASSERT_EQUAL_UINT32(sizeof(expectedOrder) / sizeof(expectedOrder[0]), callLogCount);
    for (size_t index = 0; index < callLogCount; ++index) {
        TEST_ASSERT_EQUAL_INT(expectedOrder[index], callLog[index]);
    }
}

void test_wifi_phase_reads_dma_probe_when_start_is_eligible() {
    const LoopWifiPhaseValues wifiValues =
        processLoopWifiPhase(
            1000,
            700,
            true,
            true,
            false,
            true,
            false,
            false,
            false,
            false,
            false);

    TEST_ASSERT_TRUE(wifiValues.loopRuntimeSnapshotValues.bleConnected);
    TEST_ASSERT_TRUE(wifiValues.loopRuntimeSnapshotValues.canStartDma);
    TEST_ASSERT_TRUE(wifiValues.loopRuntimeSnapshotValues.displayPreviewRunning);
    TEST_ASSERT_EQUAL_UINT32(1U, countCalls(CALL_READ_CAN_START_DMA));
}

void test_power_touch_phase_returns_early_with_explicit_settings_state_capture() {
    const bool shouldReturnEarly = shouldReturnEarlyFromLoopPowerTouchPhase(1000, 8500);

    TEST_ASSERT_TRUE(shouldReturnEarly);
    TEST_ASSERT_TRUE(observedTouchBootButton);
    TEST_ASSERT_EQUAL(500U, observedLoopJitterUs);
    TEST_ASSERT_EQUAL(111U, observedFreeHeap);
    TEST_ASSERT_EQUAL(222U, observedLargestHeapBlock);
    TEST_ASSERT_EQUAL(333U, observedCachedFreeDma);
    TEST_ASSERT_EQUAL(444U, observedCachedLargestDma);

    const int expectedOrder[] = {
        CALL_POWER_PROCESS,
        CALL_TOUCH_UI_PROCESS,
        CALL_RECORD_LOOP_JITTER,
        CALL_REFRESH_DMA_CACHE,
        CALL_RECORD_HEAP_STATS,
    };

    TEST_ASSERT_EQUAL_UINT32(sizeof(expectedOrder) / sizeof(expectedOrder[0]), callLogCount);
    for (size_t index = 0; index < callLogCount; ++index) {
        TEST_ASSERT_EQUAL_INT(expectedOrder[index], callLog[index]);
    }
}

void test_finalize_phase_defers_settings_persist_when_loop_overloaded() {
    const LoopFinalizePhaseValues finalizeValues =
        processLoopFinalizePhase(
            1000,
            false,
            false,
            false,
            true,
            250,
            4000,
            3000);

    TEST_ASSERT_EQUAL(1600UL, finalizeValues.dispatchNowMs);
    TEST_ASSERT_FALSE(finalizeValues.bleConnectedNow);
    TEST_ASSERT_EQUAL(500UL, finalizeValues.lastLoopUs);

    const int expectedOrder[] = {
        CALL_READ_DISPATCH_NOW,
        CALL_READ_FINAL_BLE_CONNECTED,
        CALL_CONNECTION_STATE_DISPATCH,
        CALL_PERF_REPORT,
        CALL_OBD_SETTINGS_SYNC,
        CALL_YIELD_ONE_TICK,
    };

    TEST_ASSERT_EQUAL_UINT32(sizeof(expectedOrder) / sizeof(expectedOrder[0]), callLogCount);
    for (size_t index = 0; index < callLogCount; ++index) {
        TEST_ASSERT_EQUAL_INT(expectedOrder[index], callLog[index]);
    }
}

void test_settings_early_return_phase_defers_settings_persist_and_forces_tail_drain() {
    const unsigned long lastLoopUs = processLoopSettingsEarlyReturnPhase(1000, 3000);

    TEST_ASSERT_EQUAL(500UL, lastLoopUs);

    const int expectedOrder[] = {
        CALL_PERF_REPORT,
        CALL_OBD_SETTINGS_SYNC,
        CALL_BLE_DRAIN,
        CALL_YIELD_ONE_TICK,
    };

    TEST_ASSERT_EQUAL_UINT32(sizeof(expectedOrder) / sizeof(expectedOrder[0]), callLogCount);
    for (size_t index = 0; index < callLogCount; ++index) {
        TEST_ASSERT_EQUAL_INT(expectedOrder[index], callLog[index]);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_main_loop_phases_preserve_expected_order_and_phase_contracts);
    RUN_TEST(test_wifi_phase_reads_dma_probe_when_start_is_eligible);
    RUN_TEST(test_power_touch_phase_returns_early_with_explicit_settings_state_capture);
    RUN_TEST(test_finalize_phase_defers_settings_persist_when_loop_overloaded);
    RUN_TEST(test_settings_early_return_phase_defers_settings_persist_and_forces_tail_drain);
    return UNITY_END();
}
