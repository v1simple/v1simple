/**
 * Standalone SD-backed performance CSV logger implementation.
 */

#include "perf_sd_logger.h"

#include "storage_manager.h"
#include "perf_metrics.h"
#include <FS.h>
#include <cstdarg>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_system.h>

#ifndef MALLOC_CAP_DMA
#define MALLOC_CAP_DMA (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

namespace {
static constexpr const char* PERF_DIR_PATH = "/perf";
static constexpr const char* PERF_CSV_PATH_FALLBACK = "/perf/perf.csv";
static constexpr uint32_t PERF_CSV_SCHEMA_VERSION = 45;  // drops unused priority-display-index and proxy-latency telemetry
static constexpr const char* PERF_CSV_HEADER =
    "millis,utc,rx,qDrop,parseOK,parseFail,parseResync,disc,reconn,loopMax_us,bleDrainMax_us,dispMax_us,freeHeap,freeDma,largestDma,freeDmaCap,largestDmaCap,dmaFreeMin,dmaLargestMin,bleProcessMax_us,touchMax_us,wifiMax_us,uiToScan,uiToRest,uiScanToRest,uiFastScanExit,uiLastScanDwellMs,uiMinScanDwellMs,fadeDown,fadeRestore,fadeSkipEqual,fadeSkipNoBaseline,fadeSkipNotFaded,fadeLastDecision,fadeLastCurrentVol,fadeLastOriginalVol,fadeLastDecisionMs,speedVolDrop,speedVolRestore,speedVolRetry,bleScanStartMs,bleTargetFoundMs,bleConnectStartMs,bleConnectedMs,bleFirstRxMs,bleFollowupRequestAlertMax_us,bleFollowupRequestVersionMax_us,bleConnectStableCallbackMax_us,bleProxyStartMax_us,displayGapRecoverMax_us,displayFullRenderCount,displayRestingFullRenderCount,displayRestingIncrementalRenderCount,displayPersistedRenderCount,displayPreviewRenderCount,displayRestoreRenderCount,displayLiveScenarioRenderCount,displayRestingScenarioRenderCount,displayPersistedScenarioRenderCount,displayPreviewScenarioRenderCount,displayRestoreScenarioRenderCount,displayRestingFlushReasonFullRedrawCount,displayRestingFlushReasonPendingExternalCount,displayRestingFlushReasonPaintedCount,displayRestingFlushReasonCacheHitCount,displayPersistedFlushReasonFullRedrawCount,displayPersistedFlushReasonPendingExternalCount,displayPersistedFlushReasonPaintedCount,displayPersistedFlushReasonCacheHitCount,displayStatusVolumePaintCount,displayStatusRssiPaintCount,displayStatusProfilePaintCount,displayStatusBatteryPaintCount,displayStatusBleProxyPaintCount,displayStatusWifiPaintCount,displayStatusObdPaintCount,displayStatusGpsPaintCount,displayStatusAlpPaintCount,displayRedrawReasonFirstRunCount,displayRedrawReasonEnterLiveCount,displayRedrawReasonLeaveLiveCount,displayRedrawReasonLeavePersistedCount,displayRedrawReasonForceRedrawCount,displayRedrawReasonFrequencyChangeCount,displayRedrawReasonBandSetChangeCount,displayRedrawReasonArrowChangeCount,displayRedrawReasonSignalBarChangeCount,displayRedrawReasonVolumeChangeCount,displayRedrawReasonBogeyCounterChangeCount,displayRedrawReasonRssiRefreshCount,displayRedrawReasonFlashTickCount,displayRedrawReasonFullFlushForRedrawCount,displayRedrawReasonCacheHitSkipFlushCount,displayRedrawReasonUnionExceedsCapCount,displayRedrawReasonPartialRegionFlushCount,displayFullFlushCount,displayPartialFlushCount,displayPartialFlushAreaPeakPx,displayPartialFlushAreaTotalPx,displayFlushEquivalentAreaTotalPx,displayFlushMaxAreaPx,displayPartialFlushLogicalWidthPeakPx,displayPartialFlushLogicalHeightPeakPx,displayPartialFlushRowCallsPeak,displayPartialFlushPixelsPerRowPeakPx,displayPartialFlushUsPeak_us,displayPartialFlushWorstUsLogicalWidthPx,displayPartialFlushWorstUsLogicalHeightPx,displayPartialFlushWorstUsAreaPx,displayPartialFlushWouldFullRows64Count,displayPartialFlushWouldFullRows128Count,displayPartialFlushWouldFullRows256Count,displayUnionExceedsCapAreaPeakPx,displayUnionExceedsCapRectCountPeak,displayUnionExceedsCapAreaPeakSourceMask,displayUnionExceedsCapWithFrequencyCount,displayUnionExceedsCapWithBandsBarsCount,displayUnionExceedsCapWithArrowsCount,displayUnionExceedsCapWithStatusCount,displayUnionExceedsCapWithIndicatorsCount,displayUnionExceedsCapWithExternalCount,displayUnionExceedsCapUnclassifiedCount,displayBaseFrameMax_us,displayStatusStripMax_us,displayFrequencyMax_us,displayBandsBarsMax_us,displayArrowsIconsMax_us,displayFlushSubphaseMax_us,displayLiveRenderMax_us,displayRestingRenderMax_us,displayPersistedRenderMax_us,displayPreviewRenderMax_us,displayRestoreRenderMax_us,displayPreviewFirstRenderMax_us,displayPreviewSteadyRenderMax_us,alertPersistStarts,alertPersistStartsSkippedActive,alertPersistStartsSkippedInvalid,alertPersistExpires,alertPersistClears,autoPushStarts,autoPushCompletes,autoPushNoProfile,autoPushProfileLoadFail,autoPushProfileWriteFail,autoPushBusyRetries,autoPushModeFail,autoPushVolumeFail,autoPushDisconnectAbort,powerAutoPowerArmed,powerAutoPowerTimerStart,powerAutoPowerTimerCancel,powerAutoPowerTimerExpire,powerCarModeAlpSilenceExpire,powerCriticalWarn,powerCriticalShutdown,perfUncleanShutdown,cmdBleBusy,rxBytes,oversizeDrops,queueHighWater,bleMutexSkip,bleMutexTimeout,cmdPaceNotYet,bleDiscTaskCreateFail,displayUpdates,displaySkips,wifiConnectDeferred,pushNowRetries,pushNowFailures,minLargestBlock,fsMax_us,sdMax_us,sdWriteCount,sdWriteLt1ms,sdWrite1to5ms,sdWrite5to10ms,sdWriteGe10ms,flushMax_us,bleConnectMax_us,bleDiscoveryMax_us,bleSubscribeMax_us,dispPipeMax_us,perfReportMax_us,prioritySelectRowFlag,prioritySelectFirstUsable,prioritySelectFirstEntry,prioritySelectAmbiguousIndex,prioritySelectUnusableIndex,prioritySelectInvalidChosen,alertTablePublishes,alertTablePublishes3Bogey,alertTableRowReplacements,alertTableAssemblyTimeouts,parserRowsBandNone,parserRowsKuRaw,displayLiveInvalidPrioritySkips,displayLiveFallbackToUsable,obdMax_us,obdConnectCallMax_us,obdSecurityStartCallMax_us,obdDiscoveryCallMax_us,obdSubscribeCallMax_us,obdWriteCallMax_us,obdRssiCallMax_us,obdPollErrors,obdStaleCount,perfDrop,eventBusDrops,wifiHandleClientMax_us,wifiMaintenanceMax_us,wifiStatusCheckMax_us,wifiTimeoutCheckMax_us,wifiHeapGuardMax_us,wifiApStaPollMax_us,wifiStopHttpServerMax_us,wifiStopStaDisconnectMax_us,wifiStopApDisableMax_us,wifiStopModeOffMax_us,wifiStartPreflightMax_us,wifiStartApBringupMax_us,freeDmaMin,largestDmaMin,bleState,subscribeStep,connectInProgress,asyncConnectPending,pendingDisconnectCleanup,proxyAdvertising,proxyAdvertisingLastTransitionReason,wifiPriorityMode,speedSourceSelected,speedSourceValid,speedSelectedMph_x10,speedSelectedAgeMs,speedSourceSwitches,speedNoSourceSelections,speedGpsSelections,cycleState,cycleTransitionsTotal,cycleTimeInStateMs,cycleTeardownDurationMs,cycleObdRetryAttemptsTotal,cycleWifiManualPhoneKicksTotal,cycleProxyNoClientLatched,gpsSentencesOk,gpsSentencesChecksumFail,gpsSentencesUnknown,gpsBufferOverruns,gpsBytesIn,gpsFirstFixMs,gpsLastSentenceAgeMs,gpsFixAgeMs,gpsStableFixAgeMs,gpsSatellitesInUse,gpsHdopX10,gpsHasFix,gpsStableHasFix,gpsEnableTransitions,notifyToDisplayMax_ms,notifyToDisplayTotalCount\n";
static constexpr UBaseType_t PERF_SD_QUEUE_DEPTH = 16;       // Halved from 32 to reclaim ~7 KiB internal SRAM
static constexpr uint32_t PERF_SD_WRITER_STACK_SIZE = 8192;  // Bench high-water leaves ~4 KiB free
static constexpr UBaseType_t PERF_SD_WRITER_PRIORITY = 1;
static constexpr TickType_t PERF_SD_QUEUE_RECEIVE_TIMEOUT_TICKS = pdMS_TO_TICKS(1000);
static constexpr uint16_t PERF_SD_FLUSH_EVERY_ROWS = 3;
static constexpr uint32_t PERF_SD_FLUSH_INTERVAL_MS = 15000;
static constexpr size_t PERF_CSV_LINE_BUFFER_SIZE = 6144;
static constexpr size_t PERF_SD_WRITE_STAGING_SIZE = 512;

static uint16_t countCsvColumns(const char* text, size_t len) {
    if (!text || len == 0) {
        return 0;
    }
    uint16_t columns = 1;
    bool sawContent = false;
    for (size_t i = 0; i < len; ++i) {
        char c = text[i];
        if (c == '\0' || c == '\n' || c == '\r') {
            break;
        }
        sawContent = true;
        if (c == ',') {
            columns++;
        }
    }
    return sawContent ? columns : 0;
}

static uint16_t expectedPerfCsvColumns() {
    static const uint16_t kColumns = countCsvColumns(PERF_CSV_HEADER, strlen(PERF_CSV_HEADER));
    return kColumns;
}

static void buildPerfCsvPath(uint32_t bootId_, uint32_t bootToken, char* out, size_t outLen) {
    if (!out || outLen == 0) {
        return;
    }
    if (bootToken == 0) {
        if (bootId_ != 0) {
            snprintf(out, outLen, "/perf/perf_boot_%lu.csv",
                     static_cast<unsigned long>(bootId_));
        } else {
            snprintf(out, outLen, "%s", PERF_CSV_PATH_FALLBACK);
        }
        return;
    }
    snprintf(out, outLen, "/perf/perf_boot_%lu-%08lx.csv",
             static_cast<unsigned long>(bootId_),
             static_cast<unsigned long>(bootToken));
}

static bool appendCsvFormat(char* buffer, size_t bufferLen, size_t& offset, const char* fmt, ...) {
    if (!buffer || offset >= bufferLen) {
        return false;
    }

    va_list args;
    va_start(args, fmt);
    const int written = vsnprintf(buffer + offset, bufferLen - offset, fmt, args);
    va_end(args);

    if (written <= 0 || static_cast<size_t>(written) >= (bufferLen - offset)) {
        return false;
    }

    offset += static_cast<size_t>(written);
    return true;
}

static bool appendCsvUInt32(char* buffer, size_t bufferLen, size_t& offset, uint32_t value) {
    return appendCsvFormat(buffer, bufferLen, offset, "%lu,", static_cast<unsigned long>(value));
}

static bool appendCsvUInt8(char* buffer, size_t bufferLen, size_t& offset, uint8_t value) {
    return appendCsvFormat(buffer, bufferLen, offset, "%u,", static_cast<unsigned int>(value));
}

static bool appendCsvUInt32Last(char* buffer, size_t bufferLen, size_t& offset, uint32_t value) {
    return appendCsvFormat(buffer, bufferLen, offset, "%lu\n", static_cast<unsigned long>(value));
}

static bool appendCsvUInt16(char* buffer, size_t bufferLen, size_t& offset, uint16_t value) {
    return appendCsvFormat(buffer, bufferLen, offset, "%u,", static_cast<unsigned int>(value));
}

// UTC field: YYYY-MM-DDTHH:MM:SS.sssZ or empty field (followed by comma)
static bool appendCsvUtcField(char* buffer, size_t bufferLen, size_t& offset,
                              uint64_t utcEpochMs, bool valid) {
    if (!valid || utcEpochMs == 0) {
        return appendCsvFormat(buffer, bufferLen, offset, ",");
    }
    const uint64_t totalSec = utcEpochMs / 1000;
    const uint32_t ms       = static_cast<uint32_t>(utcEpochMs % 1000);
    // Simple calendar decomposition (sufficient for logging, no leap-second handling)
    uint32_t sec  = static_cast<uint32_t>(totalSec % 60);
    uint32_t min  = static_cast<uint32_t>((totalSec / 60) % 60);
    uint32_t hour = static_cast<uint32_t>((totalSec / 3600) % 24);
    uint32_t days = static_cast<uint32_t>(totalSec / 86400); // days since 1970-01-01
    // Gregorian calendar
    uint32_t y = 1970;
    while (true) {
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        uint32_t diy = leap ? 366u : 365u;
        if (days < diy) break;
        days -= diy;
        y++;
    }
    static const uint8_t mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    uint32_t mo = 1;
    for (uint32_t m = 0; m < 12; m++) {
        uint32_t md = mdays[m] + ((m == 1 && leap) ? 1u : 0u);
        if (days < md) { mo = m + 1; break; }
        days -= md;
    }
    return appendCsvFormat(buffer, bufferLen, offset,
        "%04lu-%02lu-%02luT%02lu:%02lu:%02lu.%03luZ,",
        static_cast<unsigned long>(y),
        static_cast<unsigned long>(mo),
        static_cast<unsigned long>(days + 1),
        static_cast<unsigned long>(hour),
        static_cast<unsigned long>(min),
        static_cast<unsigned long>(sec),
        static_cast<unsigned long>(ms));
}
}  // namespace

