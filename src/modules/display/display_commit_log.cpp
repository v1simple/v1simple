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
    "# display_commit_schema=1,timebase=millis,source=renderer_commit\n"
    "seq,millis,path,dispatch,render_us,pushes,arrows_to_show,blink_phase,arrow_painted,alert_count,"
    "region_x,region_y,region_w,region_h,active_bands,arrows,priority_arrow,signal_bars,flash_bits,"
    "band_flash_bits,muted,soft_muted,display_on,system_status,system_test,mode_char,bogey_char,"
    "bogey_dot,bogey_char2,main_volume,mute_volume,has_junk,has_photo,has_ku,dropped_commits\n";
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

void V1DisplayCommitLog::record(const V1DisplayCommitSnapshot& snapshot) {
    if (!enabled_) {
        return;
    }
    // Stamp the loss count here rather than trusting each call site to remember it.
    // A commit the queue dropped must never be indistinguishable from one that never
    // happened, so every surviving record carries the running total.
    V1DisplayCommitSnapshot stamped = snapshot;
    stamped.droppedSnapshots = droppedSnapshots_.load(std::memory_order_relaxed);
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
    const int written = snprintf(
        out, outLen,
        "%lu,%lu,%s,%s,%lu,%lu,%u,%u,%u,%u,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%c,%c,%u,%c,%u,%u,%u,%u,%u,"
        "%lu\n",
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
        state.hasKuAlert ? 1u : 0u, static_cast<unsigned long>(snapshot.droppedSnapshots));
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

    char line[320];
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
