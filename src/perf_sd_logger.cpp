/**
 * Standalone SD-backed performance CSV logger implementation.
 */

#include "perf_sd_logger.h"

#include "storage_manager.h"
#include "perf_metrics.h"
#include "qualification_clock.h"
#include <FS.h>
#include <cstdarg>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_system.h>
#ifndef UNIT_TEST
#include <esp_vfs_fat.h>
#endif

#ifndef MALLOC_CAP_DMA
#define MALLOC_CAP_DMA (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

namespace {
static constexpr const char* PERF_DIR_PATH = "/perf";
static constexpr const char* PERF_CSV_PATH_FALLBACK = "/perf/perf.csv";
static constexpr uint32_t PERF_CSV_SCHEMA_VERSION = 51; // drops the largest-block watermark (sampled PSRAM)
static constexpr char PERF_CSV_HEADER[] =
    "millis,utc,rx,qDrop,parseOK,parseFail,parseResync,disc,reconn,loopMax_us,bleDrainMax_us,dispMax_us,freeHeap,"
    "freeDmaCap,largestDmaCap,dmaFreeMin,dmaLargestMin,bleProcessMax_us,touchMax_us,uiToScan,uiToRest,uiScanToRest,"
    "uiFastScanExit,uiLastScanDwellMs,uiMinScanDwellMs,fadeDown,fadeRestore,fadeSkipEqual,fadeSkipNoBaseline,"
    "fadeSkipNotFaded,fadeLastDecision,fadeLastCurrentVol,fadeLastOriginalVol,fadeLastDecisionMs,speedVolDrop,"
    "speedVolRestore,speedVolRetry,bleScanStartMs,bleTargetFoundMs,bleConnectStartMs,bleConnectedMs,bleFirstRxMs,"
    "bleFollowupRequestAlertMax_us,bleFollowupRequestVersionMax_us,bleConnectStableCallbackMax_us,"
    "bleProxyStartMax_us,displayFullRenderCount,displayRestingFullRenderCount,displayRestingIncrementalRenderCount,"
    "displayPersistedRenderCount,displayPreviewRenderCount,displayRestoreRenderCount,displayLiveScenarioRenderCount,"
    "displayRestingScenarioRenderCount,displayPersistedScenarioRenderCount,displayPreviewScenarioRenderCount,"
    "displayRestoreScenarioRenderCount,displayRestingFlushReasonFullRedrawCount,"
    "displayRestingFlushReasonPendingExternalCount,displayRestingFlushReasonPaintedCount,"
    "displayRestingFlushReasonCacheHitCount,displayPersistedFlushReasonFullRedrawCount,"
    "displayPersistedFlushReasonPendingExternalCount,displayPersistedFlushReasonPaintedCount,"
    "displayPersistedFlushReasonCacheHitCount,displayStatusVolumePaintCount,displayStatusRssiPaintCount,"
    "displayStatusProfilePaintCount,displayStatusBatteryPaintCount,displayStatusBleProxyPaintCount,"
    "displayStatusWifiPaintCount,displayStatusObdPaintCount,displayStatusGpsPaintCount,displayStatusAlpPaintCount,"
    "displayRedrawReasonFirstRunCount,displayRedrawReasonEnterLiveCount,displayRedrawReasonLeaveLiveCount,"
    "displayRedrawReasonLeavePersistedCount,displayRedrawReasonForceRedrawCount,"
    "displayRedrawReasonFrequencyChangeCount,displayRedrawReasonBandSetChangeCount,"
    "displayRedrawReasonArrowChangeCount,displayRedrawReasonSignalBarChangeCount,"
    "displayRedrawReasonVolumeChangeCount,displayRedrawReasonBogeyCounterChangeCount,"
    "displayRedrawReasonRssiRefreshCount,displayRedrawReasonFlashTickCount,"
    "displayRedrawReasonFullFlushForRedrawCount,displayRedrawReasonCacheHitSkipFlushCount,"
    "displayRedrawReasonUnionExceedsCapCount,displayRedrawReasonPartialRegionFlushCount,displayFullFlushCount,"
    "displayPartialFlushCount,displayPartialFlushAreaPeakPx,displayPartialFlushAreaTotalPx,"
    "displayFlushEquivalentAreaTotalPx,displayFlushMaxAreaPx,displayPartialFlushLogicalWidthPeakPx,"
    "displayPartialFlushLogicalHeightPeakPx,displayPartialFlushRowCallsPeak,displayPartialFlushPixelsPerRowPeakPx,"
    "displayPartialFlushUsPeak_us,displayPartialFlushWorstUsLogicalWidthPx,displayPartialFlushWorstUsLogicalHeightPx,"
    "displayPartialFlushWorstUsAreaPx,displayPartialFlushWouldFullRows64Count,"
    "displayPartialFlushWouldFullRows128Count,displayPartialFlushWouldFullRows256Count,"
    "displayUnionExceedsCapAreaPeakPx,displayUnionExceedsCapRectCountPeak,displayUnionExceedsCapAreaPeakSourceMask,"
    "displayUnionExceedsCapWithFrequencyCount,displayUnionExceedsCapWithBandsBarsCount,"
    "displayUnionExceedsCapWithArrowsCount,displayUnionExceedsCapWithStatusCount,"
    "displayUnionExceedsCapWithIndicatorsCount,displayUnionExceedsCapWithExternalCount,"
    "displayUnionExceedsCapUnclassifiedCount,displayBaseFrameMax_us,displayStatusStripMax_us,displayFrequencyMax_us,"
    "displayBandsBarsMax_us,displayArrowsIconsMax_us,displayFlushSubphaseMax_us,displayLiveRenderMax_us,"
    "displayRestingRenderMax_us,displayPersistedRenderMax_us,displayPreviewRenderMax_us,displayRestoreRenderMax_us,"
    "displayPreviewFirstRenderMax_us,displayPreviewSteadyRenderMax_us,alertPersistStarts,"
    "alertPersistStartsSkippedActive,alertPersistStartsSkippedInvalid,alertPersistExpires,alertPersistClears,"
    "autoPushStarts,autoPushCompletes,autoPushNoProfile,autoPushProfileLoadFail,autoPushProfileWriteFail,"
    "autoPushBusyRetries,autoPushModeFail,autoPushVolumeFail,autoPushDisconnectAbort,powerAutoPowerArmed,"
    "powerAutoPowerTimerStart,powerAutoPowerTimerCancel,powerAutoPowerTimerExpire,powerCarModeAlpSilenceExpire,"
    "powerCriticalWarn,powerCriticalShutdown,perfUncleanShutdown,cmdBleBusy,rxBytes,oversizeDrops,queueHighWater,"
    "bleMutexSkip,bleMutexTimeout,cmdPaceNotYet,bleDiscTaskCreateFail,displayUpdates,displaySkips,pushNowRetries,"
    "pushNowFailures,fsMax_us,sdMax_us,sdWriteCount,sdWriteLt1ms,sdWrite1to5ms,sdWrite5to10ms,"
    "sdWriteGe10ms,flushMax_us,bleConnectMax_us,bleDiscoveryMax_us,bleSubscribeMax_us,dispPipeMax_us,"
    "perfReportMax_us,prioritySelectRowFlag,prioritySelectFirstUsable,prioritySelectFirstEntry,"
    "prioritySelectAmbiguousIndex,prioritySelectUnusableIndex,prioritySelectInvalidChosen,alertTablePublishes,"
    "alertTablePublishes3Bogey,alertTableRowReplacements,alertTableAssemblyTimeouts,parserRowsBandNone,"
    "parserRowsKuRaw,displayLiveInvalidPrioritySkips,displayLiveFallbackToUsable,obdMax_us,obdConnectCallMax_us,"
    "obdSecurityStartCallMax_us,obdDiscoveryCallMax_us,obdWriteCallMax_us,obdRssiCallMax_us,obdPollErrors,"
    "obdStaleCount,perfDrop,bleState,subscribeStep,connectInProgress,asyncConnectPending,pendingDisconnectCleanup,"
    "proxyAdvertising,proxyAdvertisingLastTransitionReason,speedSourceSelected,speedSourceValid,speedSelectedMph_x10,"
    "speedSelectedAgeMs,speedSourceSwitches,speedNoSourceSelections,speedGpsSelections,cycleState,"
    "cycleTransitionsTotal,cycleTimeInStateMs,cycleTeardownDurationMs,cycleObdRetryAttemptsTotal,"
    "cycleWifiManualPhoneKicksTotal,cycleProxyNoClientLatched,gpsSentencesOk,gpsSentencesChecksumFail,"
    "gpsSentencesUnknown,gpsBufferOverruns,gpsBytesIn,gpsFirstFixMs,gpsLastSentenceAgeMs,gpsFixAgeMs,"
    "gpsStableFixAgeMs,gpsSatellitesInUse,gpsHdopX10,gpsHasFix,gpsStableHasFix,gpsEnableTransitions,"
    "notifyToDisplayPipelineCompleteMax_ms,notifyToDisplayPipelineCompleteTotalCount,v1AllVolumeParsed,"
    "dutMicros,clockSegment\n";
static constexpr UBaseType_t PERF_SD_QUEUE_DEPTH = 16;      // Halved from 32 to reclaim ~7 KiB internal SRAM
static constexpr uint32_t PERF_SD_WRITER_STACK_SIZE = 8192; // Bench high-water leaves ~4 KiB free
static constexpr UBaseType_t PERF_SD_WRITER_PRIORITY = 1;
static constexpr TickType_t PERF_SD_QUEUE_RECEIVE_TIMEOUT_TICKS = pdMS_TO_TICKS(1000);
static constexpr uint16_t PERF_SD_FLUSH_EVERY_ROWS = 1;
static constexpr uint32_t PERF_SD_FLUSH_INTERVAL_MS = 15000;
static constexpr size_t PERF_CSV_LINE_BUFFER_SIZE = 6656;
static constexpr size_t PERF_SD_WRITE_STAGING_SIZE = 512;
static constexpr size_t PERF_SD_SESSION_MARKER_BUFFER_SIZE = 192;
// One boot file can run for hours, so this reserve is an optimization rather
// than a hard cap. Once it is consumed, the same r+ handle grows normally and
// the allocation cost remains inside appendSnapshotLine()'s latency sample.
static constexpr size_t PERF_SD_CONTIGUOUS_RESERVE_SIZE = 1024 * 1024;
static constexpr size_t PERF_SD_RESERVE_YIELD_EVERY_BYTES = 32 * 1024;
static constexpr const char* PERF_SD_RESERVE_TEMP_PATH = "/perf/.perf_reserve.tmp";
static constexpr const char* PERF_SD_READ_WRITE_MODE = "r+";
static_assert((sizeof(PERF_CSV_HEADER) - 1) + PERF_SD_SESSION_MARKER_BUFFER_SIZE <= PERF_CSV_LINE_BUFFER_SIZE,
              "CSV line buffer must fit an adjacent header and maximum session marker");

#ifndef UNIT_TEST
static bool buildMountedPath(fs::FS& fs, const char* fsPath, char* out, size_t outLen) {
    if (!fsPath || !out || outLen == 0) {
        return false;
    }
    const char* mountpoint = fs.mountpoint();
    if (!mountpoint || mountpoint[0] == '\0') {
        return false;
    }
    const int n = snprintf(out, outLen, "%s%s", mountpoint, fsPath);
    return n > 0 && static_cast<size_t>(n) < outLen;
}

static bool createContiguousReserve(fs::FS& fs, const char* fsPath, size_t size) {
    char fullPath[128];
    if (!buildMountedPath(fs, fsPath, fullPath, sizeof(fullPath))) {
        return false;
    }
    // opt=0 leaves the temp at logical size zero and seeds FatFs's next
    // allocation search. The caller immediately fills it while still owning
    // the project-wide SD lock, then verifies the resulting chain. In
    // contrast, opt=1 makes uninitialized card contents part of the file's
    // visible size before parser-safe bytes can be written.
    return esp_vfs_fat_create_contiguous_file(fs.mountpoint(), fullPath, size, false) == ESP_OK;
}

static bool verifyContiguousReserve(fs::FS& fs, const char* fsPath, bool& contiguous) {
    char fullPath[128];
    if (!buildMountedPath(fs, fsPath, fullPath, sizeof(fullPath))) {
        return false;
    }
    // IDF 5.5.4's public signature takes bool*, but vfs_fat.c passes that
    // pointer to an internal int* result and writes four bytes. Back it with
    // an aligned int so the vendor implementation cannot overwrite adjacent
    // stack state; do not replace this with a local bool until IDF fixes the
    // ABI mismatch.
    static_assert(sizeof(int) == 4, "IDF FAT contiguous result requires a 4-byte int");
    int contiguousWord = 0;
    const esp_err_t result =
        esp_vfs_fat_test_contiguous_file(fs.mountpoint(), fullPath, reinterpret_cast<bool*>(&contiguousWord));
    contiguous = contiguousWord != 0;
    return result == ESP_OK;
}

#else
static bool createContiguousReserve(fs::FS&, const char*, size_t) {
    return false;
}

static bool verifyContiguousReserve(fs::FS&, const char*, bool& contiguous) {
    contiguous = false;
    return false;
}

#endif

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
            snprintf(out, outLen, "/perf/perf_boot_%lu.csv", static_cast<unsigned long>(bootId_));
        } else {
            snprintf(out, outLen, "%s", PERF_CSV_PATH_FALLBACK);
        }
        return;
    }
    snprintf(out, outLen, "/perf/perf_boot_%lu-%08lx.csv", static_cast<unsigned long>(bootId_),
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

static bool appendCsvUInt64(char* buffer, size_t bufferLen, size_t& offset, uint64_t value) {
    return appendCsvFormat(buffer, bufferLen, offset, "%llu,", static_cast<unsigned long long>(value));
}

static bool appendCsvUInt64Last(char* buffer, size_t bufferLen, size_t& offset, uint64_t value) {
    return appendCsvFormat(buffer, bufferLen, offset, "%llu\n", static_cast<unsigned long long>(value));
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
static bool appendCsvUtcField(char* buffer, size_t bufferLen, size_t& offset, uint64_t utcEpochMs, bool valid) {
    if (!valid || utcEpochMs == 0) {
        return appendCsvFormat(buffer, bufferLen, offset, ",");
    }
    const uint64_t totalSec = utcEpochMs / 1000;
    const uint32_t ms = static_cast<uint32_t>(utcEpochMs % 1000);
    // Simple calendar decomposition (sufficient for logging, no leap-second handling)
    uint32_t sec = static_cast<uint32_t>(totalSec % 60);
    uint32_t min = static_cast<uint32_t>((totalSec / 60) % 60);
    uint32_t hour = static_cast<uint32_t>((totalSec / 3600) % 24);
    uint32_t days = static_cast<uint32_t>(totalSec / 86400); // days since 1970-01-01
    // Gregorian calendar
    uint32_t y = 1970;
    while (true) {
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        uint32_t diy = leap ? 366u : 365u;
        if (days < diy)
            break;
        days -= diy;
        y++;
    }
    static const uint8_t mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    uint32_t mo = 1;
    for (uint32_t m = 0; m < 12; m++) {
        uint32_t md = mdays[m] + ((m == 1 && leap) ? 1u : 0u);
        if (days < md) {
            mo = m + 1;
            break;
        }
        days -= md;
    }
    return appendCsvFormat(
        buffer, bufferLen, offset, "%04lu-%02lu-%02luT%02lu:%02lu:%02lu.%03luZ,", static_cast<unsigned long>(y),
        static_cast<unsigned long>(mo), static_cast<unsigned long>(days + 1), static_cast<unsigned long>(hour),
        static_cast<unsigned long>(min), static_cast<unsigned long>(sec), static_cast<unsigned long>(ms));
}
} // namespace

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
    reservedLayoutActive_ = false;
    reserveExhaustionPending_ = false;
    reserveExhaustionReported_ = false;
    reservedLogicalEnd_ = 0;
    pendingWrites_.store(0, std::memory_order_relaxed);
}
#endif