PerfSdLogger perfSdLogger;

#ifdef UNIT_TEST
PerfSdLogger::~PerfSdLogger() {
    releaseForTest();
}

void PerfSdLogger::releaseForTest() {
    enabled_ = false;
    if (persistentFile_) {
        persistentFile_.close();
    }
    if (queue_) {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
    if (queueAllocation_.queueBuffer) {
        heap_caps_free(queueAllocation_.queueBuffer);
        queueAllocation_.queueBuffer = nullptr;
    }
    if (csvLineBuffer_) {
        heap_caps_free(csvLineBuffer_);
        csvLineBuffer_ = nullptr;
    }
    if (writeStagingBuffer_) {
        heap_caps_free(writeStagingBuffer_);
        writeStagingBuffer_ = nullptr;
    }
    writerTask_ = nullptr;
    queueInPsram_ = false;
    writerTaskStackInPsram_ = false;
    perfDirReady_ = false;
    csvHeaderReady_ = false;
    sessionMarkerPending_ = false;
    pendingWrites_.store(0, std::memory_order_relaxed);
}
#endif

void PerfSdLogger::setBootId(uint32_t id, uint32_t bootToken) {
    bootId_ = id;
    bootToken_ = bootToken;
    buildPerfCsvPath(bootId_, bootToken_, csvPathBuf_, sizeof(csvPathBuf_));
    csvHeaderReady_ = false;
    sessionMarkerPending_ = true;
    rowsSinceFlush_ = 0;
    lastFlushMs_ = 0;
    // Path may have changed; force a reopen on the next write.
    if (persistentFile_) {
        StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
        if (lock) {
            persistentFile_.close();
        }
    }
}

void PerfSdLogger::begin(bool sdAvailable) {
    enabled_ = false;
    if (!sdAvailable) {
        return;
    }

    if (csvPathBuf_[0] == '\0') {
        setBootId(bootId_, bootToken_);
    }

    // Reset cached file state for each runtime session and emit a marker on first write.
    perfDirReady_ = false;
    csvHeaderReady_ = false;
    sessionMarkerPending_ = true;
    rowsSinceFlush_ = 0;
    lastFlushMs_ = 0;
    sessionStartMs_ = millis();
    sessionToken_ = static_cast<uint32_t>(esp_random());
    sessionSeq_++;

    if (!ensureCsvBuffers()) {
        return;
    }

    if (!queue_) {
        queue_ = createQueuePreferPsram(PERF_SD_QUEUE_DEPTH,
                                       sizeof(PerfSdSnapshot),
                                       queueAllocation_,
                                       &queueInPsram_);
        if (!queue_) {
            Serial.println("[Perf] ERROR: Failed to create SD logger queue");
            return;
        }
        if (!queueInPsram_) {
            Serial.println("[Perf] WARN: SD logger queue using internal SRAM fallback");
        }
    }

    if (!writerTask_) {
        BaseType_t rc = createTaskPinnedToCoreInternalStack(writerTaskEntry,
                                                            "PerfSdWriter",
                                                            PERF_SD_WRITER_STACK_SIZE,
                                                            this,
                                                            PERF_SD_WRITER_PRIORITY,
                                                            &writerTask_,
                                                            0);
        if (rc != pdPASS) {
            Serial.println("[Perf] ERROR: Failed to create SD logger task");
            return;
        }
    }

    enabled_ = true;
}

bool PerfSdLogger::enqueue(const PerfSdSnapshot& snapshot) {
    if (!enabled_ || !queue_) {
        return false;
    }
    pendingWrites_.fetch_add(1, std::memory_order_relaxed);
    if (xQueueSend(queue_, &snapshot, 0) != pdTRUE) {
        pendingWrites_.fetch_sub(1, std::memory_order_relaxed);
        PERF_INC(perfDrop);
        return false;
    }
    return true;
}

#ifndef UNIT_TEST
uint32_t PerfSdLogger::writerStackHighWaterBytes() const {
    TaskHandle_t task = writerTask_;
    if (!task) {
        return 0;
    }
    return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(task));
}
#endif

