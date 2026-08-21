#include "display_commit_log.h"

#include <cstdio>
#include <cstring>

#ifndef UNIT_TEST
#include <Arduino.h>

#include "../../storage_manager.h"
#endif

namespace {
constexpr const char* COMMIT_DIR_PATH = "/display_commits";
constexpr const char* COMMIT_HEADER =
    "# display_commit_schema=2,timebase=millis,source=renderer_commit,"
    "alert_table_digest=fnv1a32(count_then_ordered_alert_fields_no_padding),"
    "complete_alert_rows=encounter_csv_by_session_revision_digest\n"
    "seq,millis,path,dispatch,render_us,pushes,arrows_to_show,blink_phase,arrow_painted,alert_count,"
    "region_x,region_y,region_w,region_h,active_bands,arrows,priority_arrow,signal_bars,flash_bits,"
    "band_flash_bits,muted,soft_muted,display_on,system_status,system_test,mode_char,bogey_char,"
    "bogey_dot,bogey_char2,main_volume,mute_volume,has_junk,has_photo,has_ku,dropped_commits,"
    "bogey_byte,bogey_byte2,bogey_dot2,has_mode,has_display_on,has_v1_version,v1_firmware_version,has_volume_data,"
    "v1_priority_index,saved_main_volume,saved_mute_volume,has_saved_volume,qualification_session_token,"
    "state_revision,alert_revision,state_event_seq,state_rx_first_seq,state_rx_last_seq,"
    "alert_event_seq,alert_rx_first_seq,alert_rx_last_seq,alert_table_fnv1a32,priority_valid,priority_v1_index,"
    "priority_band,priority_frequency_mhz,priority_direction,priority_front_raw,priority_rear_raw,priority_front_bars,"
    "priority_rear_bars,priority_flag,priority_junk,priority_photo_type,priority_raw_band_bits,priority_is_ku\n";
constexpr const char* COMMIT_EXPORT_MARKER_FORMAT =
    "# display_commit_export_schema=1,terminal_seq=%lu,dropped_commits=%lu\n";

#ifndef UNIT_TEST
// Peak commit rate on a replay window is ~18/s against a writer that already keeps up
// with the encounter log. Depth 32 covers roughly two seconds of backlog; beyond that
// the producer drops and counts rather than ever blocking the render path.
constexpr UBaseType_t COMMIT_QUEUE_DEPTH = 32;
constexpr uint32_t COMMIT_WRITER_STACK_SIZE = 6144;
constexpr UBaseType_t COMMIT_WRITER_PRIORITY = 1;
constexpr uint16_t FLUSH_EVERY_ROWS = 40;
constexpr uint32_t FLUSH_INTERVAL_MS = 5000;
#endif

const char* pathName(V1DisplayCommitPath path) {
    switch (path) {
    case V1DisplayCommitPath::Resting:
        return "RESTING";
    case V1DisplayCommitPath::Persisted:
        return "PERSISTED";
    case V1DisplayCommitPath::Live:
    default:
        return "LIVE";
    }
}

const char* dispatchName(V1DisplayCommitDispatch dispatch) {
    switch (dispatch) {
    case V1DisplayCommitDispatch::FullFlush:
        return "FULL";
    case V1DisplayCommitDispatch::PartialRegion:
        return "PARTIAL";
    case V1DisplayCommitDispatch::MultiRect:
        return "MULTIRECT";
    case V1DisplayCommitDispatch::None:
    default:
        return "NONE";
    }
}

// A printable stand-in so an unset or non-ASCII control character cannot break the
// column layout. The decoded character is what reaches pixels; 0 means "none".
char printableChar(char value) {
    return (value >= 0x20 && value < 0x7f) ? value : '-';
}
} // namespace

V1DisplayCommitLog v1DisplayCommitLog;

void V1DisplayCommitLog::setBootId(uint32_t bootId, uint32_t bootToken) {
    if (bootToken != 0) {
        snprintf(csvPathBuf_, sizeof(csvPathBuf_), "/display_commits/display_commits_%lu-%08lx.csv",
                 static_cast<unsigned long>(bootId), static_cast<unsigned long>(bootToken));
    } else {
        snprintf(csvPathBuf_, sizeof(csvPathBuf_), "/display_commits/display_commits_%lu.csv",
                 static_cast<unsigned long>(bootId));
    }
#ifndef UNIT_TEST
    headerReady_ = false;
#endif
}

