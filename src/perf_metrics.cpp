/**
 * Low-Overhead Performance Metrics Implementation
 */

#include "perf_metrics.h"
#include "ble_client.h"
#include "display_drawn_region.h"
#include "perf_sd_logger.h"
#include "storage_manager.h"
#include "settings.h"
#include "main_globals.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/speed/speed_source_selector.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/gps/gps_publishers.h"
#include "modules/system/system_event_bus.h"
#include "modules/wifi/wifi_auto_start_module.h"
#if PERF_METRICS && PERF_MONITORING && !defined(UNIT_TEST)
#include "modules/alp/alp_sd_logger.h"
#endif
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>

// Global instances
PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;

#if PERF_METRICS
PerfLatency perfLatency;
#endif

#if PERF_METRICS && PERF_MONITORING
bool perfDebugEnabled = false;
uint32_t perfLastReportMs = 0;
#endif

// Session minima for true MALLOC_CAP_DMA heap (updated only in sampled snapshot path).
static uint32_t sDmaFreeCapMin = UINT32_MAX;
static uint32_t sDmaLargestCapMin = UINT32_MAX;
static std::atomic<uint32_t> sPrevWindowLoopMaxUs{0};
static std::atomic<uint32_t> sPrevWindowWifiMaxUs{0};
static std::atomic<uint32_t> sPrevWindowBleProcessMaxUs{0};
static std::atomic<uint32_t> sPrevWindowDispPipeMaxUs{0};
static std::atomic<uint8_t> sConnectionCycleStateCode{0};
static std::atomic<uint32_t> sConnectionCycleTimeInStateMs{0};
static std::atomic<uint32_t> sConnectionCycleTransitionsTotal{0};
static std::atomic<uint32_t> sConnectionCycleTeardownDurationMs{0};
static std::atomic<uint32_t> sConnectionCycleObdRetryAttemptsTotal{0};
static std::atomic<uint32_t> sConnectionCycleWifiManualPhoneKicksTotal{0};
static std::atomic<uint8_t> sConnectionCycleProxyNoClientLatched{0};
static std::atomic<uint8_t> sDisplayRenderScenario{
    static_cast<uint8_t>(PerfDisplayRenderScenario::None)};
static std::atomic<bool> sSdCapturePaused{false};

void perfMetricsInit() {
    perfCounters.reset();
    perfExtended.reset();
    sDmaFreeCapMin = UINT32_MAX;
    sDmaLargestCapMin = UINT32_MAX;
    sPrevWindowLoopMaxUs.store(0, std::memory_order_relaxed);
    sPrevWindowWifiMaxUs.store(0, std::memory_order_relaxed);
    sPrevWindowBleProcessMaxUs.store(0, std::memory_order_relaxed);
    sPrevWindowDispPipeMaxUs.store(0, std::memory_order_relaxed);
    sConnectionCycleStateCode.store(0, std::memory_order_relaxed);
    sConnectionCycleTimeInStateMs.store(0, std::memory_order_relaxed);
    sConnectionCycleTransitionsTotal.store(0, std::memory_order_relaxed);
    sConnectionCycleTeardownDurationMs.store(0, std::memory_order_relaxed);
    sConnectionCycleObdRetryAttemptsTotal.store(0, std::memory_order_relaxed);
    sConnectionCycleWifiManualPhoneKicksTotal.store(0, std::memory_order_relaxed);
    sConnectionCycleProxyNoClientLatched.store(0, std::memory_order_relaxed);
    sDisplayRenderScenario.store(
        static_cast<uint8_t>(PerfDisplayRenderScenario::None), std::memory_order_relaxed);
    sSdCapturePaused.store(false, std::memory_order_relaxed);
#if PERF_METRICS
    perfLatency.reset();
#if PERF_MONITORING
    perfDebugEnabled = false;
    perfLastReportMs = millis();
#endif
#endif
}

void perfMetricsReset() {
    perfCounters.reset();
    perfExtended.reset();
    sDmaFreeCapMin = UINT32_MAX;
    sDmaLargestCapMin = UINT32_MAX;
    sPrevWindowLoopMaxUs.store(0, std::memory_order_relaxed);
    sPrevWindowWifiMaxUs.store(0, std::memory_order_relaxed);
    sPrevWindowBleProcessMaxUs.store(0, std::memory_order_relaxed);
    sPrevWindowDispPipeMaxUs.store(0, std::memory_order_relaxed);
    sConnectionCycleStateCode.store(0, std::memory_order_relaxed);
    sConnectionCycleTimeInStateMs.store(0, std::memory_order_relaxed);
    sConnectionCycleTransitionsTotal.store(0, std::memory_order_relaxed);
    sConnectionCycleTeardownDurationMs.store(0, std::memory_order_relaxed);
    sConnectionCycleObdRetryAttemptsTotal.store(0, std::memory_order_relaxed);
    sConnectionCycleWifiManualPhoneKicksTotal.store(0, std::memory_order_relaxed);
    sConnectionCycleProxyNoClientLatched.store(0, std::memory_order_relaxed);
    sDisplayRenderScenario.store(
        static_cast<uint8_t>(PerfDisplayRenderScenario::None), std::memory_order_relaxed);
    sSdCapturePaused.store(false, std::memory_order_relaxed);
#if PERF_METRICS
    perfLatency.reset();
#endif
}