void PerfSdLogger::startNewSession() {
    if (!enabled_) {
        return;
    }
    perfMetricsResetSessionWindow();
    // Force next write to emit a fresh header + session marker.
    csvHeaderReady_ = false;
    sessionMarkerPending_ = true;
    rowsSinceFlush_ = 0;
    lastFlushMs_ = 0;
    sessionStartMs_ = millis();
    sessionToken_ = static_cast<uint32_t>(esp_random());
    sessionSeq_++;
}

void PerfSdLogger::writerTaskEntry(void* param) {
    PerfSdLogger* self = static_cast<PerfSdLogger*>(param);
    self->writerTaskLoop();
}

bool PerfSdLogger::receiveSnapshot(PerfSdSnapshot& snapshot, TickType_t timeoutTicks) {
    if (!queue_) {
        return false;
    }
    return xQueueReceive(queue_, &snapshot, timeoutTicks) == pdTRUE;
}

void PerfSdLogger::writerTaskLoop() {
    while (true) {
        PerfSdSnapshot snapshot{};
        if (!receiveSnapshot(snapshot, PERF_SD_QUEUE_RECEIVE_TIMEOUT_TICKS)) {
            continue;
        }
        appendSnapshotLine(snapshot);
        pendingWrites_.fetch_sub(1, std::memory_order_relaxed);
        taskYIELD();
    }
}

