/**
 * Perf snapshot capture / populate paths.
 * Moved verbatim out of perf_metrics.cpp; no behavior change.
 */

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

namespace {

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
    ctx.connectionCycleTransitionsTotal = sConnectionCycleTransitionsTotal.load(std::memory_order_relaxed);
    ctx.connectionCycleTeardownDurationMs = sConnectionCycleTeardownDurationMs.load(std::memory_order_relaxed);
    ctx.connectionCycleObdRetryAttemptsTotal = sConnectionCycleObdRetryAttemptsTotal.load(std::memory_order_relaxed);
    ctx.connectionCycleWifiManualPhoneKicksTotal =
        sConnectionCycleWifiManualPhoneKicksTotal.load(std::memory_order_relaxed);
    ctx.connectionCycleProxyNoClientLatched = sConnectionCycleProxyNoClientLatched.load(std::memory_order_relaxed) != 0;
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
    perfExtended.displayCardsMaxUs = 0;
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

static void capturePerfExtendedSnapshot(PerfExtendedSnapshot& snapshot, const RuntimeSnapshotCaptureContext& ctx,
                                        PerfRuntimeSnapshotMode mode) {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    if (mode == PerfRuntimeSnapshotMode::CaptureAndResetWindowPeaks && ctx.freeDmaCap < sDmaFreeCapMin) {
        sDmaFreeCapMin = ctx.freeDmaCap;
    }
    if (mode == PerfRuntimeSnapshotMode::CaptureAndResetWindowPeaks && ctx.largestDmaCap < sDmaLargestCapMin) {
        sDmaLargestCapMin = ctx.largestDmaCap;
    }
    snapshot.dmaFreeMin = (sDmaFreeCapMin == UINT32_MAX) ? ctx.freeDmaCap : sDmaFreeCapMin;
    snapshot.dmaLargestMin = (sDmaLargestCapMin == UINT32_MAX) ? ctx.largestDmaCap : sDmaLargestCapMin;
    snapshot.metrics = perfExtended;

    if (mode == PerfRuntimeSnapshotMode::CaptureAndResetWindowPeaks) {
        sPrevWindowLoopMaxUs.store(snapshot.metrics.loopMaxUs, std::memory_order_relaxed);
        sPrevWindowWifiMaxUs.store(snapshot.metrics.wifiMaxUs, std::memory_order_relaxed);
        sPrevWindowBleProcessMaxUs.store(snapshot.metrics.bleProcessMaxUs, std::memory_order_relaxed);
        sPrevWindowDispPipeMaxUs.store(snapshot.metrics.dispPipeMaxUs, std::memory_order_relaxed);
        resetPerfExtendedWindowPeaks();
    }
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

static void populateFlatSnapshot(PerfSdSnapshot& flat, const RuntimeSnapshotCaptureContext& ctx,
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
    flat.alertPersistStartsSkippedActive = perfCounters.alertPersistStartsSkippedActive.load(std::memory_order_relaxed);
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
    flat.largestDmaMin = (metrics.minLargestDma == UINT32_MAX) ? ctx.largestDma : metrics.minLargestDma;
    flat.bleState = bleClient.getBLEStateCode();
    flat.subscribeStep = bleClient.getSubscribeStepCode();
    flat.connectInProgress = bleClient.isConnectInProgress() ? 1 : 0;
    flat.asyncConnectPending = bleClient.isAsyncConnectPending() ? 1 : 0;
    flat.pendingDisconnectCleanup = bleClient.hasPendingDisconnectCleanup() ? 1 : 0;
    flat.proxyAdvertising = perfGetProxyAdvertisingState() != 0 ? 1 : 0;
    flat.proxyAdvertisingLastTransitionReason = static_cast<uint8_t>(perfGetProxyAdvertisingLastTransitionReason());
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
    flat.speedSelectedMph_x10 = (speedSelectedValid && std::isfinite(selectedSpeedMph) && selectedSpeedMph > 0.0f)
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
        flat.gpsSentencesOk = gpsStatus.sentencesParsed;
        flat.gpsSentencesChecksumFail = gpsStatus.checksumFailures;
        flat.gpsSentencesUnknown = gpsStatus.sentencesUnknown;
        flat.gpsBufferOverruns = gpsStatus.bufferOverruns;
        flat.gpsBytesIn = gpsStatus.bytesRead;
        flat.gpsFirstFixMs = gpsStatus.firstFixMs;
        flat.gpsLastSentenceAgeMs =
            (gpsStatus.lastSentenceTsMs != 0) ? (nowMs - gpsStatus.lastSentenceTsMs) : UINT32_MAX;
        flat.gpsFixAgeMs = gpsStatus.fixAgeMs;
        flat.gpsStableFixAgeMs = gpsStatus.stableFixAgeMs;
        flat.gpsSatellitesInUse = gpsStatus.satellites;
        flat.gpsHdopX10 =
            std::isnan(gpsStatus.hdop) ? UINT16_MAX : static_cast<uint16_t>(gpsStatus.hdop * 10.0f + 0.5f);
        flat.gpsHasFix = gpsStatus.hasFix;
        flat.gpsStableHasFix = gpsStatus.stableHasFix;
        flat.gpsEnableTransitions = gpsStatus.enableTransitions;
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
    flat.minLargestBlock = (metrics.minLargestBlock == UINT32_MAX) ? 0 : metrics.minLargestBlock;

    flat.uiToScanCount = metrics.uiToScanCount;
    flat.uiToRestCount = metrics.uiToRestCount;
    flat.uiScanToRestCount = metrics.uiScanToRestCount;
    flat.uiFastScanExitCount = metrics.uiFastScanExitCount;
    flat.uiLastScanDwellMs = metrics.uiLastScanDwellMs;
    flat.uiMinScanDwellMs = (metrics.uiMinScanDwellMs == UINT32_MAX) ? 0 : metrics.uiMinScanDwellMs;
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

static void populateRuntimeSnapshot(PerfRuntimeMetricsSnapshot& snapshot, const RuntimeSnapshotCaptureContext& ctx,
                                    PerfRuntimeSnapshotMode mode) {
    snapshot = {};
    populateFlatSnapshot(snapshot.flat, ctx, mode);

    snapshot.phoneCmdDrops = ctx.phoneCmdDropMetrics;
    snapshot.uptimeMs = ctx.nowMs;
    snapshot.connectionDispatchRuns = perfCounters.connectionDispatchRuns.load(std::memory_order_relaxed);
    snapshot.connectionCadenceDisplayDue = perfCounters.connectionCadenceDisplayDue.load(std::memory_order_relaxed);
    snapshot.connectionCadenceHoldScanDwell =
        perfCounters.connectionCadenceHoldScanDwell.load(std::memory_order_relaxed);
    snapshot.connectionStateProcessRuns = perfCounters.connectionStateProcessRuns.load(std::memory_order_relaxed);
    snapshot.connectionStateWatchdogForces = perfCounters.connectionStateWatchdogForces.load(std::memory_order_relaxed);
    snapshot.connectionStateProcessGapMaxMs =
        perfCounters.connectionStateProcessGapMaxMs.load(std::memory_order_relaxed);
    snapshot.bleScanStateEntries = perfCounters.bleScanStateEntries.load(std::memory_order_relaxed);
    snapshot.bleScanStateExits = perfCounters.bleScanStateExits.load(std::memory_order_relaxed);
    snapshot.bleScanTargetFound = perfCounters.bleScanTargetFound.load(std::memory_order_relaxed);
    snapshot.bleScanNoTargetExits = perfCounters.bleScanNoTargetExits.load(std::memory_order_relaxed);
    snapshot.bleScanDwellMaxMs = perfCounters.bleScanDwellMaxMs.load(std::memory_order_relaxed);
    snapshot.uuid128FallbackHits = perfCounters.uuid128FallbackHits.load(std::memory_order_relaxed);
    snapshot.wifiStopGraceful = perfCounters.wifiStopGraceful.load(std::memory_order_relaxed);
    snapshot.wifiStopImmediate = perfCounters.wifiStopImmediate.load(std::memory_order_relaxed);
    snapshot.wifiStopManual = perfCounters.wifiStopManual.load(std::memory_order_relaxed);
    snapshot.wifiStopTimeout = perfCounters.wifiStopTimeout.load(std::memory_order_relaxed);
    snapshot.wifiStopNoClients = perfCounters.wifiStopNoClients.load(std::memory_order_relaxed);
    snapshot.wifiStopNoClientsAuto = perfCounters.wifiStopNoClientsAuto.load(std::memory_order_relaxed);
    snapshot.wifiStopLowDma = perfCounters.wifiStopLowDma.load(std::memory_order_relaxed);
    snapshot.wifiStopPoweroff = perfCounters.wifiStopPoweroff.load(std::memory_order_relaxed);
    snapshot.wifiStopOther = perfCounters.wifiStopOther.load(std::memory_order_relaxed);
    snapshot.wifiApDropLowDma = perfCounters.wifiApDropLowDma.load(std::memory_order_relaxed);
    snapshot.wifiApDropIdleSta = perfCounters.wifiApDropIdleSta.load(std::memory_order_relaxed);
    snapshot.wifiApUpTransitions = perfCounters.wifiApUpTransitions.load(std::memory_order_relaxed);
    snapshot.wifiApDownTransitions = perfCounters.wifiApDownTransitions.load(std::memory_order_relaxed);
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
    snapshot.proxyAdvertisingOffTransitions =
        perfCounters.proxyAdvertisingOffTransitions.load(std::memory_order_relaxed);
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
    snapshot.wifiApLastTransitionReason = perfWifiApTransitionReasonName(snapshot.wifiApLastTransitionReasonCode);
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
    snapshot.speedSource.selectedMph =
        snapshot.speedSource.selectedValueValid ? ctx.speedStatus.selectedSpeedMph : 0.0f;
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
    snapshot.connectionCycle.wifiManualPhoneKicksTotal = ctx.connectionCycleWifiManualPhoneKicksTotal;
    snapshot.connectionCycle.proxyNoClientLatched = ctx.connectionCycleProxyNoClientLatched;
}

} // namespace

void captureSdSnapshot(PerfSdSnapshot& snapshot) {
    // loopTask has an 8 KB stack budget. Keep the periodic SD snapshot on the
    // flat-only path so Tier 4 observability work cannot pay for the larger
    // runtime wrapper when only the CSV payload is needed.
    const RuntimeSnapshotCaptureContext ctx = captureRuntimeSnapshotContext();
    populateFlatSnapshot(snapshot, ctx, PerfRuntimeSnapshotMode::CaptureAndResetWindowPeaks);
}

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

void perfCaptureRuntimeMetricsSnapshot(PerfRuntimeMetricsSnapshot& snapshot, PerfRuntimeSnapshotMode mode) {
    const RuntimeSnapshotCaptureContext ctx = captureRuntimeSnapshotContext();
    populateRuntimeSnapshot(snapshot, ctx, mode);
}
