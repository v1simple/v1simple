/**
 * Low-Overhead Performance Metrics Implementation
 */

#include "perf_metrics.h"
#include "perf_metrics_internal.h"
#include "audio_beep.h"
#include "ble_bond_backup_writer.h"
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
uint32_t sDmaFreeCapMin = UINT32_MAX;
uint32_t sDmaLargestCapMin = UINT32_MAX;
std::atomic<uint32_t> sPrevWindowLoopMaxUs{0};
std::atomic<uint32_t> sPrevWindowWifiMaxUs{0};
std::atomic<uint32_t> sPrevWindowBleProcessMaxUs{0};
std::atomic<uint32_t> sPrevWindowDispPipeMaxUs{0};
std::atomic<uint8_t> sConnectionCycleStateCode{0};
std::atomic<uint32_t> sConnectionCycleTimeInStateMs{0};
std::atomic<uint32_t> sConnectionCycleTransitionsTotal{0};
std::atomic<uint32_t> sConnectionCycleTeardownDurationMs{0};
std::atomic<uint32_t> sConnectionCycleObdRetryAttemptsTotal{0};
std::atomic<uint32_t> sConnectionCycleWifiManualPhoneKicksTotal{0};
std::atomic<uint8_t> sConnectionCycleProxyNoClientLatched{0};
static std::atomic<uint8_t> sDisplayRenderScenario{
    static_cast<uint8_t>(PerfDisplayRenderScenario::None)};
static std::atomic<bool> sSdCapturePaused{false};
portMUX_TYPE sPerfSnapshotMux = portMUX_INITIALIZER_UNLOCKED;

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
} // namespace

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
        case PerfDisplayRenderSubphase::Cards:
            if (us > perfExtended.displayCardsMaxUs) {
                perfExtended.displayCardsMaxUs = us;
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
uint32_t perfGetProxyAdvertisingState() {
    return perfCounters.proxyAdvertisingState.load(std::memory_order_relaxed);
}
uint32_t perfGetProxyAdvertisingLastTransitionMs() {
    return perfCounters.proxyAdvertisingLastTransitionMs.load(std::memory_order_relaxed);
}
uint32_t perfGetProxyAdvertisingLastTransitionReason() {
    return perfCounters.proxyAdvertisingLastTransitionReason.load(std::memory_order_relaxed);
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