bool PerfSdLogger::ensurePerfDir(fs::FS& fs) {
    if (perfDirReady_) {
        return true;
    }
    if (fs.mkdir(PERF_DIR_PATH) || fs.exists(PERF_DIR_PATH)) {
        perfDirReady_ = true;
        return true;
    }
    PERF_INC(perfSdDirFail);
    return false;
}

bool PerfSdLogger::ensureCsvBuffers() {
    if (!csvLineBuffer_) {
        csvLineBuffer_ = static_cast<char*>(
            heap_caps_malloc(PERF_CSV_LINE_BUFFER_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
        if (!csvLineBuffer_) {
            Serial.println("[Perf] ERROR: Failed to allocate SD CSV line buffer in PSRAM");
            return false;
        }
    }

    if (!writeStagingBuffer_) {
        writeStagingBuffer_ = static_cast<uint8_t*>(
            heap_caps_malloc(PERF_SD_WRITE_STAGING_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
        if (!writeStagingBuffer_) {
            Serial.println("[Perf] ERROR: Failed to allocate SD CSV write staging buffer");
            heap_caps_free(csvLineBuffer_);
            csvLineBuffer_ = nullptr;
            return false;
        }
    }

    return true;
}

bool PerfSdLogger::writeStaged(File& f, const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        return len == 0;
    }
    if (!writeStagingBuffer_) {
        return false;
    }

    size_t offset = 0;
    while (offset < len) {
        const size_t remaining = len - offset;
        const size_t chunkLen =
            (remaining > PERF_SD_WRITE_STAGING_SIZE) ? PERF_SD_WRITE_STAGING_SIZE : remaining;
        memcpy(writeStagingBuffer_, data + offset, chunkLen);
        const size_t written = f.write(writeStagingBuffer_, chunkLen);
        if (written != chunkLen) {
            return false;
        }
        offset += chunkLen;
    }
    return true;
}

bool PerfSdLogger::writeSessionMarker(File& f) {
    char marker[128];
    int n = snprintf(
        marker,
        sizeof(marker),
        "#session_start,seq=%lu,bootId_=%lu,uptime_ms=%lu,token=%08lX,schema=%lu\n",
        static_cast<unsigned long>(sessionSeq_),
        static_cast<unsigned long>(bootId_),
        static_cast<unsigned long>(sessionStartMs_),
        static_cast<unsigned long>(sessionToken_),
        static_cast<unsigned long>(PERF_CSV_SCHEMA_VERSION));
    if (n <= 0 || n >= static_cast<int>(sizeof(marker))) {
        return false;
    }
    size_t markerLen = static_cast<size_t>(n);
    return writeStaged(f, reinterpret_cast<const uint8_t*>(marker), markerLen);
}

bool PerfSdLogger::ensureCsvHeaderAndSessionMarker(File& f) {
    // If the file was rotated/deleted while running, size 0 means header must be rewritten.
    if (f.size() == 0) {
        csvHeaderReady_ = false;
    }

    bool metadataWritten = false;
    if (!csvHeaderReady_) {
        size_t headerLen = strlen(PERF_CSV_HEADER);
        if (!writeStaged(f, reinterpret_cast<const uint8_t*>(PERF_CSV_HEADER), headerLen)) {
            PERF_INC(perfSdHeaderFail);
            return false;
        }
        metadataWritten = true;
        csvHeaderReady_ = true;
    }

    if (sessionMarkerPending_) {
        if (!writeSessionMarker(f)) {
            PERF_INC(perfSdMarkerFail);
            return false;
        }
        metadataWritten = true;
        sessionMarkerPending_ = false;
    }

    if (metadataWritten && !flushPersistentFile(f)) {
        return false;
    }

    return true;
}

bool PerfSdLogger::flushPersistentFile(File& f) {
    f.flush();
    rowsSinceFlush_ = 0;
    lastFlushMs_ = millis();
    return true;
}

bool PerfSdLogger::flushPersistentFileIfDue(File& f) {
    if (rowsSinceFlush_ == 0) {
        return true;
    }

    const uint32_t nowMs = millis();
    if (lastFlushMs_ == 0) {
        lastFlushMs_ = nowMs;
    }

    if (rowsSinceFlush_ < PERF_SD_FLUSH_EVERY_ROWS &&
        static_cast<uint32_t>(nowMs - lastFlushMs_) < PERF_SD_FLUSH_INTERVAL_MS) {
        return true;
    }

    return flushPersistentFile(f);
}

bool PerfSdLogger::appendSnapshotLine(const PerfSdSnapshot& snapshot) {
    uint32_t startUs = PERF_TIMESTAMP_US();

    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return false;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        return false;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) {
        PERF_INC(perfSdLockFail);
        return false;
    }

    if (!ensurePerfDir(*fs)) {
        return false;
    }

    const char* csvPath = (csvPathBuf_[0] != '\0') ? csvPathBuf_ : PERF_CSV_PATH_FALLBACK;

    // Persistent handle: open once, keep open across rows. Eliminates the per-row
    // FAT EOF walk + dirent rewrite that dominates flush_max_peak_us on the slower
    // FATFS path in IDF 5.5.1+. Data flushes are batched; metadata/session markers
    // and shutdown drain still force a flush boundary.
    if (!persistentFile_) {
        persistentFile_ = fs->open(csvPath, FILE_APPEND, true);
        if (!persistentFile_ && perfDirReady_) {
            // Directory can be removed while running; invalidate cache and retry once.
            perfDirReady_ = false;
            if (ensurePerfDir(*fs)) {
                persistentFile_ = fs->open(csvPath, FILE_APPEND, true);
            }
        }
        if (!persistentFile_) {
            PERF_INC(perfSdOpenFail);
            return false;
        }
    }

    if (!csvLineBuffer_ || !writeStagingBuffer_) {
        PERF_INC(perfSdWriteFail);
        persistentFile_.close();
        return false;
    }

    if (!ensureCsvHeaderAndSessionMarker(persistentFile_)) {
        persistentFile_.close();
        return false;
    }

    // Single-consumer writer task; format the large CSV row in PSRAM, then
    // write it through a small internal/DMA-capable staging buffer.
    char* line = csvLineBuffer_;
    const size_t lineBufferLen = PERF_CSV_LINE_BUFFER_SIZE;
    size_t offset = 0;
    const bool ok =
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.millisTs) &&
        appendCsvUtcField(line, lineBufferLen, offset, snapshot.utcEpochMs, snapshot.utcValid) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.rx) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.qDrop) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.parseOk) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.parseFail) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.parseResync) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.disc) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.reconn) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.loopMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleDrainMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.dispMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.freeHeap) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.freeDma) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.largestDma) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.freeDmaCap) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.largestDmaCap) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.dmaFreeMin) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.dmaLargestMin) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleProcessMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.touchMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.wifiMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.uiToScanCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.uiToRestCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.uiScanToRestCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.uiFastScanExitCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.uiLastScanDwellMs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.uiMinScanDwellMs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.fadeDownCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.fadeRestoreCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.fadeSkipEqualCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.fadeSkipNoBaselineCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.fadeSkipNotFadedCount) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.fadeLastDecision) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.fadeLastCurrentVol) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.fadeLastOriginalVol) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.fadeLastDecisionMs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.speedVolDropCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.speedVolRestoreCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.speedVolRetryCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleScanStartMs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleTargetFoundMs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleConnectStartMs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleConnectedMs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleFirstRxMs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleFollowupRequestAlertMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleFollowupRequestVersionMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleConnectStableCallbackMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleProxyStartMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayGapRecoverMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayFullRenderCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRestingFullRenderCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRestingIncrementalRenderCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPersistedRenderCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPreviewRenderCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRestoreRenderCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayLiveScenarioRenderCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRestingScenarioRenderCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPersistedScenarioRenderCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPreviewScenarioRenderCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRestoreScenarioRenderCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRestingFlushReasonFullRedrawCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRestingFlushReasonPendingExternalCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRestingFlushReasonPaintedCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRestingFlushReasonCacheHitCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPersistedFlushReasonFullRedrawCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPersistedFlushReasonPendingExternalCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPersistedFlushReasonPaintedCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPersistedFlushReasonCacheHitCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayStatusVolumePaintCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayStatusRssiPaintCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayStatusProfilePaintCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayStatusBatteryPaintCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayStatusBleProxyPaintCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayStatusWifiPaintCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayStatusObdPaintCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayStatusGpsPaintCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayStatusAlpPaintCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRedrawReasonFirstRunCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRedrawReasonEnterLiveCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRedrawReasonLeaveLiveCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRedrawReasonLeavePersistedCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRedrawReasonForceRedrawCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRedrawReasonFrequencyChangeCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRedrawReasonBandSetChangeCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRedrawReasonArrowChangeCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRedrawReasonSignalBarChangeCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRedrawReasonVolumeChangeCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRedrawReasonBogeyCounterChangeCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRedrawReasonRssiRefreshCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRedrawReasonFlashTickCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRedrawReasonFullFlushForRedrawCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRedrawReasonCacheHitSkipFlushCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRedrawReasonUnionExceedsCapCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRedrawReasonPartialRegionFlushCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayFullFlushCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPartialFlushCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPartialFlushAreaPeakPx) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPartialFlushAreaTotalPx) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayFlushEquivalentAreaTotalPx) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayFlushMaxAreaPx) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPartialFlushLogicalWidthPeakPx) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPartialFlushLogicalHeightPeakPx) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPartialFlushRowCallsPeak) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPartialFlushPixelsPerRowPeakPx) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPartialFlushUsPeak) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPartialFlushWorstUsLogicalWidthPx) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPartialFlushWorstUsLogicalHeightPx) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPartialFlushWorstUsAreaPx) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPartialFlushWouldFullRows64Count) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPartialFlushWouldFullRows128Count) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPartialFlushWouldFullRows256Count) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayUnionExceedsCapAreaPeakPx) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayUnionExceedsCapRectCountPeak) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayUnionExceedsCapAreaPeakSourceMask) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayUnionExceedsCapWithFrequencyCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayUnionExceedsCapWithBandsBarsCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayUnionExceedsCapWithArrowsCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayUnionExceedsCapWithStatusCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayUnionExceedsCapWithIndicatorsCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayUnionExceedsCapWithExternalCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayUnionExceedsCapUnclassifiedCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayBaseFrameMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayStatusStripMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayFrequencyMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayBandsBarsMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayArrowsIconsMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayFlushSubphaseMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayLiveRenderMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRestingRenderMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPersistedRenderMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPreviewRenderMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayRestoreRenderMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPreviewFirstRenderMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayPreviewSteadyRenderMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.alertPersistStarts) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.alertPersistStartsSkippedActive) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.alertPersistStartsSkippedInvalid) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.alertPersistExpires) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.alertPersistClears) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.autoPushStarts) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.autoPushCompletes) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.autoPushNoProfile) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.autoPushProfileLoadFail) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.autoPushProfileWriteFail) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.autoPushBusyRetries) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.autoPushModeFail) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.autoPushVolumeFail) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.autoPushDisconnectAbort) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.powerAutoPowerArmed) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.powerAutoPowerTimerStart) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.powerAutoPowerTimerCancel) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.powerAutoPowerTimerExpire) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.powerCarModeAlpSilenceExpire) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.powerCriticalWarn) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.powerCriticalShutdown) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.perfUncleanShutdown) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.cmdBleBusy) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.rxBytes) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.oversizeDrops) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.queueHighWater) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleMutexSkip) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleMutexTimeout) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.cmdPaceNotYet) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleDiscTaskCreateFail) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayUpdates) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displaySkips) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.wifiConnectDeferred) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.pushNowRetries) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.pushNowFailures) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.minLargestBlock) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.fsMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.sdMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.sdWriteCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.sdWriteLt1msCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.sdWrite1to5msCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.sdWrite5to10msCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.sdWriteGe10msCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.flushMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleConnectMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleDiscoveryMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleSubscribeMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.dispPipeMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.perfReportMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.prioritySelectRowFlag) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.prioritySelectFirstUsable) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.prioritySelectFirstEntry) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.prioritySelectAmbiguousIndex) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.prioritySelectUnusableIndex) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.prioritySelectInvalidChosen) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.alertTablePublishes) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.alertTablePublishes3Bogey) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.alertTableRowReplacements) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.alertTableAssemblyTimeouts) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.parserRowsBandNone) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.parserRowsKuRaw) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayLiveInvalidPrioritySkips) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.displayLiveFallbackToUsable) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.obdMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.obdConnectCallMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.obdSecurityStartCallMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.obdDiscoveryCallMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.obdSubscribeCallMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.obdWriteCallMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.obdRssiCallMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.obdPollErrors) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.obdStaleCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.perfDrop) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.eventBusDrops) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.wifiHandleClientMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.wifiMaintenanceMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.wifiStatusCheckMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.wifiTimeoutCheckMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.wifiHeapGuardMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.wifiApStaPollMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.wifiStopHttpServerMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.wifiStopStaDisconnectMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.wifiStopApDisableMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.wifiStopModeOffMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.wifiStartPreflightMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.wifiStartApBringupMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.freeDmaMin) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.largestDmaMin) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.bleState) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.subscribeStep) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.connectInProgress) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.asyncConnectPending) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.pendingDisconnectCleanup) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.proxyAdvertising) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.proxyAdvertisingLastTransitionReason) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.wifiPriorityMode) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.speedSourceSelected) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.speedSourceValid) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.speedSelectedMph_x10) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.speedSelectedAgeMs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.speedSourceSwitches) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.speedNoSourceSelections) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.speedGpsSelections) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.cycleState) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.cycleTransitionsTotal) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.cycleTimeInStateMs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.cycleTeardownDurationMs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.cycleObdRetryAttemptsTotal) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.cycleWifiManualPhoneKicksTotal) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.cycleProxyNoClientLatched) &&
        // GPS observability (schema v37)
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.gpsSentencesOk) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.gpsSentencesChecksumFail) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.gpsSentencesUnknown) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.gpsBufferOverruns) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.gpsBytesIn) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.gpsFirstFixMs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.gpsLastSentenceAgeMs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.gpsFixAgeMs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.gpsStableFixAgeMs) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.gpsSatellitesInUse) &&
        appendCsvUInt16(line, lineBufferLen, offset, snapshot.gpsHdopX10) &&
        appendCsvUInt8(line, lineBufferLen, offset, static_cast<uint8_t>(snapshot.gpsHasFix ? 1 : 0)) &&
        appendCsvUInt8(line, lineBufferLen, offset, static_cast<uint8_t>(snapshot.gpsStableHasFix ? 1 : 0)) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.gpsEnableTransitions) &&
        // Notify-to-display latency histogram (schema v38)
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.notifyToDisplayMaxMs) &&
        appendCsvUInt32Last(line, lineBufferLen, offset, snapshot.notifyToDisplayTotalCount);

    if (!ok) {
        persistentFile_.close();
        return false;
    }
    const size_t lineLen = offset;
    const uint16_t expectedColumns = expectedPerfCsvColumns();
    const uint16_t lineColumns = countCsvColumns(line, lineLen);
    if (expectedColumns == 0 || lineColumns != expectedColumns) {
        PERF_INC(perfSdWriteFail);
        persistentFile_.close();
        return false;
    }

    if (!writeStaged(persistentFile_, reinterpret_cast<const uint8_t*>(line), lineLen)) {
        PERF_INC(perfSdWriteFail);
        persistentFile_.close();
        return false;
    }
    rowsSinceFlush_++;
    if (!flushPersistentFileIfDue(persistentFile_)) {
        PERF_INC(perfSdWriteFail);
        persistentFile_.close();
        return false;
    }

    perfRecordSdFlushUs(PERF_TIMESTAMP_US() - startUs);
    return true;
}