void PerfSdLogger::setBootId(uint32_t id, uint32_t bootToken) {
    bootId_ = id;
    bootToken_ = bootToken;
    buildPerfCsvPath(bootId_, bootToken_, csvPathBuf_, sizeof(csvPathBuf_));
    csvHeaderReady_ = false;
    sessionMarkerPending_ = true;
    reservedLayoutActive_ = false;
    reserveExhaustionPending_ = false;
    reserveExhaustionReported_ = false;
    reservedLogicalEnd_ = 0;
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
    reservedLayoutActive_ = false;
    reserveExhaustionPending_ = false;
    reserveExhaustionReported_ = false;
    reservedLogicalEnd_ = 0;
    rowsSinceFlush_ = 0;
    lastFlushMs_ = 0;
    sessionStartMs_ = millis();
    sessionStartUs_ = QualificationClock::nowMicros();
    clockSegment_ = QualificationClock::segment();
    sessionToken_ = static_cast<uint32_t>(esp_random());
    sessionSeq_++;

    if (!ensureCsvBuffers()) {
        return;
    }

    if (!queue_) {
        queue_ = createQueuePreferPsram(PERF_SD_QUEUE_DEPTH, sizeof(PerfSdSnapshot), queueAllocation_, &queueInPsram_);
        if (!queue_) {
            Serial.println("[Perf] ERROR: Failed to create SD logger queue");
            return;
        }
        if (!queueInPsram_) {
            Serial.println("[Perf] WARN: SD logger queue using internal SRAM fallback");
        }
    }

    if (!writerTask_) {
        BaseType_t rc = createTaskPinnedToCoreInternalStack(writerTaskEntry, "PerfSdWriter", PERF_SD_WRITER_STACK_SIZE,
                                                            this, PERF_SD_WRITER_PRIORITY, &writerTask_, 0);
        if (rc != pdPASS) {
            Serial.println("[Perf] ERROR: Failed to create SD logger task");
            return;
        }
    }

    // Warm the CSV now, during setup: /perf mkdir, file create, header, and
    // session marker are this file's first writes during boot and carry the
    // FAT-allocation cost (10-25 ms each on a worn card). Paying it here keeps
    // it off the first flush boundary after BLE connect, where an alert can be
    // in flight. Failure is not fatal — the writer retries lazily per row.
    if (storageManager.isReady() && storageManager.isSDCard()) {
        if (fs::FS* fs = storageManager.getFilesystem()) {
            StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
            if (lock) {
                // Reserve work is boot-only. If storage becomes available later,
                // the lazy writer takes the existing append path so a 1 MiB
                // allocation can never be shifted into an unreported background
                // phase. Any lazy append cost remains in appendSnapshotLine().
                reservedLayoutActive_ = prepareReservedFileLocked(*fs);
                if (ensurePersistentFileLocked(*fs)) {
                    if (!ensureCsvHeaderAndSessionMarker(persistentFile_)) {
                        persistentFile_.close();
                    }
                }
            }
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
    sessionStartUs_ = QualificationClock::nowMicros();
    clockSegment_ = QualificationClock::segment();
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

bool PerfSdLogger::writeParserSafeReservePadding(File& f) {
    if (!writeStagingBuffer_ || !f.seek(0, SeekSet)) {
        return false;
    }

    // Raw f_expand() contents are unspecified. Each fixed-size record is a
    // valid CSV comment line, and any suffix left after a logical row
    // overwrites its leading '#' consists only of spaces plus a newline.
    // Consequently a power loss, export, or append fallback can expose only
    // comments/blank lines after the last complete CSV row, never binary junk.
    memset(writeStagingBuffer_, ' ', PERF_SD_WRITE_STAGING_SIZE);
    writeStagingBuffer_[0] = '#';
    writeStagingBuffer_[PERF_SD_WRITE_STAGING_SIZE - 1] = '\n';

    size_t writtenTotal = 0;
    while (writtenTotal < PERF_SD_CONTIGUOUS_RESERVE_SIZE) {
        const size_t remaining = PERF_SD_CONTIGUOUS_RESERVE_SIZE - writtenTotal;
        const size_t chunkLen = (remaining > PERF_SD_WRITE_STAGING_SIZE) ? PERF_SD_WRITE_STAGING_SIZE : remaining;
        if (f.write(writeStagingBuffer_, chunkLen) != chunkLen) {
            return false;
        }
        writtenTotal += chunkLen;
        if (writtenTotal % PERF_SD_RESERVE_YIELD_EVERY_BYTES == 0) {
            // The setup task keeps the SD lock so no allocator can consume the
            // opt=0 contiguous hint, but a short delay lets idle/watchdog work
            // run during the one-time 1 MiB fill.
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    return f.position() == PERF_SD_CONTIGUOUS_RESERVE_SIZE;
}

bool PerfSdLogger::prepareReservedFileLocked(fs::FS& fs) {
    const uint32_t prepStartUs = PERF_TIMESTAMP_US();
    const char* csvPath = (csvPathBuf_[0] != '\0') ? csvPathBuf_ : PERF_CSV_PATH_FALLBACK;
    const auto logPrep = [&](const char* result, const char* createResult, const char* paddingResult,
                             const char* testResult, const char* contiguousResult, size_t physicalBytes) {
        Serial.printf("[PerfReserve] prep result=%s path=%s temp=%s requested=%u physical=%u logical=%u "
                      "create=%s padding=%s test=%s contiguous=%s elapsed_us=%lu\n",
                      result, csvPath, PERF_SD_RESERVE_TEMP_PATH,
                      static_cast<unsigned>(PERF_SD_CONTIGUOUS_RESERVE_SIZE), static_cast<unsigned>(physicalBytes),
                      static_cast<unsigned>(reservedLogicalEnd_), createResult, paddingResult, testResult,
                      contiguousResult, static_cast<unsigned long>(PERF_TIMESTAMP_US() - prepStartUs));
    };

    if (!ensurePerfDir(fs) || !writeStagingBuffer_) {
        logPrep("fallback_setup", "not_run", "not_run", "not_run", "unknown", 0);
        return false;
    }

    // A fixed temp name bounds crash debris to one file. Never replace an
    // existing boot CSV: a repeated begin() must preserve its prior evidence
    // and use the ordinary append path.
    if (fs.exists(PERF_SD_RESERVE_TEMP_PATH) && !fs.remove(PERF_SD_RESERVE_TEMP_PATH)) {
        logPrep("fallback_temp_cleanup", "not_run", "not_run", "not_run", "unknown", 0);
        return false;
    }
    if (fs.exists(csvPath)) {
        logPrep("append_existing", "not_run", "not_run", "not_run", "unknown", fs.open(csvPath, FILE_READ).size());
        return false;
    }

    if (!createContiguousReserve(fs, PERF_SD_RESERVE_TEMP_PATH, PERF_SD_CONTIGUOUS_RESERVE_SIZE)) {
        fs.remove(PERF_SD_RESERVE_TEMP_PATH);
        logPrep("fallback_create", "failed", "not_run", "not_run", "unknown", 0);
        return false;
    }

    File reserve = fs.open(PERF_SD_RESERVE_TEMP_PATH, PERF_SD_READ_WRITE_MODE, false);
    if (!reserve) {
        fs.remove(PERF_SD_RESERVE_TEMP_PATH);
        logPrep("fallback_reopen", "ok", "not_run", "not_run", "unknown", 0);
        return false;
    }

    const bool paddingWritten = writeParserSafeReservePadding(reserve);
    if (paddingWritten) {
        reserve.flush();
    }
    reserve.close();
    if (!paddingWritten) {
        fs.remove(PERF_SD_RESERVE_TEMP_PATH);
        logPrep("fallback_padding", "ok", "failed", "not_run", "unknown", 0);
        return false;
    }

    // File::size() can remain stale until FatFs flushes and closes the writer.
    // Reopen the closed extent and require its exact physical size before a
    // contiguity result is allowed to admit reserved mode.
    File extent = fs.open(PERF_SD_RESERVE_TEMP_PATH, FILE_READ, false);
    if (!extent) {
        fs.remove(PERF_SD_RESERVE_TEMP_PATH);
        logPrep("fallback_size_reopen", "ok", "ok", "not_run", "unknown", 0);
        return false;
    }
    const size_t physicalBytes = extent.size();
    extent.close();
    if (physicalBytes != PERF_SD_CONTIGUOUS_RESERVE_SIZE) {
        fs.remove(PERF_SD_RESERVE_TEMP_PATH);
        logPrep("fallback_size_mismatch", "ok", "ok", "not_run", "unknown", physicalBytes);
        return false;
    }

    bool contiguous = false;
    const bool testOk = verifyContiguousReserve(fs, PERF_SD_RESERVE_TEMP_PATH, contiguous);
    if (!testOk || !contiguous) {
        fs.remove(PERF_SD_RESERVE_TEMP_PATH);
        logPrep(testOk ? "fallback_fragmented" : "fallback_test", "ok", "ok", testOk ? "ok" : "failed",
                testOk ? "no" : "unknown", physicalBytes);
        return false;
    }

    if (fs.exists(csvPath) || !fs.rename(PERF_SD_RESERVE_TEMP_PATH, csvPath)) {
        fs.remove(PERF_SD_RESERVE_TEMP_PATH);
        logPrep("fallback_rename", "ok", "ok", "ok", "yes", physicalBytes);
        return false;
    }

    reservedLogicalEnd_ = 0;
    logPrep("active", "ok", "ok", "ok", "yes", physicalBytes);
    return true;
}

bool PerfSdLogger::ensureCsvBuffers() {
    if (!csvLineBuffer_) {
        csvLineBuffer_ =
            static_cast<char*>(heap_caps_malloc(PERF_CSV_LINE_BUFFER_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
        if (!csvLineBuffer_) {
            Serial.println("[Perf] ERROR: Failed to allocate SD CSV line buffer in PSRAM");
            return false;
        }
    }

    if (!writeStagingBuffer_) {
        writeStagingBuffer_ =
            static_cast<uint8_t*>(heap_caps_malloc(PERF_SD_WRITE_STAGING_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
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

    if (reservedLayoutActive_) {
        const size_t writeStart = reservedLogicalEnd_;
        if (writeStart % PERF_SD_WRITE_STAGING_SIZE != 0 || f.position() != writeStart || data[len - 1] != '\n') {
            return false;
        }

        size_t offset = 0;
        while (len - offset >= PERF_SD_WRITE_STAGING_SIZE) {
            memcpy(writeStagingBuffer_, data + offset, PERF_SD_WRITE_STAGING_SIZE);
            if (f.write(writeStagingBuffer_, PERF_SD_WRITE_STAGING_SIZE) != PERF_SD_WRITE_STAGING_SIZE) {
                return false;
            }
            offset += PERF_SD_WRITE_STAGING_SIZE;
        }

        const size_t tailLen = len - offset;
        if (tailLen > 0) {
            // Never issue a partial overwrite: combine the data tail and its
            // parser-safe suffix in one full sector. This avoids FatFs's
            // read-before-write path for fptr < physical file size.
            memcpy(writeStagingBuffer_, data + offset, tailLen);
            const size_t paddingLen = PERF_SD_WRITE_STAGING_SIZE - tailLen;
            if (paddingLen == 1) {
                writeStagingBuffer_[tailLen] = '\n';
            } else {
                writeStagingBuffer_[tailLen] = '#';
                if (paddingLen > 2) {
                    memset(writeStagingBuffer_ + tailLen + 1, ' ', paddingLen - 2);
                }
                writeStagingBuffer_[PERF_SD_WRITE_STAGING_SIZE - 1] = '\n';
            }
            if (f.write(writeStagingBuffer_, PERF_SD_WRITE_STAGING_SIZE) != PERF_SD_WRITE_STAGING_SIZE) {
                return false;
            }
        }

        const size_t paddedLen = tailLen == 0 ? len : len + (PERF_SD_WRITE_STAGING_SIZE - tailLen);
        const size_t writeEnd = f.position();
        if (writeEnd < writeStart || writeEnd - writeStart != paddedLen) {
            return false;
        }
        reservedLogicalEnd_ = writeEnd;
        if (!reserveExhaustionReported_ && writeEnd > PERF_SD_CONTIGUOUS_RESERVE_SIZE) {
            reserveExhaustionPending_ = true;
        }
        return true;
    }

    size_t offset = 0;
    while (offset < len) {
        const size_t remaining = len - offset;
        const size_t chunkLen = (remaining > PERF_SD_WRITE_STAGING_SIZE) ? PERF_SD_WRITE_STAGING_SIZE : remaining;
        memcpy(writeStagingBuffer_, data + offset, chunkLen);
        const size_t written = f.write(writeStagingBuffer_, chunkLen);
        if (written != chunkLen) {
            return false;
        }
        offset += chunkLen;
    }
    return true;
}

bool PerfSdLogger::formatSessionMarker(char* marker, size_t markerCapacity, size_t& markerLen) const {
    if (!marker || markerCapacity == 0) {
        return false;
    }
    const int n = snprintf(
        marker, markerCapacity,
        "#session_start,seq=%lu,bootId=%lu,uptime_ms=%lu,uptime_us=%llu,clockSegment=%llu,token=%08lX,schema=%lu\n",
        static_cast<unsigned long>(sessionSeq_), static_cast<unsigned long>(bootId_),
        static_cast<unsigned long>(sessionStartMs_), static_cast<unsigned long long>(sessionStartUs_),
        static_cast<unsigned long long>(clockSegment_), static_cast<unsigned long>(sessionToken_),
        static_cast<unsigned long>(PERF_CSV_SCHEMA_VERSION));
    if (n <= 0 || static_cast<size_t>(n) >= markerCapacity) {
        return false;
    }
    markerLen = static_cast<size_t>(n);
    return true;
}

bool PerfSdLogger::writeSessionMarker(File& f) {
    char marker[PERF_SD_SESSION_MARKER_BUFFER_SIZE];
    size_t markerLen = 0;
    if (!formatSessionMarker(marker, sizeof(marker), markerLen)) {
        return false;
    }
    return writeStaged(f, reinterpret_cast<const uint8_t*>(marker), markerLen);
}

bool PerfSdLogger::ensurePersistentFileLocked(fs::FS& fs) {
    if (!ensurePerfDir(fs)) {
        return false;
    }

    const char* csvPath = (csvPathBuf_[0] != '\0') ? csvPathBuf_ : PERF_CSV_PATH_FALLBACK;

    // Persistent handle: open once, keep open across rows. Eliminates the per-row
    // FAT EOF walk that contributes to sd_runtime_max_peak_us on the slower
    // FATFS path. Reserved files overwrite parser-safe, sector-aligned padding
    // through a logical cursor. Per-row f_sync still updates the dirent; after
    // the 1 MiB extent is consumed r+ grows normally and that cost remains timed.
    if (!persistentFile_) {
        if (reservedLayoutActive_) {
            persistentFile_ = fs.open(csvPath, PERF_SD_READ_WRITE_MODE, false);
            if (persistentFile_ && persistentFile_.seek(static_cast<uint32_t>(reservedLogicalEnd_), SeekSet)) {
                return true;
            }
            if (persistentFile_) {
                persistentFile_.close();
            }
            // The physical reserve contains only parser-safe comments/blanks.
            // Appending at its real EOF preserves all committed rows and keeps
            // subsequent allocation latency inside the normal row measurement.
            reservedLayoutActive_ = false;
            Serial.println("[Perf] WARN: Reserved CSV reopen/seek failed; using append fallback");
        }

        persistentFile_ = fs.open(csvPath, FILE_APPEND, true);
        if (!persistentFile_ && perfDirReady_) {
            // Directory can be removed while running; invalidate cache and retry once.
            perfDirReady_ = false;
            if (ensurePerfDir(fs)) {
                persistentFile_ = fs.open(csvPath, FILE_APPEND, true);
            }
        }
        if (!persistentFile_) {
            PERF_INC(perfSdOpenFail);
            return false;
        }
    }
    return true;
}

bool PerfSdLogger::ensureCsvHeaderAndSessionMarker(File& f) {
    // Physical size is fixed while a reserved file is being overwritten, so
    // logical end is the authoritative emptiness witness in that mode.
    if ((reservedLayoutActive_ && reservedLogicalEnd_ == 0) || (!reservedLayoutActive_ && f.size() == 0)) {
        csvHeaderReady_ = false;
    }

    bool metadataWritten = false;
    if (!csvHeaderReady_ && sessionMarkerPending_) {
        // The qualification scorer requires #session_start to be the line
        // immediately following its schema header. Compose them into one
        // sector-aligned transaction so reserved-mode padding follows the
        // marker rather than appearing between the two evidence records.
        const size_t headerLen = strlen(PERF_CSV_HEADER);
        if (!csvLineBuffer_ || headerLen >= PERF_CSV_LINE_BUFFER_SIZE) {
            PERF_INC(perfSdHeaderFail);
            return false;
        }
        memcpy(csvLineBuffer_, PERF_CSV_HEADER, headerLen);
        size_t markerLen = 0;
        if (!formatSessionMarker(csvLineBuffer_ + headerLen, PERF_CSV_LINE_BUFFER_SIZE - headerLen, markerLen) ||
            !writeStaged(f, reinterpret_cast<const uint8_t*>(csvLineBuffer_), headerLen + markerLen)) {
            PERF_INC(perfSdHeaderFail);
            return false;
        }
        metadataWritten = true;
        csvHeaderReady_ = true;
        sessionMarkerPending_ = false;
    }

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

    if (!ensurePersistentFileLocked(*fs)) {
        return false;
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
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.freeDmaCap) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.largestDmaCap) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.dmaFreeMin) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.dmaLargestMin) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.bleProcessMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.touchMaxUs) &&
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
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.pushNowRetries) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.pushNowFailures) &&
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
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.obdWriteCallMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.obdRssiCallMaxUs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.obdPollErrors) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.obdStaleCount) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.perfDrop) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.bleState) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.subscribeStep) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.connectInProgress) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.asyncConnectPending) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.pendingDisconnectCleanup) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.proxyAdvertising) &&
        appendCsvUInt8(line, lineBufferLen, offset, snapshot.proxyAdvertisingLastTransitionReason) &&
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
        // V1 notification arrival to completed display pipeline (schema v47)
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.notifyToDisplayPipelineCompleteMaxMs) &&
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.notifyToDisplayPipelineCompleteTotalCount) &&
        // V1 connected-readback evidence (schema v46)
        appendCsvUInt32(line, lineBufferLen, offset, snapshot.v1AllVolumeParsed) &&
        // Qualification clock alignment (schema v49)
        appendCsvUInt64(line, lineBufferLen, offset, snapshot.dutMicros) &&
        appendCsvUInt64Last(line, lineBufferLen, offset, snapshot.clockSegment);

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
    if (reserveExhaustionPending_ && !reserveExhaustionReported_) {
        reserveExhaustionPending_ = false;
        reserveExhaustionReported_ = true;
        Serial.printf("[PerfReserve] runtime result=extent_exhausted path=%s requested=%u logical=%u "
                      "growth=ordinary\n",
                      csvPath(), static_cast<unsigned>(PERF_SD_CONTIGUOUS_RESERVE_SIZE),
                      static_cast<unsigned>(reservedLogicalEnd_));
    }
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
                          timeoutMs, static_cast<unsigned long>(uxQueueMessagesWaiting(queue_)),
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

bool PerfSdLogger::tryResolveExportSize(size_t physicalSize, size_t& selectedSize) const {
    if ((queue_ && uxQueueMessagesWaiting(queue_) > 0) || pendingWrites_.load(std::memory_order_relaxed) > 0 ||
        persistentFile_) {
        return false;
    }
    if (!reservedLayoutActive_) {
        selectedSize = physicalSize;
        return true;
    }
    if (reservedLogicalEnd_ == 0 || reservedLogicalEnd_ > physicalSize) {
        return false;
    }
    selectedSize = reservedLogicalEnd_;
    return true;
}