void V1DisplayCommitLog::begin(bool sdAvailable) {
    enabled_ = false;
    seq_ = 0;
    droppedSnapshots_.store(0, std::memory_order_relaxed);
    pendingWrites_.store(0, std::memory_order_relaxed);
    qualificationSessionToken_.store(0, std::memory_order_relaxed);

    if (!sdAvailable) {
        return;
    }

#ifndef UNIT_TEST
    directoryReady_ = false;
    headerReady_ = false;
    rowsSinceFlush_ = 0;
    lastFlushMs_ = 0;
    if (!ensureWriter()) {
        Serial.println("[DisplayCommit] WARN: async SD writer unavailable");
        return;
    }
    {
        // Same warm-up rationale as the encounter logger: pay the directory, create,
        // and header FAT writes here during setup rather than on the first rendered
        // frame. Failure is non-fatal; ensureFileReady() retries lazily.
        StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
        if (!lock || !ensureFileReady()) {
            Serial.println("[DisplayCommit] WARN: storage warm-up deferred to first commit");
        }
    }
#endif
    enabled_ = true;
}

void V1DisplayCommitLog::beginQualificationSession(uint32_t sessionToken) {
    qualificationSessionToken_.store(sessionToken, std::memory_order_release);
}

void V1DisplayCommitLog::endQualificationSession(uint32_t sessionToken) {
    uint32_t expected = sessionToken;
    (void)qualificationSessionToken_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
}

void V1DisplayCommitLog::record(const V1DisplayCommitSnapshot& snapshot) {
    if (!enabled_) {
        return;
    }
    // Stamp the loss count here rather than trusting each call site to remember it.
    // A commit the queue dropped must never be indistinguishable from one that never
    // happened, so every surviving record carries the running total.
    V1DisplayCommitSnapshot stamped = snapshot;
    stamped.droppedSnapshots = droppedSnapshots_.load(std::memory_order_relaxed);
    stamped.qualificationSessionToken = qualificationSessionToken_.load(std::memory_order_acquire);
    enqueueSnapshot(stamped);
}