void PerfSdLogger::drainAndClose(uint32_t timeoutMs) {
    if (!enabled_ || !queue_) {
        return;
    }

    Serial.println("[PerfSdLogger] Draining queue...");

    uint32_t startMs = millis();
    while (pendingWrites_.load(std::memory_order_relaxed) > 0 || uxQueueMessagesWaiting(queue_) > 0) {
        if (millis() - startMs > timeoutMs) {
            Serial.printf("[PerfSdLogger] Drain timeout after %lums, %lu items remaining, %lu writes pending\n",
                         timeoutMs,
                         static_cast<unsigned long>(uxQueueMessagesWaiting(queue_)),
                         static_cast<unsigned long>(pendingWrites_.load(std::memory_order_relaxed)));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    Serial.println("[PerfSdLogger] Drain complete");

    // Release the persistent file handle so the SD card sees a clean dirent on shutdown.
    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (lock && persistentFile_) {
        flushPersistentFile(persistentFile_);
        persistentFile_.close();
    }
}

bool PerfSdLogger::tryDrainAndClose() {
    if (!enabled_ || !queue_) {
        return true;
    }

    if (pendingWrites_.load(std::memory_order_relaxed) > 0 || uxQueueMessagesWaiting(queue_) > 0) {
        return false;
    }

    StorageManager::SDTryLock lock(storageManager.getSDMutex());
    if (!lock) {
        return false;
    }

    if (persistentFile_) {
        flushPersistentFile(persistentFile_);
        persistentFile_.close();
    }
    return true;
}