namespace {
static constexpr uint32_t kLatencyBucketsMs[PerfHistogramMs::kBucketCount] = {
    1, 2, 5, 10, 20, 50, 100, 200, 500, 1000
};
// Keep aligned with UI scan dwell target so "fast exit" remains actionable.
static constexpr uint32_t kFastScanExitThresholdMs = 400;
static portMUX_TYPE sPerfSnapshotMux = portMUX_INITIALIZER_UNLOCKED;

static PerfDisplayRenderScenario currentDisplayRenderScenario() {
    return static_cast<PerfDisplayRenderScenario>(
        sDisplayRenderScenario.load(std::memory_order_relaxed));
}

static void recordDisplayScenarioRenderCount(PerfDisplayRenderScenario scenario) {
    switch (scenario) {
        case PerfDisplayRenderScenario::Live:
            perfExtended.displayLiveScenarioRenderCount++;
            break;
        case PerfDisplayRenderScenario::Resting:
            perfExtended.displayRestingScenarioRenderCount++;
            break;
        case PerfDisplayRenderScenario::Persisted:
            perfExtended.displayPersistedScenarioRenderCount++;
            break;
        case PerfDisplayRenderScenario::PreviewFirstFrame:
        case PerfDisplayRenderScenario::PreviewSteadyFrame:
            perfExtended.displayPreviewScenarioRenderCount++;
            break;
        case PerfDisplayRenderScenario::Restore:
            perfExtended.displayRestoreScenarioRenderCount++;
            break;
        case PerfDisplayRenderScenario::None:
        default:
            break;
    }
}

static void recordDisplayScenarioRenderMax(PerfDisplayRenderScenario scenario, uint32_t us) {
    switch (scenario) {
        case PerfDisplayRenderScenario::Live:
            if (us > perfExtended.displayLiveRenderMaxUs) {
                perfExtended.displayLiveRenderMaxUs = us;
            }
            break;
        case PerfDisplayRenderScenario::Resting:
            if (us > perfExtended.displayRestingRenderMaxUs) {
                perfExtended.displayRestingRenderMaxUs = us;
            }
            break;
        case PerfDisplayRenderScenario::Persisted:
            if (us > perfExtended.displayPersistedRenderMaxUs) {
                perfExtended.displayPersistedRenderMaxUs = us;
            }
            break;
        case PerfDisplayRenderScenario::PreviewFirstFrame:
            if (us > perfExtended.displayPreviewRenderMaxUs) {
                perfExtended.displayPreviewRenderMaxUs = us;
            }
            if (us > perfExtended.displayPreviewFirstRenderMaxUs) {
                perfExtended.displayPreviewFirstRenderMaxUs = us;
            }
            break;
        case PerfDisplayRenderScenario::PreviewSteadyFrame:
            if (us > perfExtended.displayPreviewRenderMaxUs) {
                perfExtended.displayPreviewRenderMaxUs = us;
            }
            if (us > perfExtended.displayPreviewSteadyRenderMaxUs) {
                perfExtended.displayPreviewSteadyRenderMaxUs = us;
            }
            break;
        case PerfDisplayRenderScenario::Restore:
            if (us > perfExtended.displayRestoreRenderMaxUs) {
                perfExtended.displayRestoreRenderMaxUs = us;
            }
            break;
        case PerfDisplayRenderScenario::None:
        default:
            break;
    }
}

static void addLatencySample(PerfHistogramMs& hist, uint32_t ms) {
    if (ms > hist.maxMs) {
        hist.maxMs = ms;
    }
    // Always increment total - values > max bucket go into overflow
    hist.total++;
    for (size_t i = 0; i < PerfHistogramMs::kBucketCount; ++i) {
        if (ms <= kLatencyBucketsMs[i]) {
            hist.buckets[i]++;
            return;
        }
    }
    // Value exceeds all buckets - counted in total but not in any bucket
    hist.overflow++;
}

static const char* wifiApTransitionReasonNameInternal(uint32_t reasonCode) {
    switch (static_cast<PerfWifiApTransitionReason>(reasonCode)) {
        case PerfWifiApTransitionReason::Startup:
            return "startup";
        case PerfWifiApTransitionReason::StopManual:
            return "stop_manual";
        case PerfWifiApTransitionReason::StopTimeout:
            return "stop_timeout";
        case PerfWifiApTransitionReason::StopNoClients:
            return "stop_no_clients";
        case PerfWifiApTransitionReason::StopNoClientsAuto:
            return "stop_no_clients_auto";
        case PerfWifiApTransitionReason::DropLowDma:
            return "drop_low_dma";
        case PerfWifiApTransitionReason::DropIdleSta:
            return "drop_idle_sta";
        case PerfWifiApTransitionReason::StopPoweroff:
            return "stop_poweroff";
        case PerfWifiApTransitionReason::StopOther:
            return "stop_other";
        case PerfWifiApTransitionReason::Unknown:
        default:
            return "unknown";
    }
}

static const char* proxyAdvertisingTransitionReasonNameInternal(uint32_t reasonCode) {
    switch (static_cast<PerfProxyAdvertisingTransitionReason>(reasonCode)) {
        case PerfProxyAdvertisingTransitionReason::StartConnected:
            return "start_connected";
        case PerfProxyAdvertisingTransitionReason::StartWifiPriorityResume:
            return "start_wifi_priority_resume";
        case PerfProxyAdvertisingTransitionReason::StartRetryWindow:
            return "start_retry_window";
        case PerfProxyAdvertisingTransitionReason::StartAppDisconnect:
            return "start_app_disconnect";
        case PerfProxyAdvertisingTransitionReason::StartDirect:
            return "start_direct";
        case PerfProxyAdvertisingTransitionReason::StopWifiPriority:
            return "stop_wifi_priority";
        case PerfProxyAdvertisingTransitionReason::StopNoClientTimeout:
            return "stop_no_client_timeout";
        case PerfProxyAdvertisingTransitionReason::StopIdleWindow:
            return "stop_idle_window";
        case PerfProxyAdvertisingTransitionReason::StopBeforeV1Connect:
            return "stop_before_v1_connect";
        case PerfProxyAdvertisingTransitionReason::StopV1Disconnect:
            return "stop_v1_disconnect";
        case PerfProxyAdvertisingTransitionReason::StopAppConnected:
            return "stop_app_connected";
        case PerfProxyAdvertisingTransitionReason::StopOther:
            return "stop_other";
        case PerfProxyAdvertisingTransitionReason::Unknown:
        default:
            return "unknown";
    }
}

static const char* connectionCycleStateNameInternal(uint8_t stateCode) {
    switch (stateCode) {
        case 0:
            return "scan_v1";
        case 1:
            return "v1_settling";
        case 2:
            return "obd_scan";
        case 3:
            return "obd_connect";
        case 4:
            return "obd_settled";
        case 5:
            return "proxy_open";
        case 6:
            return "wifi_open";
        case 7:
            return "steady";
        case 8:
            return "teardown";
        default:
            return "unknown";
    }
}

struct RuntimeSnapshotCaptureContext {
    uint32_t nowMs = 0;
    uint32_t freeHeap = 0;
    uint32_t largestHeap = 0;
    uint32_t freeDma = 0;
    uint32_t largestDma = 0;
    uint32_t freeDmaCap = 0;
    uint32_t largestDmaCap = 0;
    uint32_t psramTotal = 0;
    uint32_t psramFree = 0;
    uint32_t psramLargest = 0;
    ObdRuntimeStatus obdStatus = {};
    SpeedSelectorStatus speedStatus = {};
    WifiAutoStartDecisionSnapshot wifiAutoStart = {};
    // Plain-data mirror of ProxyMetrics (no std::atomic) so this struct stays movable.
    struct {
        uint32_t sendCount = 0;
        uint32_t dropCount = 0;
        uint32_t errorCount = 0;
        uint32_t queueHighWater = 0;
        uint32_t lastResetMs = 0;
    } proxyMetrics;
    uint32_t eventBusPublishCount = 0;
    uint32_t eventBusDropCount = 0;
    uint32_t eventBusSize = 0;
    PhoneCmdDropMetricsSnapshot phoneCmdDropMetrics = {};
    const V1Settings* settings = nullptr;
    uint32_t backupRevision = 0;
    bool deferredBackupPending = false;
    bool deferredBackupRetryScheduled = false;
    uint32_t deferredBackupNextAttemptAtMs = 0;
    bool perfLoggingEnabled = false;
    const char* perfLoggingPath = "";
    uint32_t sdTryLockFails = 0;
    uint32_t sdDmaStarvation = 0;
    uint8_t connectionCycleStateCode = 0;
    uint32_t connectionCycleTimeInStateMs = 0;
    uint32_t connectionCycleTransitionsTotal = 0;
    uint32_t connectionCycleTeardownDurationMs = 0;
    uint32_t connectionCycleObdRetryAttemptsTotal = 0;
    uint32_t connectionCycleWifiManualPhoneKicksTotal = 0;
    bool connectionCycleProxyNoClientLatched = false;
};

static RuntimeSnapshotCaptureContext captureRuntimeSnapshotContext() {
    RuntimeSnapshotCaptureContext ctx{};
    ctx.nowMs = millis();
    ctx.freeHeap = ESP.getFreeHeap();
    ctx.largestHeap = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    ctx.freeDma = StorageManager::getCachedFreeDma();
    ctx.largestDma = StorageManager::getCachedLargestDma();
    ctx.freeDmaCap = heap_caps_get_free_size(MALLOC_CAP_DMA);
    ctx.largestDmaCap = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    ctx.psramTotal = static_cast<uint32_t>(ESP.getPsramSize());
    ctx.psramFree = static_cast<uint32_t>(ESP.getFreePsram());
    ctx.psramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    ctx.obdStatus = obdRuntimeModule.snapshot(ctx.nowMs);
    ctx.speedStatus = speedSourceSelector.snapshot();
    ctx.wifiAutoStart = wifiAutoStartModule.getLastDecision();
    // Field-by-field copy: ProxyMetrics contains std::atomic (non-copyable).
    {
        const auto& pm = bleClient.getProxyMetrics();
        ctx.proxyMetrics.sendCount = pm.sendCount;
        ctx.proxyMetrics.dropCount = pm.dropCount.load(std::memory_order_relaxed);
        ctx.proxyMetrics.errorCount = pm.errorCount;
        ctx.proxyMetrics.queueHighWater = pm.queueHighWater;
        ctx.proxyMetrics.lastResetMs = pm.lastResetMs;
    }
    ctx.eventBusPublishCount = systemEventBus.getPublishCount();
    ctx.eventBusDropCount = systemEventBus.getDropCount();
    ctx.eventBusSize = static_cast<uint32_t>(systemEventBus.size());
    ctx.phoneCmdDropMetrics = perfPhoneCmdDropMetricsSnapshot();
    ctx.settings = &settingsManager.get();
    ctx.backupRevision = settingsManager.backupRevision();
    ctx.deferredBackupPending = settingsManager.deferredBackupPending();
    ctx.deferredBackupRetryScheduled = settingsManager.deferredBackupRetryScheduled();
    ctx.deferredBackupNextAttemptAtMs = settingsManager.deferredBackupNextAttemptAtMs();
    ctx.perfLoggingEnabled = perfSdLogger.isEnabled();
    ctx.perfLoggingPath = perfSdLogger.csvPath();
    ctx.sdTryLockFails = StorageManager::sdTryLockFailCount.load(std::memory_order_relaxed);
    ctx.sdDmaStarvation = StorageManager::sdDmaStarvationCount.load(std::memory_order_relaxed);
    ctx.connectionCycleStateCode = sConnectionCycleStateCode.load(std::memory_order_relaxed);
    ctx.connectionCycleTimeInStateMs = sConnectionCycleTimeInStateMs.load(std::memory_order_relaxed);
    ctx.connectionCycleTransitionsTotal =
        sConnectionCycleTransitionsTotal.load(std::memory_order_relaxed);
    ctx.connectionCycleTeardownDurationMs =
        sConnectionCycleTeardownDurationMs.load(std::memory_order_relaxed);
    ctx.connectionCycleObdRetryAttemptsTotal =
        sConnectionCycleObdRetryAttemptsTotal.load(std::memory_order_relaxed);
    ctx.connectionCycleWifiManualPhoneKicksTotal =
        sConnectionCycleWifiManualPhoneKicksTotal.load(std::memory_order_relaxed);
    ctx.connectionCycleProxyNoClientLatched =
        sConnectionCycleProxyNoClientLatched.load(std::memory_order_relaxed) != 0;
    return ctx;
}

struct PerfExtendedSnapshot {
    PerfExtendedMetrics metrics;
    uint32_t dmaFreeMin = 0;
    uint32_t dmaLargestMin = 0;
};

static void resetPerfExtendedWindowPeaks() {
    perfExtended.notifyToDisplayMs.reset();
    perfExtended.loopMaxUs = 0;
    perfExtended.bleDrainMaxUs = 0;
    perfExtended.displayRenderMaxUs = 0;
    perfExtended.dispPipeMaxUs = 0;
    perfExtended.bleProcessMaxUs = 0;
    perfExtended.bleFollowupRequestAlertMaxUs = 0;
    perfExtended.bleFollowupRequestVersionMaxUs = 0;
    perfExtended.bleConnectStableCallbackMaxUs = 0;
    perfExtended.bleProxyStartMaxUs = 0;
    perfExtended.displayVoiceMaxUs = 0;
    perfExtended.displayGapRecoverMaxUs = 0;
    perfExtended.displayPartialFlushAreaPeakPx = 0;
    perfExtended.displayFlushMaxAreaPx = 0;
    perfExtended.displayPartialFlushLogicalWidthPeakPx = 0;
    perfExtended.displayPartialFlushLogicalHeightPeakPx = 0;
    perfExtended.displayPartialFlushRowCallsPeak = 0;
    perfExtended.displayPartialFlushPixelsPerRowPeakPx = 0;
    perfExtended.displayPartialFlushUsPeak = 0;
    perfExtended.displayPartialFlushWorstUsLogicalWidthPx = 0;
    perfExtended.displayPartialFlushWorstUsLogicalHeightPx = 0;
    perfExtended.displayPartialFlushWorstUsAreaPx = 0;
    perfExtended.displayUnionExceedsCapAreaPeakPx = 0;
    perfExtended.displayUnionExceedsCapRectCountPeak = 0;
    perfExtended.displayUnionExceedsCapAreaPeakSourceMask = 0;
    perfExtended.displayBaseFrameMaxUs = 0;
    perfExtended.displayStatusStripMaxUs = 0;
    perfExtended.displayFrequencyMaxUs = 0;
    perfExtended.displayBandsBarsMaxUs = 0;
    perfExtended.displayArrowsIconsMaxUs = 0;
    perfExtended.displayFlushSubphaseMaxUs = 0;
    perfExtended.displayLiveRenderMaxUs = 0;
    perfExtended.displayRestingRenderMaxUs = 0;
    perfExtended.displayPersistedRenderMaxUs = 0;
    perfExtended.displayPreviewRenderMaxUs = 0;
    perfExtended.displayRestoreRenderMaxUs = 0;
    perfExtended.displayPreviewFirstRenderMaxUs = 0;
    perfExtended.displayPreviewSteadyRenderMaxUs = 0;
    perfExtended.touchMaxUs = 0;
    perfExtended.obdMaxUs = 0;
    perfExtended.obdConnectCallMaxUs = 0;
    perfExtended.obdSecurityStartCallMaxUs = 0;
    perfExtended.obdDiscoveryCallMaxUs = 0;
    perfExtended.obdSubscribeCallMaxUs = 0;
    perfExtended.obdWriteCallMaxUs = 0;
    perfExtended.obdRssiCallMaxUs = 0;
    perfExtended.wifiMaxUs = 0;
    perfExtended.wifiHandleClientMaxUs = 0;
    perfExtended.wifiMaintenanceMaxUs = 0;
    perfExtended.wifiStatusCheckMaxUs = 0;
    perfExtended.wifiTimeoutCheckMaxUs = 0;
    perfExtended.wifiHeapGuardMaxUs = 0;
    perfExtended.wifiApStaPollMaxUs = 0;
    perfExtended.wifiStopHttpServerMaxUs = 0;
    perfExtended.wifiStopStaDisconnectMaxUs = 0;
    perfExtended.wifiStopApDisableMaxUs = 0;
    perfExtended.wifiStopModeOffMaxUs = 0;
    perfExtended.wifiStartPreflightMaxUs = 0;
    perfExtended.wifiStartApBringupMaxUs = 0;
    perfExtended.fsMaxUs = 0;
    perfExtended.sdMaxUs = 0;
    perfExtended.flushMaxUs = 0;
    perfExtended.bleConnectMaxUs = 0;
    perfExtended.bleDiscoveryMaxUs = 0;
    perfExtended.bleSubscribeMaxUs = 0;
    perfExtended.perfReportMaxUs = 0;
    perfExtended.minLargestBlock = UINT32_MAX;
}

static void capturePerfExtendedSnapshot(PerfExtendedSnapshot& snapshot,
                                        const RuntimeSnapshotCaptureContext& ctx,
                                        PerfRuntimeSnapshotMode mode) {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    if (mode == PerfRuntimeSnapshotMode::CaptureAndResetWindowPeaks &&
        ctx.freeDmaCap < sDmaFreeCapMin) {
        sDmaFreeCapMin = ctx.freeDmaCap;
    }
    if (mode == PerfRuntimeSnapshotMode::CaptureAndResetWindowPeaks &&
        ctx.largestDmaCap < sDmaLargestCapMin) {
        sDmaLargestCapMin = ctx.largestDmaCap;
    }
    snapshot.dmaFreeMin = (sDmaFreeCapMin == UINT32_MAX) ? ctx.freeDmaCap : sDmaFreeCapMin;
    snapshot.dmaLargestMin =
        (sDmaLargestCapMin == UINT32_MAX) ? ctx.largestDmaCap : sDmaLargestCapMin;
    snapshot.metrics = perfExtended;

    if (mode == PerfRuntimeSnapshotMode::CaptureAndResetWindowPeaks) {
        sPrevWindowLoopMaxUs.store(snapshot.metrics.loopMaxUs, std::memory_order_relaxed);
        sPrevWindowWifiMaxUs.store(snapshot.metrics.wifiMaxUs, std::memory_order_relaxed);
        sPrevWindowBleProcessMaxUs.store(snapshot.metrics.bleProcessMaxUs,
                                         std::memory_order_relaxed);
        sPrevWindowDispPipeMaxUs.store(snapshot.metrics.dispPipeMaxUs, std::memory_order_relaxed);
        resetPerfExtendedWindowPeaks();
    }
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

static void populateFlatSnapshot(PerfSdSnapshot& flat,
                                 const RuntimeSnapshotCaptureContext& ctx,
                                 PerfRuntimeSnapshotMode mode) {
    flat = {};
    flat.millisTs = ctx.nowMs;
    flat.freeHeap = ctx.freeHeap;
    flat.freeDma = ctx.freeDma;
    flat.largestDma = ctx.largestDma;
    flat.freeDmaCap = ctx.freeDmaCap;
    flat.largestDmaCap = ctx.largestDmaCap;

    flat.rx = perfCounters.rxPackets.load(std::memory_order_relaxed);
    flat.qDrop = perfCounters.queueDrops.load(std::memory_order_relaxed);
    flat.perfDrop = perfCounters.perfDrop.load(std::memory_order_relaxed);
    flat.eventBusDrops = ctx.eventBusDropCount;
    flat.parseOk = perfCounters.parseSuccesses.load(std::memory_order_relaxed);
    flat.parseFail = perfCounters.parseFailures.load(std::memory_order_relaxed);
    flat.parseResync = perfCounters.parseResyncs.load(std::memory_order_relaxed);
    flat.disc = perfCounters.disconnects.load(std::memory_order_relaxed);
    flat.reconn = perfCounters.reconnects.load(std::memory_order_relaxed);

    flat.alertPersistStarts = perfCounters.alertPersistStarts.load(std::memory_order_relaxed);
    flat.alertPersistStartsSkippedActive =
        perfCounters.alertPersistStartsSkippedActive.load(std::memory_order_relaxed);
    flat.alertPersistStartsSkippedInvalid =
        perfCounters.alertPersistStartsSkippedInvalid.load(std::memory_order_relaxed);
    flat.alertPersistExpires = perfCounters.alertPersistExpires.load(std::memory_order_relaxed);
    flat.alertPersistClears = perfCounters.alertPersistClears.load(std::memory_order_relaxed);
    flat.autoPushStarts = perfCounters.autoPushStarts.load(std::memory_order_relaxed);
    flat.autoPushCompletes = perfCounters.autoPushCompletes.load(std::memory_order_relaxed);
    flat.autoPushNoProfile = perfCounters.autoPushNoProfile.load(std::memory_order_relaxed);
    flat.autoPushProfileLoadFail = perfCounters.autoPushProfileLoadFail.load(std::memory_order_relaxed);
    flat.autoPushProfileWriteFail = perfCounters.autoPushProfileWriteFail.load(std::memory_order_relaxed);
    flat.autoPushBusyRetries = perfCounters.autoPushBusyRetries.load(std::memory_order_relaxed);
    flat.autoPushModeFail = perfCounters.autoPushModeFail.load(std::memory_order_relaxed);
    flat.autoPushVolumeFail = perfCounters.autoPushVolumeFail.load(std::memory_order_relaxed);
    flat.autoPushDisconnectAbort = perfCounters.autoPushDisconnectAbort.load(std::memory_order_relaxed);
    flat.prioritySelectRowFlag = perfCounters.prioritySelectRowFlag.load(std::memory_order_relaxed);
    flat.prioritySelectFirstUsable = perfCounters.prioritySelectFirstUsable.load(std::memory_order_relaxed);
    flat.prioritySelectFirstEntry = perfCounters.prioritySelectFirstEntry.load(std::memory_order_relaxed);
    flat.prioritySelectAmbiguousIndex = perfCounters.prioritySelectAmbiguousIndex.load(std::memory_order_relaxed);
    flat.prioritySelectUnusableIndex = perfCounters.prioritySelectUnusableIndex.load(std::memory_order_relaxed);
    flat.prioritySelectInvalidChosen = perfCounters.prioritySelectInvalidChosen.load(std::memory_order_relaxed);
    flat.alertTablePublishes = perfCounters.alertTablePublishes.load(std::memory_order_relaxed);
    flat.alertTablePublishes3Bogey = perfCounters.alertTablePublishes3Bogey.load(std::memory_order_relaxed);
    flat.alertTableRowReplacements = perfCounters.alertTableRowReplacements.load(std::memory_order_relaxed);
    flat.alertTableAssemblyTimeouts = perfCounters.alertTableAssemblyTimeouts.load(std::memory_order_relaxed);
    flat.parserRowsBandNone = perfCounters.parserRowsBandNone.load(std::memory_order_relaxed);
    flat.parserRowsKuRaw = perfCounters.parserRowsKuRaw.load(std::memory_order_relaxed);
    flat.displayLiveInvalidPrioritySkips = perfCounters.displayLiveInvalidPrioritySkips.load(std::memory_order_relaxed);
    flat.displayLiveFallbackToUsable = perfCounters.displayLiveFallbackToUsable.load(std::memory_order_relaxed);
    flat.powerAutoPowerArmed = perfCounters.powerAutoPowerArmed.load(std::memory_order_relaxed);
    flat.powerAutoPowerTimerStart = perfCounters.powerAutoPowerTimerStart.load(std::memory_order_relaxed);
    flat.powerAutoPowerTimerCancel = perfCounters.powerAutoPowerTimerCancel.load(std::memory_order_relaxed);
    flat.powerAutoPowerTimerExpire = perfCounters.powerAutoPowerTimerExpire.load(std::memory_order_relaxed);
    flat.powerCarModeAlpSilenceExpire = perfCounters.powerCarModeAlpSilenceExpire.load(std::memory_order_relaxed);
    flat.powerCriticalWarn = perfCounters.powerCriticalWarn.load(std::memory_order_relaxed);
    flat.powerCriticalShutdown = perfCounters.powerCriticalShutdown.load(std::memory_order_relaxed);
    flat.perfUncleanShutdown = perfCounters.perfUncleanShutdown.load(std::memory_order_relaxed);
    flat.cmdBleBusy = perfCounters.cmdBleBusy.load(std::memory_order_relaxed);
    flat.rxBytes = perfCounters.rxBytes.load(std::memory_order_relaxed);
    flat.oversizeDrops = perfCounters.oversizeDrops.load(std::memory_order_relaxed);
    flat.queueHighWater = perfCounters.queueHighWater.load(std::memory_order_relaxed);
    flat.bleMutexSkip = perfCounters.bleMutexSkip.load(std::memory_order_relaxed);
    flat.bleMutexTimeout = perfCounters.bleMutexTimeout.load(std::memory_order_relaxed);
    flat.cmdPaceNotYet = perfCounters.cmdPaceNotYet.load(std::memory_order_relaxed);
    flat.bleDiscTaskCreateFail = perfCounters.bleDiscTaskCreateFail.load(std::memory_order_relaxed);
    flat.displayUpdates = perfCounters.displayUpdates.load(std::memory_order_relaxed);
    flat.displaySkips = perfCounters.displaySkips.load(std::memory_order_relaxed);
    flat.wifiConnectDeferred = perfCounters.wifiConnectDeferred.load(std::memory_order_relaxed);
    flat.pushNowRetries = perfCounters.pushNowRetries.load(std::memory_order_relaxed);
    flat.pushNowFailures = perfCounters.pushNowFailures.load(std::memory_order_relaxed);
    PerfExtendedSnapshot extended{};
    capturePerfExtendedSnapshot(extended, ctx, mode);
    const PerfExtendedMetrics& metrics = extended.metrics;

    flat.freeDmaMin = (metrics.minFreeDma == UINT32_MAX) ? ctx.freeDma : metrics.minFreeDma;
    flat.largestDmaMin =
        (metrics.minLargestDma == UINT32_MAX) ? ctx.largestDma : metrics.minLargestDma;
    flat.bleState = bleClient.getBLEStateCode();
    flat.subscribeStep = bleClient.getSubscribeStepCode();
    flat.connectInProgress = bleClient.isConnectInProgress() ? 1 : 0;
    flat.asyncConnectPending = bleClient.isAsyncConnectPending() ? 1 : 0;
    flat.pendingDisconnectCleanup = bleClient.hasPendingDisconnectCleanup() ? 1 : 0;
    flat.proxyAdvertising = perfGetProxyAdvertisingState() != 0 ? 1 : 0;
    flat.proxyAdvertisingLastTransitionReason =
        static_cast<uint8_t>(perfGetProxyAdvertisingLastTransitionReason());
    flat.wifiPriorityMode = bleClient.isWifiPriority() ? 1 : 0;

    flat.dmaFreeMin = extended.dmaFreeMin;
    flat.dmaLargestMin = extended.dmaLargestMin;

    flat.loopMaxUs = metrics.loopMaxUs;
    flat.notifyToDisplayMaxMs = metrics.notifyToDisplayMs.maxMs;
    flat.notifyToDisplayTotalCount = metrics.notifyToDisplayMs.total;
    flat.bleDrainMaxUs = metrics.bleDrainMaxUs;
    flat.dispMaxUs = metrics.displayRenderMaxUs;
    flat.bleProcessMaxUs = metrics.bleProcessMaxUs;
    flat.touchMaxUs = metrics.touchMaxUs;
    flat.obdMaxUs = metrics.obdMaxUs;
    flat.obdConnectCallMaxUs = metrics.obdConnectCallMaxUs;
    flat.obdSecurityStartCallMaxUs = metrics.obdSecurityStartCallMaxUs;
    flat.obdDiscoveryCallMaxUs = metrics.obdDiscoveryCallMaxUs;
    flat.obdSubscribeCallMaxUs = metrics.obdSubscribeCallMaxUs;
    flat.obdWriteCallMaxUs = metrics.obdWriteCallMaxUs;
    flat.obdRssiCallMaxUs = metrics.obdRssiCallMaxUs;
    flat.obdPollErrors = ctx.obdStatus.pollErrors;
    flat.obdStaleCount = ctx.obdStatus.staleSpeedCount;
    const bool speedSelectedValid = ctx.speedStatus.selectedSource != SpeedSource::NONE;
    const float selectedSpeedMph = speedSelectedValid ? ctx.speedStatus.selectedSpeedMph : 0.0f;
    flat.speedSourceSelected = static_cast<uint8_t>(ctx.speedStatus.selectedSource);
    flat.speedSourceValid = speedSelectedValid ? 1 : 0;
    flat.speedSelectedMph_x10 =
        (speedSelectedValid && std::isfinite(selectedSpeedMph) && selectedSpeedMph > 0.0f)
            ? static_cast<uint32_t>(std::lround(selectedSpeedMph * 10.0f))
            : 0;
    flat.speedSelectedAgeMs = speedSelectedValid ? ctx.speedStatus.selectedAgeMs : UINT32_MAX;
    flat.speedSourceSwitches = ctx.speedStatus.sourceSwitches;
    flat.speedNoSourceSelections = ctx.speedStatus.noSourceSelections;
    flat.speedGpsSelections = ctx.speedStatus.gpsSelections;
    flat.cycleState = ctx.connectionCycleStateCode;
    flat.cycleTransitionsTotal = ctx.connectionCycleTransitionsTotal;
    flat.cycleTimeInStateMs = ctx.connectionCycleTimeInStateMs;
    flat.cycleTeardownDurationMs = ctx.connectionCycleTeardownDurationMs;
    flat.cycleObdRetryAttemptsTotal = ctx.connectionCycleObdRetryAttemptsTotal;
    flat.cycleWifiManualPhoneKicksTotal = ctx.connectionCycleWifiManualPhoneKicksTotal;
    flat.cycleProxyNoClientLatched = ctx.connectionCycleProxyNoClientLatched ? 1 : 0;

    // GPS observability (schema v37)
    {
        const uint32_t nowMs = static_cast<uint32_t>(millis());
        flat.utcValid = gpsTimePublisher.readUtc(nowMs, flat.utcEpochMs);
        if (!flat.utcValid) {
            flat.utcEpochMs = 0;
        }
        const GpsRuntimeStatus gpsStatus = gpsRuntimeModule.snapshot(nowMs);
        flat.gpsSentencesOk           = gpsStatus.sentencesParsed;
        flat.gpsSentencesChecksumFail = gpsStatus.checksumFailures;
        flat.gpsSentencesUnknown      = gpsStatus.sentencesUnknown;
        flat.gpsBufferOverruns        = gpsStatus.bufferOverruns;
        flat.gpsBytesIn               = gpsStatus.bytesRead;
        flat.gpsFirstFixMs            = gpsStatus.firstFixMs;
        flat.gpsLastSentenceAgeMs     = (gpsStatus.lastSentenceTsMs != 0)
                                            ? (nowMs - gpsStatus.lastSentenceTsMs)
                                            : UINT32_MAX;
        flat.gpsFixAgeMs              = gpsStatus.fixAgeMs;
        flat.gpsStableFixAgeMs        = gpsStatus.stableFixAgeMs;
        flat.gpsSatellitesInUse       = gpsStatus.satellites;
        flat.gpsHdopX10               = std::isnan(gpsStatus.hdop)
                                            ? UINT16_MAX
                                            : static_cast<uint16_t>(gpsStatus.hdop * 10.0f + 0.5f);
        flat.gpsHasFix                = gpsStatus.hasFix;
        flat.gpsStableHasFix          = gpsStatus.stableHasFix;
        flat.gpsEnableTransitions     = gpsStatus.enableTransitions;
    }
    flat.wifiMaxUs = metrics.wifiMaxUs;
    flat.wifiHandleClientMaxUs = metrics.wifiHandleClientMaxUs;
    flat.wifiMaintenanceMaxUs = metrics.wifiMaintenanceMaxUs;
    flat.wifiStatusCheckMaxUs = metrics.wifiStatusCheckMaxUs;
    flat.wifiTimeoutCheckMaxUs = metrics.wifiTimeoutCheckMaxUs;
    flat.wifiHeapGuardMaxUs = metrics.wifiHeapGuardMaxUs;
    flat.wifiApStaPollMaxUs = metrics.wifiApStaPollMaxUs;
    flat.wifiStopHttpServerMaxUs = metrics.wifiStopHttpServerMaxUs;
    flat.wifiStopStaDisconnectMaxUs = metrics.wifiStopStaDisconnectMaxUs;
    flat.wifiStopApDisableMaxUs = metrics.wifiStopApDisableMaxUs;
    flat.wifiStopModeOffMaxUs = metrics.wifiStopModeOffMaxUs;
    flat.wifiStartPreflightMaxUs = metrics.wifiStartPreflightMaxUs;
    flat.wifiStartApBringupMaxUs = metrics.wifiStartApBringupMaxUs;
    flat.fsMaxUs = metrics.fsMaxUs;
    flat.sdMaxUs = metrics.sdMaxUs;
    flat.sdWriteCount = metrics.sdWriteCount;
    flat.sdWriteLt1msCount = metrics.sdWriteLt1msCount;
    flat.sdWrite1to5msCount = metrics.sdWrite1to5msCount;
    flat.sdWrite5to10msCount = metrics.sdWrite5to10msCount;
    flat.sdWriteGe10msCount = metrics.sdWriteGe10msCount;
    flat.flushMaxUs = metrics.flushMaxUs;
    flat.bleConnectMaxUs = metrics.bleConnectMaxUs;
    flat.bleDiscoveryMaxUs = metrics.bleDiscoveryMaxUs;
    flat.bleSubscribeMaxUs = metrics.bleSubscribeMaxUs;
    flat.dispPipeMaxUs = metrics.dispPipeMaxUs;
    flat.perfReportMaxUs = metrics.perfReportMaxUs;
    flat.minLargestBlock =
        (metrics.minLargestBlock == UINT32_MAX) ? 0 : metrics.minLargestBlock;

    flat.uiToScanCount = metrics.uiToScanCount;
    flat.uiToRestCount = metrics.uiToRestCount;
    flat.uiScanToRestCount = metrics.uiScanToRestCount;
    flat.uiFastScanExitCount = metrics.uiFastScanExitCount;
    flat.uiLastScanDwellMs = metrics.uiLastScanDwellMs;
    flat.uiMinScanDwellMs =
        (metrics.uiMinScanDwellMs == UINT32_MAX) ? 0 : metrics.uiMinScanDwellMs;
    flat.fadeDownCount = metrics.fadeDownCount;
    flat.fadeRestoreCount = metrics.fadeRestoreCount;
    flat.fadeSkipEqualCount = metrics.fadeSkipEqualCount;
    flat.fadeSkipNoBaselineCount = metrics.fadeSkipNoBaselineCount;
    flat.fadeSkipNotFadedCount = metrics.fadeSkipNotFadedCount;
    flat.fadeLastDecision = metrics.fadeLastDecision;
    flat.fadeLastCurrentVol = metrics.fadeLastCurrentVol;
    flat.fadeLastOriginalVol = metrics.fadeLastOriginalVol;
    flat.fadeLastDecisionMs = metrics.fadeLastDecisionMs;
    flat.speedVolDropCount = metrics.speedVolDropCount;
    flat.speedVolRestoreCount = metrics.speedVolRestoreCount;
    flat.speedVolRetryCount = metrics.speedVolRetryCount;
    flat.bleScanStartMs = metrics.bleScanStartMs;
    flat.bleTargetFoundMs = metrics.bleTargetFoundMs;
    flat.bleConnectStartMs = metrics.bleConnectStartMs;
    flat.bleConnectedMs = metrics.bleConnectedMs;
    flat.bleFirstRxMs = metrics.bleFirstRxMs;
    flat.bleFollowupRequestAlertMaxUs = metrics.bleFollowupRequestAlertMaxUs;
    flat.bleFollowupRequestVersionMaxUs = metrics.bleFollowupRequestVersionMaxUs;
    flat.bleConnectStableCallbackMaxUs = metrics.bleConnectStableCallbackMaxUs;
    flat.bleProxyStartMaxUs = metrics.bleProxyStartMaxUs;
    flat.displayGapRecoverMaxUs = metrics.displayGapRecoverMaxUs;
    flat.displayFullRenderCount = metrics.displayFullRenderCount;
    flat.displayRestingFullRenderCount = metrics.displayRestingFullRenderCount;
    flat.displayRestingIncrementalRenderCount = metrics.displayRestingIncrementalRenderCount;
    flat.displayPersistedRenderCount = metrics.displayPersistedRenderCount;
    flat.displayPreviewRenderCount = metrics.displayPreviewRenderCount;
    flat.displayRestoreRenderCount = metrics.displayRestoreRenderCount;
    flat.displayLiveScenarioRenderCount = metrics.displayLiveScenarioRenderCount;
    flat.displayRestingScenarioRenderCount = metrics.displayRestingScenarioRenderCount;
    flat.displayPersistedScenarioRenderCount = metrics.displayPersistedScenarioRenderCount;
    flat.displayPreviewScenarioRenderCount = metrics.displayPreviewScenarioRenderCount;
    flat.displayRestoreScenarioRenderCount = metrics.displayRestoreScenarioRenderCount;
    flat.displayRestingFlushReasonFullRedrawCount = metrics.displayRestingFlushReasonFullRedrawCount;
    flat.displayRestingFlushReasonPendingExternalCount = metrics.displayRestingFlushReasonPendingExternalCount;
    flat.displayRestingFlushReasonPaintedCount = metrics.displayRestingFlushReasonPaintedCount;
    flat.displayRestingFlushReasonCacheHitCount = metrics.displayRestingFlushReasonCacheHitCount;
    flat.displayPersistedFlushReasonFullRedrawCount = metrics.displayPersistedFlushReasonFullRedrawCount;
    flat.displayPersistedFlushReasonPendingExternalCount = metrics.displayPersistedFlushReasonPendingExternalCount;
    flat.displayPersistedFlushReasonPaintedCount = metrics.displayPersistedFlushReasonPaintedCount;
    flat.displayPersistedFlushReasonCacheHitCount = metrics.displayPersistedFlushReasonCacheHitCount;
    flat.displayStatusVolumePaintCount = metrics.displayStatusVolumePaintCount;
    flat.displayStatusRssiPaintCount = metrics.displayStatusRssiPaintCount;
    flat.displayStatusProfilePaintCount = metrics.displayStatusProfilePaintCount;
    flat.displayStatusBatteryPaintCount = metrics.displayStatusBatteryPaintCount;
    flat.displayStatusBleProxyPaintCount = metrics.displayStatusBleProxyPaintCount;
    flat.displayStatusWifiPaintCount = metrics.displayStatusWifiPaintCount;
    flat.displayStatusObdPaintCount = metrics.displayStatusObdPaintCount;
    flat.displayStatusGpsPaintCount = metrics.displayStatusGpsPaintCount;
    flat.displayStatusAlpPaintCount = metrics.displayStatusAlpPaintCount;
    flat.displayRedrawReasonFirstRunCount = metrics.displayRedrawReasonFirstRunCount;
    flat.displayRedrawReasonEnterLiveCount = metrics.displayRedrawReasonEnterLiveCount;
    flat.displayRedrawReasonLeaveLiveCount = metrics.displayRedrawReasonLeaveLiveCount;
    flat.displayRedrawReasonLeavePersistedCount = metrics.displayRedrawReasonLeavePersistedCount;
    flat.displayRedrawReasonForceRedrawCount = metrics.displayRedrawReasonForceRedrawCount;
    flat.displayRedrawReasonFrequencyChangeCount = metrics.displayRedrawReasonFrequencyChangeCount;
    flat.displayRedrawReasonBandSetChangeCount = metrics.displayRedrawReasonBandSetChangeCount;
    flat.displayRedrawReasonArrowChangeCount = metrics.displayRedrawReasonArrowChangeCount;
    flat.displayRedrawReasonSignalBarChangeCount = metrics.displayRedrawReasonSignalBarChangeCount;
    flat.displayRedrawReasonVolumeChangeCount = metrics.displayRedrawReasonVolumeChangeCount;
    flat.displayRedrawReasonBogeyCounterChangeCount = metrics.displayRedrawReasonBogeyCounterChangeCount;
    flat.displayRedrawReasonRssiRefreshCount = metrics.displayRedrawReasonRssiRefreshCount;
    flat.displayRedrawReasonFlashTickCount = metrics.displayRedrawReasonFlashTickCount;
    flat.displayRedrawReasonFullFlushForRedrawCount = metrics.displayRedrawReasonFullFlushForRedrawCount;
    flat.displayRedrawReasonCacheHitSkipFlushCount = metrics.displayRedrawReasonCacheHitSkipFlushCount;
    flat.displayRedrawReasonUnionExceedsCapCount = metrics.displayRedrawReasonUnionExceedsCapCount;
    flat.displayRedrawReasonPartialRegionFlushCount = metrics.displayRedrawReasonPartialRegionFlushCount;
    flat.displayFullFlushCount = metrics.displayFullFlushCount;
    flat.displayPartialFlushCount = metrics.displayPartialFlushCount;
    flat.displayPartialFlushAreaPeakPx = metrics.displayPartialFlushAreaPeakPx;
    flat.displayPartialFlushAreaTotalPx = metrics.displayPartialFlushAreaTotalPx;
    flat.displayFlushEquivalentAreaTotalPx = metrics.displayFlushEquivalentAreaTotalPx;
    flat.displayFlushMaxAreaPx = metrics.displayFlushMaxAreaPx;
    flat.displayPartialFlushLogicalWidthPeakPx = metrics.displayPartialFlushLogicalWidthPeakPx;
    flat.displayPartialFlushLogicalHeightPeakPx = metrics.displayPartialFlushLogicalHeightPeakPx;
    flat.displayPartialFlushRowCallsPeak = metrics.displayPartialFlushRowCallsPeak;
    flat.displayPartialFlushPixelsPerRowPeakPx = metrics.displayPartialFlushPixelsPerRowPeakPx;
    flat.displayPartialFlushUsPeak = metrics.displayPartialFlushUsPeak;
    flat.displayPartialFlushWorstUsLogicalWidthPx = metrics.displayPartialFlushWorstUsLogicalWidthPx;
    flat.displayPartialFlushWorstUsLogicalHeightPx = metrics.displayPartialFlushWorstUsLogicalHeightPx;
    flat.displayPartialFlushWorstUsAreaPx = metrics.displayPartialFlushWorstUsAreaPx;
    flat.displayPartialFlushWouldFullRows64Count = metrics.displayPartialFlushWouldFullRows64Count;
    flat.displayPartialFlushWouldFullRows128Count = metrics.displayPartialFlushWouldFullRows128Count;
    flat.displayPartialFlushWouldFullRows256Count = metrics.displayPartialFlushWouldFullRows256Count;
    flat.displayUnionExceedsCapAreaPeakPx = metrics.displayUnionExceedsCapAreaPeakPx;
    flat.displayUnionExceedsCapRectCountPeak = metrics.displayUnionExceedsCapRectCountPeak;
    flat.displayUnionExceedsCapAreaPeakSourceMask = metrics.displayUnionExceedsCapAreaPeakSourceMask;
    flat.displayUnionExceedsCapWithFrequencyCount = metrics.displayUnionExceedsCapWithFrequencyCount;
    flat.displayUnionExceedsCapWithBandsBarsCount = metrics.displayUnionExceedsCapWithBandsBarsCount;
    flat.displayUnionExceedsCapWithArrowsCount = metrics.displayUnionExceedsCapWithArrowsCount;
    flat.displayUnionExceedsCapWithStatusCount = metrics.displayUnionExceedsCapWithStatusCount;
    flat.displayUnionExceedsCapWithIndicatorsCount = metrics.displayUnionExceedsCapWithIndicatorsCount;
    flat.displayUnionExceedsCapWithExternalCount = metrics.displayUnionExceedsCapWithExternalCount;
    flat.displayUnionExceedsCapUnclassifiedCount = metrics.displayUnionExceedsCapUnclassifiedCount;
    flat.displayBaseFrameMaxUs = metrics.displayBaseFrameMaxUs;
    flat.displayStatusStripMaxUs = metrics.displayStatusStripMaxUs;
    flat.displayFrequencyMaxUs = metrics.displayFrequencyMaxUs;
    flat.displayBandsBarsMaxUs = metrics.displayBandsBarsMaxUs;
    flat.displayArrowsIconsMaxUs = metrics.displayArrowsIconsMaxUs;
    flat.displayFlushSubphaseMaxUs = metrics.displayFlushSubphaseMaxUs;
    flat.displayLiveRenderMaxUs = metrics.displayLiveRenderMaxUs;
    flat.displayRestingRenderMaxUs = metrics.displayRestingRenderMaxUs;
    flat.displayPersistedRenderMaxUs = metrics.displayPersistedRenderMaxUs;
    flat.displayPreviewRenderMaxUs = metrics.displayPreviewRenderMaxUs;
    flat.displayRestoreRenderMaxUs = metrics.displayRestoreRenderMaxUs;
    flat.displayPreviewFirstRenderMaxUs = metrics.displayPreviewFirstRenderMaxUs;
    flat.displayPreviewSteadyRenderMaxUs = metrics.displayPreviewSteadyRenderMaxUs;
}

static void populateRuntimeSnapshot(PerfRuntimeMetricsSnapshot& snapshot,
                                    const RuntimeSnapshotCaptureContext& ctx,
                                    PerfRuntimeSnapshotMode mode) {
    snapshot = {};
    populateFlatSnapshot(snapshot.flat, ctx, mode);

    snapshot.phoneCmdDrops = ctx.phoneCmdDropMetrics;
    snapshot.uptimeMs = ctx.nowMs;
    snapshot.connectionDispatchRuns = perfCounters.connectionDispatchRuns.load(std::memory_order_relaxed);
    snapshot.connectionCadenceDisplayDue =
        perfCounters.connectionCadenceDisplayDue.load(std::memory_order_relaxed);
    snapshot.connectionCadenceHoldScanDwell =
        perfCounters.connectionCadenceHoldScanDwell.load(std::memory_order_relaxed);
    snapshot.connectionStateProcessRuns =
        perfCounters.connectionStateProcessRuns.load(std::memory_order_relaxed);
    snapshot.connectionStateWatchdogForces =
        perfCounters.connectionStateWatchdogForces.load(std::memory_order_relaxed);
    snapshot.connectionStateProcessGapMaxMs =
        perfCounters.connectionStateProcessGapMaxMs.load(std::memory_order_relaxed);
    snapshot.bleScanStateEntries = perfCounters.bleScanStateEntries.load(std::memory_order_relaxed);
    snapshot.bleScanStateExits = perfCounters.bleScanStateExits.load(std::memory_order_relaxed);
    snapshot.bleScanTargetFound = perfCounters.bleScanTargetFound.load(std::memory_order_relaxed);
    snapshot.bleScanNoTargetExits =
        perfCounters.bleScanNoTargetExits.load(std::memory_order_relaxed);
    snapshot.bleScanDwellMaxMs = perfCounters.bleScanDwellMaxMs.load(std::memory_order_relaxed);
    snapshot.uuid128FallbackHits = perfCounters.uuid128FallbackHits.load(std::memory_order_relaxed);
    snapshot.wifiStopGraceful = perfCounters.wifiStopGraceful.load(std::memory_order_relaxed);
    snapshot.wifiStopImmediate = perfCounters.wifiStopImmediate.load(std::memory_order_relaxed);
    snapshot.wifiStopManual = perfCounters.wifiStopManual.load(std::memory_order_relaxed);
    snapshot.wifiStopTimeout = perfCounters.wifiStopTimeout.load(std::memory_order_relaxed);
    snapshot.wifiStopNoClients = perfCounters.wifiStopNoClients.load(std::memory_order_relaxed);
    snapshot.wifiStopNoClientsAuto =
        perfCounters.wifiStopNoClientsAuto.load(std::memory_order_relaxed);
    snapshot.wifiStopLowDma = perfCounters.wifiStopLowDma.load(std::memory_order_relaxed);
    snapshot.wifiStopPoweroff = perfCounters.wifiStopPoweroff.load(std::memory_order_relaxed);
    snapshot.wifiStopOther = perfCounters.wifiStopOther.load(std::memory_order_relaxed);
    snapshot.wifiApDropLowDma = perfCounters.wifiApDropLowDma.load(std::memory_order_relaxed);
    snapshot.wifiApDropIdleSta = perfCounters.wifiApDropIdleSta.load(std::memory_order_relaxed);
    snapshot.wifiApUpTransitions = perfCounters.wifiApUpTransitions.load(std::memory_order_relaxed);
    snapshot.wifiApDownTransitions =
        perfCounters.wifiApDownTransitions.load(std::memory_order_relaxed);
    snapshot.wifiProcessMaxUs = perfCounters.wifiProcessMaxUs.load(std::memory_order_relaxed);
    const BLEState bleState = bleClient.getBLEState();
    snapshot.bleState = bleStateToString(bleState);
    snapshot.bleStateCode = snapshot.flat.bleState;
    snapshot.subscribeStep = bleClient.getSubscribeStepName();
    snapshot.subscribeStepCode = snapshot.flat.subscribeStep;
    snapshot.connectInProgress = snapshot.flat.connectInProgress != 0;
    snapshot.asyncConnectPending = snapshot.flat.asyncConnectPending != 0;
    snapshot.pendingDisconnectCleanup = snapshot.flat.pendingDisconnectCleanup != 0;
    snapshot.proxyAdvertising = snapshot.flat.proxyAdvertising != 0;
    snapshot.proxyAdvertisingOnTransitions = perfCounters.proxyAdvertisingOnTransitions.load(std::memory_order_relaxed);
    snapshot.proxyAdvertisingOffTransitions = perfCounters.proxyAdvertisingOffTransitions.load(std::memory_order_relaxed);
    snapshot.proxyAdvertisingLastTransitionMs = perfGetProxyAdvertisingLastTransitionMs();
    snapshot.proxyAdvertisingLastTransitionReasonCode = perfGetProxyAdvertisingLastTransitionReason();
    snapshot.proxyAdvertisingLastTransitionReason =
        perfProxyAdvertisingTransitionReasonName(snapshot.proxyAdvertisingLastTransitionReasonCode);
    snapshot.wifiPriorityMode = snapshot.flat.wifiPriorityMode != 0;
    snapshot.loopMaxPrevWindowUs = perfGetPrevWindowLoopMaxUs();
    snapshot.wifiMaxPrevWindowUs = perfGetPrevWindowWifiMaxUs();
    snapshot.bleProcessMaxPrevWindowUs = perfGetPrevWindowBleProcessMaxUs();
    snapshot.dispPipeMaxPrevWindowUs = perfGetPrevWindowDispPipeMaxUs();
    snapshot.wifiApActive = perfGetWifiApState();
    snapshot.wifiApLastTransitionMs = perfGetWifiApLastTransitionMs();
    snapshot.wifiApLastTransitionReasonCode = perfGetWifiApLastTransitionReason();
    snapshot.wifiApLastTransitionReason =
        perfWifiApTransitionReasonName(snapshot.wifiApLastTransitionReasonCode);
    snapshot.perfSdLockFail = perfCounters.perfSdLockFail.load(std::memory_order_relaxed);
    snapshot.perfSdDirFail = perfCounters.perfSdDirFail.load(std::memory_order_relaxed);
    snapshot.perfSdOpenFail = perfCounters.perfSdOpenFail.load(std::memory_order_relaxed);
    snapshot.perfSdHeaderFail = perfCounters.perfSdHeaderFail.load(std::memory_order_relaxed);
    snapshot.perfSdMarkerFail = perfCounters.perfSdMarkerFail.load(std::memory_order_relaxed);
    snapshot.perfSdWriteFail = perfCounters.perfSdWriteFail.load(std::memory_order_relaxed);
#if PERF_METRICS
    snapshot.monitoringEnabled = static_cast<bool>(PERF_MONITORING);
#if PERF_MONITORING
    const uint32_t minUsVal = perfLatency.minUs.load(std::memory_order_relaxed);
    snapshot.latencyMinUs = (minUsVal == UINT32_MAX) ? 0 : minUsVal;
    snapshot.latencyAvgUs = perfLatency.avgUs();
    snapshot.latencyMaxUs = perfLatency.maxUs.load(std::memory_order_relaxed);
    snapshot.latencySamples = perfLatency.sampleCount.load(std::memory_order_relaxed);
    snapshot.debugEnabled = perfDebugEnabled;
#endif
#else
    snapshot.metricsEnabled = false;
#endif

    snapshot.wifiAutoStart.gate = wifiAutoStartGateName(ctx.wifiAutoStart.gate);
    snapshot.wifiAutoStart.gateCode = static_cast<uint8_t>(ctx.wifiAutoStart.gate);
    snapshot.wifiAutoStart.enableWifi = ctx.wifiAutoStart.enableWifi;
    snapshot.wifiAutoStart.bleConnected = ctx.wifiAutoStart.bleConnected;
    snapshot.wifiAutoStart.v1ConnectedAtMs = ctx.wifiAutoStart.v1ConnectedAtMs;
    snapshot.wifiAutoStart.msSinceV1Connect = ctx.wifiAutoStart.msSinceV1Connect;
    snapshot.wifiAutoStart.settleMs = ctx.wifiAutoStart.settleMs;
    snapshot.wifiAutoStart.bootTimeoutMs = ctx.wifiAutoStart.bootTimeoutMs;
    snapshot.wifiAutoStart.canStartDma = ctx.wifiAutoStart.canStartDma;
    snapshot.wifiAutoStart.wifiAutoStartDone = ctx.wifiAutoStart.wifiAutoStartDone;
    snapshot.wifiAutoStart.bleSettled = ctx.wifiAutoStart.bleSettled;
    snapshot.wifiAutoStart.bootTimeoutReached = ctx.wifiAutoStart.bootTimeoutReached;
    snapshot.wifiAutoStart.shouldAutoStart = ctx.wifiAutoStart.shouldAutoStart;
    snapshot.wifiAutoStart.startTriggered = ctx.wifiAutoStart.startTriggered;
    snapshot.wifiAutoStart.startSucceeded = ctx.wifiAutoStart.startSucceeded;

    snapshot.settingsPersistence.backupRevision = ctx.backupRevision;
    snapshot.settingsPersistence.deferredBackupPending = ctx.deferredBackupPending;
    snapshot.settingsPersistence.deferredBackupRetryScheduled = ctx.deferredBackupRetryScheduled;
    snapshot.settingsPersistence.deferredBackupHasNextAttempt = ctx.deferredBackupNextAttemptAtMs != 0;
    snapshot.settingsPersistence.deferredBackupNextAttemptAtMs = ctx.deferredBackupNextAttemptAtMs;
    snapshot.settingsPersistence.deferredBackupDelayMs =
        (ctx.deferredBackupNextAttemptAtMs != 0 &&
         static_cast<int32_t>(ctx.deferredBackupNextAttemptAtMs - ctx.nowMs) > 0)
            ? (ctx.deferredBackupNextAttemptAtMs - ctx.nowMs)
            : 0;
    snapshot.settingsPersistence.perfLoggingEnabled = ctx.perfLoggingEnabled;
    snapshot.settingsPersistence.perfLoggingPath = ctx.perfLoggingPath;

    snapshot.speedSource.selected = SpeedSourceSelector::sourceName(ctx.speedStatus.selectedSource);
    snapshot.speedSource.selectedValueValid = ctx.speedStatus.selectedSource != SpeedSource::NONE;
    snapshot.speedSource.selectedMph = snapshot.speedSource.selectedValueValid ? ctx.speedStatus.selectedSpeedMph : 0.0f;
    snapshot.speedSource.selectedAgeMs = snapshot.speedSource.selectedValueValid ? ctx.speedStatus.selectedAgeMs : 0;
    snapshot.speedSource.sourceSwitches = ctx.speedStatus.sourceSwitches;
    snapshot.speedSource.gpsSelections = ctx.speedStatus.gpsSelections;
    snapshot.speedSource.noSourceSelections = ctx.speedStatus.noSourceSelections;

    snapshot.heap.heapFree = ctx.freeHeap;
    snapshot.heap.heapMinFree = perfGetMinFreeHeap();
    snapshot.heap.heapLargest = ctx.largestHeap;
    snapshot.heap.heapInternalFree = ctx.freeDma;
    snapshot.heap.heapInternalFreeMin = snapshot.flat.freeDmaMin;
    snapshot.heap.heapInternalLargest = ctx.largestDma;
    snapshot.heap.heapInternalLargestMin = snapshot.flat.largestDmaMin;
    snapshot.heap.heapDmaFree = ctx.freeDmaCap;
    snapshot.heap.heapDmaFreeMin = snapshot.flat.dmaFreeMin;
    snapshot.heap.heapDmaLargest = ctx.largestDmaCap;
    snapshot.heap.heapDmaLargestMin = snapshot.flat.dmaLargestMin;

    snapshot.psram.total = ctx.psramTotal;
    snapshot.psram.free = ctx.psramFree;
    snapshot.psram.largest = ctx.psramLargest;

    snapshot.sdContention.tryLockFails = ctx.sdTryLockFails;
    snapshot.sdContention.dmaStarvation = ctx.sdDmaStarvation;

    snapshot.proxy.sendCount = ctx.proxyMetrics.sendCount;
    snapshot.proxy.dropCount = ctx.proxyMetrics.dropCount;
    snapshot.proxy.errorCount = ctx.proxyMetrics.errorCount;
    snapshot.proxy.queueHighWater = ctx.proxyMetrics.queueHighWater;
    snapshot.proxy.connected = bleClient.isProxyClientConnected();
    snapshot.proxy.advertising = snapshot.proxyAdvertising;
    snapshot.proxy.advertisingOnTransitions = snapshot.proxyAdvertisingOnTransitions;
    snapshot.proxy.advertisingOffTransitions = snapshot.proxyAdvertisingOffTransitions;
    snapshot.proxy.advertisingLastTransitionMs = snapshot.proxyAdvertisingLastTransitionMs;
    snapshot.proxy.advertisingLastTransitionReasonCode = snapshot.proxyAdvertisingLastTransitionReasonCode;
    snapshot.proxy.advertisingLastTransitionReason = snapshot.proxyAdvertisingLastTransitionReason;

    snapshot.eventBus.publishCount = ctx.eventBusPublishCount;
    snapshot.eventBus.dropCount = ctx.eventBusDropCount;
    snapshot.eventBus.size = ctx.eventBusSize;

    snapshot.connectionCycle.stateCode = ctx.connectionCycleStateCode;
    snapshot.connectionCycle.state = perfConnectionCycleStateName(ctx.connectionCycleStateCode);
    snapshot.connectionCycle.transitionsTotal = ctx.connectionCycleTransitionsTotal;
    snapshot.connectionCycle.timeInStateMs = ctx.connectionCycleTimeInStateMs;
    snapshot.connectionCycle.teardownDurationMs = ctx.connectionCycleTeardownDurationMs;
    snapshot.connectionCycle.obdRetryAttemptsTotal = ctx.connectionCycleObdRetryAttemptsTotal;
    snapshot.connectionCycle.wifiManualPhoneKicksTotal =
        ctx.connectionCycleWifiManualPhoneKicksTotal;
    snapshot.connectionCycle.proxyNoClientLatched =
        ctx.connectionCycleProxyNoClientLatched;
}

static void captureSdSnapshot(PerfSdSnapshot& snapshot) {
    // loopTask has an 8 KB stack budget. Keep the periodic SD snapshot on the
    // flat-only path so Tier 4 observability work cannot pay for the larger
    // runtime wrapper when only the CSV payload is needed.
    const RuntimeSnapshotCaptureContext ctx = captureRuntimeSnapshotContext();
    populateFlatSnapshot(snapshot, ctx, PerfRuntimeSnapshotMode::CaptureAndResetWindowPeaks);
}

} // namespace

void perfMetricsResetSessionWindow() {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    resetPerfExtendedWindowPeaks();
    sDmaFreeCapMin = UINT32_MAX;
    sDmaLargestCapMin = UINT32_MAX;
    sPrevWindowLoopMaxUs.store(0, std::memory_order_relaxed);
    sPrevWindowWifiMaxUs.store(0, std::memory_order_relaxed);
    sPrevWindowBleProcessMaxUs.store(0, std::memory_order_relaxed);
    sPrevWindowDispPipeMaxUs.store(0, std::memory_order_relaxed);
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfCaptureRuntimeMetricsSnapshot(PerfRuntimeMetricsSnapshot& snapshot,
                                       PerfRuntimeSnapshotMode mode) {
    const RuntimeSnapshotCaptureContext ctx = captureRuntimeSnapshotContext();
    populateRuntimeSnapshot(snapshot, ctx, mode);
}

void perfSetConnectionCycleSnapshot(uint8_t stateCode,
                                    uint32_t timeInStateMs,
                                    uint32_t transitionsTotal,
                                    uint32_t teardownDurationMs,
                                    uint32_t obdRetryAttemptsTotal,
                                    uint32_t wifiManualPhoneKicksTotal,
                                    bool proxyNoClientLatched) {
    sConnectionCycleStateCode.store(stateCode, std::memory_order_relaxed);
    sConnectionCycleTimeInStateMs.store(timeInStateMs, std::memory_order_relaxed);
    sConnectionCycleTransitionsTotal.store(transitionsTotal, std::memory_order_relaxed);
    sConnectionCycleTeardownDurationMs.store(teardownDurationMs, std::memory_order_relaxed);
    sConnectionCycleObdRetryAttemptsTotal.store(obdRetryAttemptsTotal,
                                                std::memory_order_relaxed);
    sConnectionCycleWifiManualPhoneKicksTotal.store(wifiManualPhoneKicksTotal,
                                                    std::memory_order_relaxed);
    sConnectionCycleProxyNoClientLatched.store(proxyNoClientLatched ? 1 : 0,
                                               std::memory_order_relaxed);
}

const char* perfConnectionCycleStateName(uint8_t stateCode) {
    return connectionCycleStateNameInternal(stateCode);
}

void perfRecordNotifyToDisplayMs(uint32_t ms) {
    addLatencySample(perfExtended.notifyToDisplayMs, ms);
}


void perfRecordLoopJitterUs(uint32_t us) {
    if (us > perfExtended.loopMaxUs) {
        perfExtended.loopMaxUs = us;
    }
}

void perfRecordHeapStats(uint32_t freeHeap, uint32_t largestBlock, uint32_t freeDma, uint32_t largestDma) {
    if (freeHeap < perfExtended.minFreeHeap) {
        perfExtended.minFreeHeap = freeHeap;
    }
    if (largestBlock < perfExtended.minLargestBlock) {
        perfExtended.minLargestBlock = largestBlock;
    }
    if (freeDma < perfExtended.minFreeDma) {
        perfExtended.minFreeDma = freeDma;
    }
    if (largestDma < perfExtended.minLargestDma) {
        perfExtended.minLargestDma = largestDma;
    }
}

void perfRecordWifiProcessUs(uint32_t us) {
    if (us > perfExtended.wifiMaxUs) {
        perfExtended.wifiMaxUs = us;
    }
}

void perfRecordWifiHandleClientUs(uint32_t us) {
    if (us > perfExtended.wifiHandleClientMaxUs) {
        perfExtended.wifiHandleClientMaxUs = us;
    }
}

void perfRecordWifiMaintenanceUs(uint32_t us) {
    if (us > perfExtended.wifiMaintenanceMaxUs) {
        perfExtended.wifiMaintenanceMaxUs = us;
    }
}

void perfRecordWifiStatusCheckUs(uint32_t us) {
    if (us > perfExtended.wifiStatusCheckMaxUs) {
        perfExtended.wifiStatusCheckMaxUs = us;
    }
}

void perfRecordWifiTimeoutCheckUs(uint32_t us) {
    if (us > perfExtended.wifiTimeoutCheckMaxUs) {
        perfExtended.wifiTimeoutCheckMaxUs = us;
    }
}

void perfRecordWifiHeapGuardUs(uint32_t us) {
    if (us > perfExtended.wifiHeapGuardMaxUs) {
        perfExtended.wifiHeapGuardMaxUs = us;
    }
}

void perfRecordWifiApStaPollUs(uint32_t us) {
    if (us > perfExtended.wifiApStaPollMaxUs) {
        perfExtended.wifiApStaPollMaxUs = us;
    }
}

void perfRecordWifiStopHttpServerUs(uint32_t us) {
    if (us > perfExtended.wifiStopHttpServerMaxUs) {
        perfExtended.wifiStopHttpServerMaxUs = us;
    }
}

void perfRecordWifiStopStaDisconnectUs(uint32_t us) {
    if (us > perfExtended.wifiStopStaDisconnectMaxUs) {
        perfExtended.wifiStopStaDisconnectMaxUs = us;
    }
}

void perfRecordWifiStopApDisableUs(uint32_t us) {
    if (us > perfExtended.wifiStopApDisableMaxUs) {
        perfExtended.wifiStopApDisableMaxUs = us;
    }
}

void perfRecordWifiStopModeOffUs(uint32_t us) {
    if (us > perfExtended.wifiStopModeOffMaxUs) {
        perfExtended.wifiStopModeOffMaxUs = us;
    }
}

void perfRecordWifiStartPreflightUs(uint32_t us) {
    if (us > perfExtended.wifiStartPreflightMaxUs) {
        perfExtended.wifiStartPreflightMaxUs = us;
    }
}

void perfRecordWifiStartApBringupUs(uint32_t us) {
    if (us > perfExtended.wifiStartApBringupMaxUs) {
        perfExtended.wifiStartApBringupMaxUs = us;
    }
}

void perfRecordFsServeUs(uint32_t us) {
    if (us > perfExtended.fsMaxUs) {
        perfExtended.fsMaxUs = us;
    }
}

void perfRecordSdFlushUs(uint32_t us) {
    if (us > perfExtended.sdMaxUs) {
        perfExtended.sdMaxUs = us;
    }
    perfExtended.sdWriteCount++;
    if (us < 1000) {
        perfExtended.sdWriteLt1msCount++;
    } else if (us < 5000) {
        perfExtended.sdWrite1to5msCount++;
    } else if (us < 10000) {
        perfExtended.sdWrite5to10msCount++;
    } else {
        perfExtended.sdWriteGe10msCount++;
    }
}

void perfRecordFlushUs(uint32_t us, uint32_t areaPx, bool fullFlush) {
    if (us > perfExtended.flushMaxUs) {
        perfExtended.flushMaxUs = us;
        perfExtended.displayFlushMaxAreaPx = areaPx;
    }
    perfExtended.displayFlushEquivalentAreaTotalPx += areaPx;
    if (fullFlush) {
        perfExtended.displayFullFlushCount++;
    } else {
        perfExtended.displayPartialFlushCount++;
        perfExtended.displayPartialFlushAreaTotalPx += areaPx;
        if (areaPx > perfExtended.displayPartialFlushAreaPeakPx) {
            perfExtended.displayPartialFlushAreaPeakPx = areaPx;
        }
    }
}

void perfRecordPartialFlushShape(uint32_t us, uint32_t areaPx, uint16_t logicalW, uint16_t logicalH) {
    const uint32_t rowCalls = logicalW;
    const uint32_t pixelsPerRow = logicalH;

    if (logicalW > perfExtended.displayPartialFlushLogicalWidthPeakPx) {
        perfExtended.displayPartialFlushLogicalWidthPeakPx = logicalW;
    }
    if (logicalH > perfExtended.displayPartialFlushLogicalHeightPeakPx) {
        perfExtended.displayPartialFlushLogicalHeightPeakPx = logicalH;
    }
    if (rowCalls > perfExtended.displayPartialFlushRowCallsPeak) {
        perfExtended.displayPartialFlushRowCallsPeak = rowCalls;
    }
    if (pixelsPerRow > perfExtended.displayPartialFlushPixelsPerRowPeakPx) {
        perfExtended.displayPartialFlushPixelsPerRowPeakPx = pixelsPerRow;
    }

    if (us > perfExtended.displayPartialFlushUsPeak) {
        perfExtended.displayPartialFlushUsPeak = us;
        perfExtended.displayPartialFlushWorstUsLogicalWidthPx = logicalW;
        perfExtended.displayPartialFlushWorstUsLogicalHeightPx = logicalH;
        perfExtended.displayPartialFlushWorstUsAreaPx = areaPx;
    }

    // Shadow-only row-cap decisions. These counters intentionally do not alter
    // the dispatch path; they let hardware runs show how often a future
    // row-aware heuristic would have selected a full flush instead.
    if (rowCalls > 64u) {
        perfExtended.displayPartialFlushWouldFullRows64Count++;
    }
    if (rowCalls > 128u) {
        perfExtended.displayPartialFlushWouldFullRows128Count++;
    }
    if (rowCalls > 256u) {
        perfExtended.displayPartialFlushWouldFullRows256Count++;
    }
}

void perfRecordDisplayUnionExceedsCap(uint32_t areaPx, uint8_t rectCount, uint8_t sourceMask) {
    if (areaPx > perfExtended.displayUnionExceedsCapAreaPeakPx) {
        perfExtended.displayUnionExceedsCapAreaPeakPx = areaPx;
        perfExtended.displayUnionExceedsCapAreaPeakSourceMask = sourceMask;
    }
    if (rectCount > perfExtended.displayUnionExceedsCapRectCountPeak) {
        perfExtended.displayUnionExceedsCapRectCountPeak = rectCount;
    }

    if (sourceMask & DisplayDirtyRegionSource::Frequency) {
        perfExtended.displayUnionExceedsCapWithFrequencyCount++;
    }
    if (sourceMask & (DisplayDirtyRegionSource::Bands | DisplayDirtyRegionSource::SignalBars)) {
        perfExtended.displayUnionExceedsCapWithBandsBarsCount++;
    }
    if (sourceMask & DisplayDirtyRegionSource::Arrows) {
        perfExtended.displayUnionExceedsCapWithArrowsCount++;
    }
    if (sourceMask & DisplayDirtyRegionSource::Status) {
        perfExtended.displayUnionExceedsCapWithStatusCount++;
    }
    if (sourceMask & DisplayDirtyRegionSource::Indicators) {
        perfExtended.displayUnionExceedsCapWithIndicatorsCount++;
    }
    if (sourceMask & DisplayDirtyRegionSource::External) {
        perfExtended.displayUnionExceedsCapWithExternalCount++;
    }
    if ((sourceMask == 0) || (sourceMask & DisplayDirtyRegionSource::Unknown)) {
        perfExtended.displayUnionExceedsCapUnclassifiedCount++;
    }
}

void perfRecordDisplayRenderUs(uint32_t us) {
    if (us > perfExtended.displayRenderMaxUs) {
        perfExtended.displayRenderMaxUs = us;
    }
}

void perfRecordDisplayScenarioRenderUs(uint32_t us) {
    const PerfDisplayRenderScenario scenario = currentDisplayRenderScenario();
    recordDisplayScenarioRenderCount(scenario);
    recordDisplayScenarioRenderMax(scenario, us);
}

void perfRecordDisplayRenderPath(PerfDisplayRenderPath path) {
    switch (path) {
        case PerfDisplayRenderPath::Full:
            perfExtended.displayFullRenderCount++;
            break;
        case PerfDisplayRenderPath::RestingFull:
            perfExtended.displayRestingFullRenderCount++;
            break;
        case PerfDisplayRenderPath::RestingIncremental:
            perfExtended.displayRestingIncrementalRenderCount++;
            break;
        case PerfDisplayRenderPath::Persisted:
            perfExtended.displayPersistedRenderCount++;
            break;
        case PerfDisplayRenderPath::Preview:
            perfExtended.displayPreviewRenderCount++;
            break;
        case PerfDisplayRenderPath::Restore:
            perfExtended.displayRestoreRenderCount++;
            break;
        default:
            break;
    }
}

void perfRecordDisplayRedrawReason(PerfDisplayRedrawReason reason) {
    switch (reason) {
        case PerfDisplayRedrawReason::FirstRun:
            perfExtended.displayRedrawReasonFirstRunCount++;
            break;
        case PerfDisplayRedrawReason::EnterLive:
            perfExtended.displayRedrawReasonEnterLiveCount++;
            break;
        case PerfDisplayRedrawReason::LeaveLive:
            perfExtended.displayRedrawReasonLeaveLiveCount++;
            break;
        case PerfDisplayRedrawReason::LeavePersisted:
            perfExtended.displayRedrawReasonLeavePersistedCount++;
            break;
        case PerfDisplayRedrawReason::ForceRedraw:
            perfExtended.displayRedrawReasonForceRedrawCount++;
            break;
        case PerfDisplayRedrawReason::FrequencyChange:
            perfExtended.displayRedrawReasonFrequencyChangeCount++;
            break;
        case PerfDisplayRedrawReason::BandSetChange:
            perfExtended.displayRedrawReasonBandSetChangeCount++;
            break;
        case PerfDisplayRedrawReason::ArrowChange:
            perfExtended.displayRedrawReasonArrowChangeCount++;
            break;
        case PerfDisplayRedrawReason::SignalBarChange:
            perfExtended.displayRedrawReasonSignalBarChangeCount++;
            break;
        case PerfDisplayRedrawReason::VolumeChange:
            perfExtended.displayRedrawReasonVolumeChangeCount++;
            break;
        case PerfDisplayRedrawReason::BogeyCounterChange:
            perfExtended.displayRedrawReasonBogeyCounterChangeCount++;
            break;
        case PerfDisplayRedrawReason::RssiRefresh:
            perfExtended.displayRedrawReasonRssiRefreshCount++;
            break;
        case PerfDisplayRedrawReason::FlashTick:
            perfExtended.displayRedrawReasonFlashTickCount++;
            break;
        case PerfDisplayRedrawReason::FullFlushForRedraw:
            perfExtended.displayRedrawReasonFullFlushForRedrawCount++;
            break;
        case PerfDisplayRedrawReason::CacheHitSkipFlush:
            perfExtended.displayRedrawReasonCacheHitSkipFlushCount++;
            break;
        case PerfDisplayRedrawReason::UnionExceedsCap:
            perfExtended.displayRedrawReasonUnionExceedsCapCount++;
            break;
        case PerfDisplayRedrawReason::PartialRegionFlush:
            perfExtended.displayRedrawReasonPartialRegionFlushCount++;
            break;
        default:
            break;
    }
}

void perfRecordDisplayFlushDecision(PerfDisplayFlushDecisionPath path,
                                    PerfDisplayFlushDecisionReason reason) {
    uint32_t* target = nullptr;
    switch (path) {
        case PerfDisplayFlushDecisionPath::Resting:
            switch (reason) {
                case PerfDisplayFlushDecisionReason::FullRedraw:
                    target = &perfExtended.displayRestingFlushReasonFullRedrawCount;
                    break;
                case PerfDisplayFlushDecisionReason::PendingExternal:
                    target = &perfExtended.displayRestingFlushReasonPendingExternalCount;
                    break;
                case PerfDisplayFlushDecisionReason::Painted:
                    target = &perfExtended.displayRestingFlushReasonPaintedCount;
                    break;
                case PerfDisplayFlushDecisionReason::CacheHit:
                    target = &perfExtended.displayRestingFlushReasonCacheHitCount;
                    break;
            }
            break;
        case PerfDisplayFlushDecisionPath::Persisted:
            switch (reason) {
                case PerfDisplayFlushDecisionReason::FullRedraw:
                    target = &perfExtended.displayPersistedFlushReasonFullRedrawCount;
                    break;
                case PerfDisplayFlushDecisionReason::PendingExternal:
                    target = &perfExtended.displayPersistedFlushReasonPendingExternalCount;
                    break;
                case PerfDisplayFlushDecisionReason::Painted:
                    target = &perfExtended.displayPersistedFlushReasonPaintedCount;
                    break;
                case PerfDisplayFlushDecisionReason::CacheHit:
                    target = &perfExtended.displayPersistedFlushReasonCacheHitCount;
                    break;
            }
            break;
    }
    if (target) {
        ++(*target);
    }
}

void perfRecordDisplayStatusPaint(PerfDisplayStatusPaint element) {
    switch (element) {
        case PerfDisplayStatusPaint::Volume:
            perfExtended.displayStatusVolumePaintCount++;
            break;
        case PerfDisplayStatusPaint::Rssi:
            perfExtended.displayStatusRssiPaintCount++;
            break;
        case PerfDisplayStatusPaint::Profile:
            perfExtended.displayStatusProfilePaintCount++;
            break;
        case PerfDisplayStatusPaint::Battery:
            perfExtended.displayStatusBatteryPaintCount++;
            break;
        case PerfDisplayStatusPaint::BleProxy:
            perfExtended.displayStatusBleProxyPaintCount++;
            break;
        case PerfDisplayStatusPaint::Wifi:
            perfExtended.displayStatusWifiPaintCount++;
            break;
        case PerfDisplayStatusPaint::Obd:
            perfExtended.displayStatusObdPaintCount++;
            break;
        case PerfDisplayStatusPaint::Gps:
            perfExtended.displayStatusGpsPaintCount++;
            break;
        case PerfDisplayStatusPaint::Alp:
            perfExtended.displayStatusAlpPaintCount++;
            break;
    }
}

void perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase subphase, uint32_t us) {
    switch (subphase) {
        case PerfDisplayRenderSubphase::BaseFrame:
            if (us > perfExtended.displayBaseFrameMaxUs) {
                perfExtended.displayBaseFrameMaxUs = us;
            }
            break;
        case PerfDisplayRenderSubphase::StatusStrip:
            if (us > perfExtended.displayStatusStripMaxUs) {
                perfExtended.displayStatusStripMaxUs = us;
            }
            break;
        case PerfDisplayRenderSubphase::Frequency:
            if (us > perfExtended.displayFrequencyMaxUs) {
                perfExtended.displayFrequencyMaxUs = us;
            }
            break;
        case PerfDisplayRenderSubphase::BandsBars:
            if (us > perfExtended.displayBandsBarsMaxUs) {
                perfExtended.displayBandsBarsMaxUs = us;
            }
            break;
        case PerfDisplayRenderSubphase::ArrowsIcons:
            if (us > perfExtended.displayArrowsIconsMaxUs) {
                perfExtended.displayArrowsIconsMaxUs = us;
            }
            break;
        case PerfDisplayRenderSubphase::Flush:
            if (us > perfExtended.displayFlushSubphaseMaxUs) {
                perfExtended.displayFlushSubphaseMaxUs = us;
            }
            break;
        default:
            break;
    }
}

void perfSetDisplayRenderScenario(PerfDisplayRenderScenario scenario) {
    sDisplayRenderScenario.store(static_cast<uint8_t>(scenario), std::memory_order_relaxed);
}

PerfDisplayRenderScenario perfGetDisplayRenderScenario() {
    return currentDisplayRenderScenario();
}

void perfClearDisplayRenderScenario() {
    perfSetDisplayRenderScenario(PerfDisplayRenderScenario::None);
}

void perfRecordBleDrainUs(uint32_t us) {
    if (us > perfExtended.bleDrainMaxUs) {
        perfExtended.bleDrainMaxUs = us;
    }
}

void perfRecordBleConnectUs(uint32_t us) {
    if (us > perfExtended.bleConnectMaxUs) {
        perfExtended.bleConnectMaxUs = us;
    }
}

void perfRecordBleDiscoveryUs(uint32_t us) {
    if (us > perfExtended.bleDiscoveryMaxUs) {
        perfExtended.bleDiscoveryMaxUs = us;
    }
}

void perfRecordBleSubscribeUs(uint32_t us) {
    if (us > perfExtended.bleSubscribeMaxUs) {
        perfExtended.bleSubscribeMaxUs = us;
    }
}

void perfRecordBleFollowupRequestAlertUs(uint32_t us) {
    if (us > perfExtended.bleFollowupRequestAlertMaxUs) {
        perfExtended.bleFollowupRequestAlertMaxUs = us;
    }
}

void perfRecordBleFollowupRequestVersionUs(uint32_t us) {
    if (us > perfExtended.bleFollowupRequestVersionMaxUs) {
        perfExtended.bleFollowupRequestVersionMaxUs = us;
    }
}

void perfRecordV1FirmwareVersion(uint32_t version) {
    // Record the decoded V1 firmware version.
    // 0 means "not yet known", so reject 0 to avoid clobbering a known value
    // when a non-V1 ESP device replies on the bus.
    if (version != 0) {
        perfExtended.v1FirmwareVersion = version;
    }
}

void perfRecordBleConnectStableCallbackUs(uint32_t us) {
    if (us > perfExtended.bleConnectStableCallbackMaxUs) {
        perfExtended.bleConnectStableCallbackMaxUs = us;
    }
}

void perfRecordBleProxyStartUs(uint32_t us) {
    if (us > perfExtended.bleProxyStartMaxUs) {
        perfExtended.bleProxyStartMaxUs = us;
    }
}

void perfRecordBleProcessUs(uint32_t us) {
    if (us > perfExtended.bleProcessMaxUs) {
        perfExtended.bleProcessMaxUs = us;
    }
}

void perfRecordDispPipeUs(uint32_t us) {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    if (us > perfExtended.dispPipeMaxUs) {
        perfExtended.dispPipeMaxUs = us;
    }
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordDisplayVoiceUs(uint32_t us) {
    if (us > perfExtended.displayVoiceMaxUs) {
        perfExtended.displayVoiceMaxUs = us;
    }
}

void perfRecordDisplayGapRecoverUs(uint32_t us) {
    if (us > perfExtended.displayGapRecoverMaxUs) {
        perfExtended.displayGapRecoverMaxUs = us;
    }
}

void perfRecordTouchUs(uint32_t us) {
    if (us > perfExtended.touchMaxUs) {
        perfExtended.touchMaxUs = us;
    }
}

void perfRecordObdUs(uint32_t us) {
    if (us > perfExtended.obdMaxUs) {
        perfExtended.obdMaxUs = us;
    }
}

void perfRecordObdConnectCallUs(uint32_t us) {
    if (us > perfExtended.obdConnectCallMaxUs) {
        perfExtended.obdConnectCallMaxUs = us;
    }
}

void perfRecordObdSecurityStartCallUs(uint32_t us) {
    if (us > perfExtended.obdSecurityStartCallMaxUs) {
        perfExtended.obdSecurityStartCallMaxUs = us;
    }
}

void perfRecordObdDiscoveryCallUs(uint32_t us) {
    if (us > perfExtended.obdDiscoveryCallMaxUs) {
        perfExtended.obdDiscoveryCallMaxUs = us;
    }
}

void perfRecordObdSubscribeCallUs(uint32_t us) {
    if (us > perfExtended.obdSubscribeCallMaxUs) {
        perfExtended.obdSubscribeCallMaxUs = us;
    }
}

void perfRecordObdWriteCallUs(uint32_t us) {
    if (us > perfExtended.obdWriteCallMaxUs) {
        perfExtended.obdWriteCallMaxUs = us;
    }
}

void perfRecordObdRssiCallUs(uint32_t us) {
    if (us > perfExtended.obdRssiCallMaxUs) {
        perfExtended.obdRssiCallMaxUs = us;
    }
}

void perfRecordPerfReportUs(uint32_t us) {
    if (us > perfExtended.perfReportMaxUs) {
        perfExtended.perfReportMaxUs = us;
    }
}

void perfRecordDisplayScreenTransition(PerfDisplayScreen from, PerfDisplayScreen to, uint32_t nowMs) {
    if (from == to) {
        return;
    }

    portENTER_CRITICAL(&sPerfSnapshotMux);

    if (to == PerfDisplayScreen::Scanning) {
        perfExtended.uiToScanCount++;
        perfExtended.uiLastScanEnteredMs = nowMs;
    } else if (to == PerfDisplayScreen::Resting) {
        perfExtended.uiToRestCount++;
    }

    if (from == PerfDisplayScreen::Scanning && to == PerfDisplayScreen::Resting) {
        perfExtended.uiScanToRestCount++;
    }

    if (from == PerfDisplayScreen::Scanning && to != PerfDisplayScreen::Scanning) {
        if (to != PerfDisplayScreen::Unknown && perfExtended.uiLastScanEnteredMs > 0) {
            uint32_t dwellMs = nowMs - perfExtended.uiLastScanEnteredMs;
            perfExtended.uiLastScanDwellMs = dwellMs;
            if (dwellMs < perfExtended.uiMinScanDwellMs) {
                perfExtended.uiMinScanDwellMs = dwellMs;
            }
            if (dwellMs < kFastScanExitThresholdMs) {
                perfExtended.uiFastScanExitCount++;
            }
        }
        perfExtended.uiLastScanEnteredMs = 0;
    }

    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordVolumeFadeDecision(PerfFadeDecision decision, uint8_t currentVolume, uint8_t originalVolume, uint32_t nowMs) {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    switch (decision) {
        case PerfFadeDecision::FadeDown:
            perfExtended.fadeDownCount++;
            break;
        case PerfFadeDecision::RestoreApplied:
            perfExtended.fadeRestoreCount++;
            break;
        case PerfFadeDecision::RestoreSkippedEqual:
            perfExtended.fadeSkipEqualCount++;
            break;
        case PerfFadeDecision::RestoreSkippedNoBaseline:
            perfExtended.fadeSkipNoBaselineCount++;
            break;
        case PerfFadeDecision::RestoreSkippedNotFaded:
            perfExtended.fadeSkipNotFadedCount++;
            break;
        case PerfFadeDecision::None:
        default:
            break;
    }
    perfExtended.fadeLastDecision = static_cast<uint8_t>(decision);
    perfExtended.fadeLastCurrentVol = currentVolume;
    perfExtended.fadeLastOriginalVol = originalVolume;
    perfExtended.fadeLastDecisionMs = nowMs;
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}


void perfRecordSpeedVolDrop() {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    perfExtended.speedVolDropCount++;
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordSpeedVolRestore() {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    perfExtended.speedVolRestoreCount++;
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordSpeedVolRetry() {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    perfExtended.speedVolRetryCount++;
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordBleTimelineEvent(PerfBleTimelineEvent event, uint32_t nowMs) {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    uint32_t* target = nullptr;
    switch (event) {
        case PerfBleTimelineEvent::ScanStart:
            target = &perfExtended.bleScanStartMs;
            break;
        case PerfBleTimelineEvent::TargetFound:
            target = &perfExtended.bleTargetFoundMs;
            break;
        case PerfBleTimelineEvent::ConnectStart:
            target = &perfExtended.bleConnectStartMs;
            break;
        case PerfBleTimelineEvent::Connected:
            target = &perfExtended.bleConnectedMs;
            break;
        case PerfBleTimelineEvent::FirstRx:
            target = &perfExtended.bleFirstRxMs;
            break;
        default:
            break;
    }
    if (target && *target == 0) {
        *target = nowMs;
    }
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordWifiApTransition(bool apActive, uint8_t reasonCode, uint32_t nowMs) {
    const uint32_t newState = apActive ? 1u : 0u;
    const uint32_t previousState = perfCounters.wifiApState.exchange(newState, std::memory_order_relaxed);
    if (previousState == newState) {
        return;
    }
    if (newState != 0u) {
        PERF_INC(wifiApUpTransitions);
    } else {
        PERF_INC(wifiApDownTransitions);
    }
    perfCounters.wifiApLastTransitionMs.store(nowMs, std::memory_order_relaxed);
    perfCounters.wifiApLastTransitionReason.store(reasonCode, std::memory_order_relaxed);
}

void perfRecordProxyAdvertisingTransition(bool advertising, uint8_t reasonCode, uint32_t nowMs) {
    const uint32_t newState = advertising ? 1u : 0u;
    const uint32_t previousState =
        perfCounters.proxyAdvertisingState.exchange(newState, std::memory_order_relaxed);
    if (previousState == newState) {
        return;
    }
    if (newState != 0u) {
        PERF_INC(proxyAdvertisingOnTransitions);
    } else {
        PERF_INC(proxyAdvertisingOffTransitions);
    }
    perfCounters.proxyAdvertisingLastTransitionMs.store(nowMs, std::memory_order_relaxed);
    perfCounters.proxyAdvertisingLastTransitionReason.store(reasonCode, std::memory_order_relaxed);
}

uint32_t perfGetMinFreeHeap() { return perfExtended.minFreeHeap == UINT32_MAX ? 0 : perfExtended.minFreeHeap; }
uint32_t perfGetMinFreeDma() { return perfExtended.minFreeDma == UINT32_MAX ? 0 : perfExtended.minFreeDma; }
uint32_t perfGetPrevWindowLoopMaxUs() {
    return sPrevWindowLoopMaxUs.load(std::memory_order_relaxed);
}
uint32_t perfGetPrevWindowWifiMaxUs() {
    return sPrevWindowWifiMaxUs.load(std::memory_order_relaxed);
}
uint32_t perfGetPrevWindowBleProcessMaxUs() {
    return sPrevWindowBleProcessMaxUs.load(std::memory_order_relaxed);
}
uint32_t perfGetPrevWindowDispPipeMaxUs() {
    return sPrevWindowDispPipeMaxUs.load(std::memory_order_relaxed);
}
uint32_t perfGetWifiApState() {
    return perfCounters.wifiApState.load(std::memory_order_relaxed);
}
uint32_t perfGetWifiApLastTransitionMs() {
    return perfCounters.wifiApLastTransitionMs.load(std::memory_order_relaxed);
}
uint32_t perfGetWifiApLastTransitionReason() {
    return perfCounters.wifiApLastTransitionReason.load(std::memory_order_relaxed);
}
const char* perfWifiApTransitionReasonName(uint32_t reasonCode) {
    return wifiApTransitionReasonNameInternal(reasonCode);
}
uint32_t perfGetProxyAdvertisingState() {
    return perfCounters.proxyAdvertisingState.load(std::memory_order_relaxed);
}
uint32_t perfGetProxyAdvertisingLastTransitionMs() {
    return perfCounters.proxyAdvertisingLastTransitionMs.load(std::memory_order_relaxed);
}
uint32_t perfGetProxyAdvertisingLastTransitionReason() {
    return perfCounters.proxyAdvertisingLastTransitionReason.load(std::memory_order_relaxed);
}
const char* perfProxyAdvertisingTransitionReasonName(uint32_t reasonCode) {
    return proxyAdvertisingTransitionReasonNameInternal(reasonCode);
}

#if PERF_METRICS && PERF_MONITORING
namespace {
const char* stackLocationLabel(bool active, bool inPsram) {
    if (!active) {
        return "off";
    }
    return inPsram ? "psram" : "internal";
}
}  // namespace

static void reportTaskStackHighWaterMarks() {
#ifndef UNIT_TEST
    static uint32_t lastStackReportMs = 0;
    const uint32_t now = millis();
    constexpr uint32_t STACK_REPORT_INTERVAL_MS = 30000;
    if (lastStackReportMs != 0 && now - lastStackReportMs < STACK_REPORT_INTERVAL_MS) {
        return;
    }
    lastStackReportMs = now;

    const bool perfActive = perfSdLogger.writerTaskActive();
    const bool alpActive = alpSdLogger.writerTaskActive();
    const bool obdActive = obdRuntimeModule.transportTaskActive();
    if (!perfActive && !alpActive && !obdActive) {
        return;
    }

    Serial.printf("[STACK] aux_free_bytes perfSd=%lu/%s alpSd=%lu/%s obd=%lu/%s\n",
                  static_cast<unsigned long>(
                      perfActive ? perfSdLogger.writerStackHighWaterBytes() : 0),
                  stackLocationLabel(perfActive, perfSdLogger.writerTaskStackInPsram()),
                  static_cast<unsigned long>(
                      alpActive ? alpSdLogger.writerStackHighWaterBytes() : 0),
                  stackLocationLabel(alpActive, alpSdLogger.writerTaskStackInPsram()),
                  static_cast<unsigned long>(
                      obdActive ? obdRuntimeModule.transportStackHighWaterBytes() : 0),
                  stackLocationLabel(obdActive, obdRuntimeModule.transportTaskStackInPsram()));
#endif
}

bool perfMetricsCheckReport() {
    uint32_t now = millis();
    constexpr uint32_t STABILITY_REPORT_INTERVAL_MS = 5000;
    if (perfLastReportMs == 0) {
        perfLastReportMs = now;
        return false;
    }
    if (now - perfLastReportMs < STABILITY_REPORT_INTERVAL_MS) {
        return false;
    }
    perfLastReportMs = now;

    // Always capture the snapshot to cycle windowed maxima (prev-window
    // store + reset).  Without this, API-polled metrics like wifiMaxUs
    // accumulate as max-ever instead of per-window when SD is absent.
    PerfSdSnapshot snapshot{};
    captureSdSnapshot(snapshot);

    // Report stack high water marks for accessible tasks
    reportTaskStackHighWaterMarks();

    // Check DMA free threshold
    if (snapshot.freeDmaCap < 8192) {
        Serial.printf("[HEAP] WARNING: DMA free critically low: %lu bytes\n",
                      (unsigned long)snapshot.freeDmaCap);
    }

    if (perfSdLogger.isEnabled() && !perfMetricsIsSdCapturePaused()) {
        perfSdLogger.enqueue(snapshot);
    }
    return true;
}
#else
bool perfMetricsCheckReport() {
    return false;
}
#endif

bool perfMetricsEnqueueSnapshotNow() {
    if (!perfSdLogger.isEnabled() || perfMetricsIsSdCapturePaused()) {
        return false;
    }

    PerfSdSnapshot snapshot{};
    captureSdSnapshot(snapshot);
    return perfSdLogger.enqueue(snapshot);
}

void perfMetricsSetSdCapturePaused(bool paused) {
    sSdCapturePaused.store(paused, std::memory_order_relaxed);
}

bool perfMetricsIsSdCapturePaused() {
    return sSdCapturePaused.load(std::memory_order_relaxed);
}

void perfMetricsSetDebug(bool enabled) {
#if PERF_METRICS && PERF_MONITORING
    perfDebugEnabled = enabled;
    if (enabled) {
        perfLastReportMs = millis();  // Reset report timer
    }
#else
    (void)enabled;
#endif
}