bool V1DisplayCommitLog::enqueueSnapshot(const V1DisplayCommitSnapshot& snapshot) {
#ifndef UNIT_TEST
    if (!queue_) {
        return false;
    }
    pendingWrites_.fetch_add(1, std::memory_order_relaxed);
    // Zero tick wait. The render path never blocks on the writer; a full queue drops
    // this commit and the count travels inside the next record.
    if (xQueueSend(queue_, &snapshot, 0) != pdTRUE) {
        pendingWrites_.fetch_sub(1, std::memory_order_relaxed);
        droppedSnapshots_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
#else
    return appendSnapshot(snapshot);
#endif
}

bool V1DisplayCommitLog::formatCsvLine(const V1DisplayCommitSnapshot& snapshot, char* out, size_t outLen) const {
    if (!out || outLen == 0) {
        return false;
    }
    const DisplayState& state = snapshot.state;
    const AlertData& priority = snapshot.priority;
    const int written = snprintf(
        out, outLen,
        "%lu,%lu,%s,%s,%lu,%lu,%u,%u,%u,%u,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%c,%c,%u,%c,%u,%u,%u,%u,%u,"
        "%lu,%u,%u,%u,%u,%u,%u,%lu,%u,%u,%u,%u,%u,%08lX,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%08lX,%u,%u,%u,"
        "%lu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
        static_cast<unsigned long>(snapshot.seq), static_cast<unsigned long>(snapshot.millisTs),
        pathName(snapshot.path), dispatchName(snapshot.dispatch), static_cast<unsigned long>(snapshot.renderUs),
        static_cast<unsigned long>(snapshot.pushes), static_cast<unsigned>(snapshot.arrowsToShow),
        static_cast<unsigned>(snapshot.blinkPhase), static_cast<unsigned>(snapshot.arrowPainted),
        static_cast<unsigned>(snapshot.alertCount), static_cast<int>(snapshot.regionX),
        static_cast<int>(snapshot.regionY), static_cast<int>(snapshot.regionW), static_cast<int>(snapshot.regionH),
        static_cast<unsigned>(state.activeBands), static_cast<unsigned>(state.arrows),
        static_cast<unsigned>(state.priorityArrow), static_cast<unsigned>(state.signalBars),
        static_cast<unsigned>(state.flashBits), static_cast<unsigned>(state.bandFlashBits), state.muted ? 1u : 0u,
        state.softMuted ? 1u : 0u, state.displayOn ? 1u : 0u, state.systemStatus ? 1u : 0u, state.systemTest ? 1u : 0u,
        printableChar(state.modeChar), printableChar(state.bogeyCounterChar), state.bogeyCounterDot ? 1u : 0u,
        printableChar(state.bogeyCounterChar2), static_cast<unsigned>(state.mainVolume),
        static_cast<unsigned>(state.muteVolume), state.hasJunkAlert ? 1u : 0u, state.hasPhotoAlert ? 1u : 0u,
        state.hasKuAlert ? 1u : 0u, static_cast<unsigned long>(snapshot.droppedSnapshots),
        static_cast<unsigned>(state.bogeyCounterByte), static_cast<unsigned>(state.bogeyCounterByte2),
        state.bogeyCounterDot2 ? 1u : 0u, state.hasMode ? 1u : 0u, state.hasDisplayOn ? 1u : 0u,
        state.hasV1Version ? 1u : 0u, static_cast<unsigned long>(state.v1FirmwareVersion),
        state.hasVolumeData ? 1u : 0u, static_cast<unsigned>(state.v1PriorityIndex),
        static_cast<unsigned>(state.savedMainVolume), static_cast<unsigned>(state.savedMuteVolume),
        state.hasSavedVolume ? 1u : 0u, static_cast<unsigned long>(snapshot.qualificationSessionToken),
        static_cast<unsigned long>(state.causal.stateRevision), static_cast<unsigned long>(state.causal.alertRevision),
        static_cast<unsigned long>(state.causal.stateSource.eventSeq),
        static_cast<unsigned long>(state.causal.stateSource.rxFirstSeq),
        static_cast<unsigned long>(state.causal.stateSource.rxLastSeq),
        static_cast<unsigned long>(state.causal.alertSource.eventSeq),
        static_cast<unsigned long>(state.causal.alertSource.rxFirstSeq),
        static_cast<unsigned long>(state.causal.alertSource.rxLastSeq),
        static_cast<unsigned long>(snapshot.alertTableDigest), priority.isValid ? 1u : 0u,
        static_cast<unsigned>(priority.v1Index), static_cast<unsigned>(priority.band),
        static_cast<unsigned long>(priority.frequency), static_cast<unsigned>(priority.direction),
        static_cast<unsigned>(priority.frontRawStrength), static_cast<unsigned>(priority.rearRawStrength),
        static_cast<unsigned>(priority.frontStrength), static_cast<unsigned>(priority.rearStrength),
        priority.isPriority ? 1u : 0u, priority.isJunk ? 1u : 0u, static_cast<unsigned>(priority.photoType),
        static_cast<unsigned>(priority.rawBandBits), priority.isKu ? 1u : 0u);
    return written > 0 && static_cast<size_t>(written) < outLen;
}

bool V1DisplayCommitLog::appendSnapshot(const V1DisplayCommitSnapshot& snapshot) {
#ifdef UNIT_TEST
    if (!formatCsvLine(snapshot, lastLineBuf_, sizeof(lastLineBuf_))) {
        return false;
    }
    ++commitsWritten_;
    return true;
#else
    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock || !ensureFileReady()) {
        return false;
    }

    char line[640];
    if (!formatCsvLine(snapshot, line, sizeof(line)) || persistentFile_.print(line) != strlen(line)) {
        persistentFile_.close();
        headerReady_ = false;
        return false;
    }
    ++rowsSinceFlush_;

    const uint32_t nowMs = millis();
    if (rowsSinceFlush_ >= FLUSH_EVERY_ROWS || (nowMs - lastFlushMs_) >= FLUSH_INTERVAL_MS) {
        persistentFile_.flush();
        rowsSinceFlush_ = 0;
        lastFlushMs_ = nowMs;
    }
    return true;
#endif
}

#ifndef UNIT_TEST
bool V1DisplayCommitLog::ensureWriter() {
    if (!queue_) {
        bool queueInPsram = false;
        queue_ = createQueuePreferPsram(COMMIT_QUEUE_DEPTH, sizeof(V1DisplayCommitSnapshot), queueAllocation_,
                                        &queueInPsram);
        if (!queue_) {
            return false;
        }
        if (!queueInPsram) {
            Serial.println("[DisplayCommit] WARN: queue using internal SRAM fallback");
        }
    }
    if (!writerTask_) {
        const BaseType_t result =
            createTaskPinnedToCoreInternalStack(writerTaskEntry, "DisplayCommitWriter", COMMIT_WRITER_STACK_SIZE, this,
                                                COMMIT_WRITER_PRIORITY, &writerTask_, 0);
        if (result != pdPASS) {
            return false;
        }
    }
    return true;
}

void V1DisplayCommitLog::writerTaskEntry(void* context) {
    static_cast<V1DisplayCommitLog*>(context)->writerTaskLoop();
}

void V1DisplayCommitLog::writerTaskLoop() {
    while (true) {
        V1DisplayCommitSnapshot snapshot;
        if (xQueueReceive(queue_, &snapshot, portMAX_DELAY) == pdTRUE) {
            if (!appendSnapshot(snapshot)) {
                droppedSnapshots_.fetch_add(1, std::memory_order_relaxed);
            }
            pendingWrites_.fetch_sub(1, std::memory_order_relaxed);
            taskYIELD();
        }
    }
}

bool V1DisplayCommitLog::ensureFileReady() {
    fs::FS* filesystem = storageManager.getFilesystem();
    if (!filesystem) {
        return false;
    }
    if (!directoryReady_) {
        directoryReady_ = filesystem->mkdir(COMMIT_DIR_PATH) || filesystem->exists(COMMIT_DIR_PATH);
        if (!directoryReady_) {
            return false;
        }
    }
    if (!persistentFile_) {
        persistentFile_ = filesystem->open(csvPathBuf_, FILE_APPEND, true);
        if (!persistentFile_) {
            return false;
        }
    }
    if (!headerReady_) {
        if (persistentFile_.size() == 0 && persistentFile_.print(COMMIT_HEADER) != strlen(COMMIT_HEADER)) {
            persistentFile_.close();
            return false;
        }
        headerReady_ = true;
    }
    return true;
}
#endif

void V1DisplayCommitLog::drainAndClose(uint32_t timeoutMs) {
#ifndef UNIT_TEST
    if (!enabled_ || !queue_) {
        return;
    }
    const uint32_t startMs = millis();
    while (pendingWrites_.load(std::memory_order_relaxed) > 0 || uxQueueMessagesWaiting(queue_) > 0) {
        if ((millis() - startMs) > timeoutMs) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (lock && persistentFile_) {
        persistentFile_.flush();
        persistentFile_.close();
    }
#else
    (void)timeoutMs;
#endif
}

bool V1DisplayCommitLog::tryDrainAndClose() {
#ifndef UNIT_TEST
    if (!enabled_ || !queue_) {
        return true;
    }
    // Qualification calls this from the same main-loop command path that owns
    // record(), then opens the fixed-size export before returning to that loop.
    // The writer task is the only concurrent owner and is covered by these two
    // counters plus the SD mutex below.
    if (pendingWrites_.load(std::memory_order_relaxed) > 0 || uxQueueMessagesWaiting(queue_) > 0) {
        return false;
    }

    StorageManager::SDTryLock lock(storageManager.getSDMutex());
    if (!lock || !ensureFileReady()) {
        return false;
    }

    char marker[128];
    const int markerLen =
        snprintf(marker, sizeof(marker), COMMIT_EXPORT_MARKER_FORMAT, static_cast<unsigned long>(seq_),
                 static_cast<unsigned long>(droppedSnapshots_.load(std::memory_order_relaxed)));
    if (markerLen <= 0 || static_cast<size_t>(markerLen) >= sizeof(marker) ||
        persistentFile_.print(marker) != static_cast<size_t>(markerLen)) {
        persistentFile_.close();
        headerReady_ = false;
        return false;
    }

    persistentFile_.flush();
    persistentFile_.close();
    rowsSinceFlush_ = 0;
    lastFlushMs_ = millis();
    return true;
#else
    return true;
#endif
}
