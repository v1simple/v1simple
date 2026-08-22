/**
 * Display commit log — what the renderer committed to putting on the screen.
 *
 * One record per display commit: the DisplayState the renderer actually consumed,
 * the values it resolved after that state (visible arrow set, blink phase), and how
 * the result was dispatched to the panel. Timestamped in DUT millis.
 *
 * This is the left-hand side of "what should be on screen versus what appeared".
 * It is not a judgement and carries no thresholds — the DUT records what it did,
 * and the comparison happens off-device against camera evidence.
 *
 * Display code only copies a bounded snapshot into a zero-wait queue, exactly as
 * V1EncounterLogger does for alert tables. A low-priority Core 0 task owns all
 * filesystem work, so nothing on the render path waits on SD.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "../../packet_parser_types.h"

#ifndef UNIT_TEST
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "../../psram_freertos_alloc.h"
#endif

// Which renderer path committed this frame.
enum class V1DisplayCommitPath : uint8_t { Live = 0, Resting = 1, Persisted = 2, Scanning = 3, Stealth = 4 };

// What physically went to the panel. Describes the transfer, not the reason for it:
// the reason lives in the perf counters, but only this says what the panel received.
enum class V1DisplayCommitDispatch : uint8_t {
    None = 0,          // nothing pushed (cache hit / no repaint)
    FullFlush = 1,     // whole panel
    PartialRegion = 2, // one region window
    MultiRect = 3,     // several region windows
};

struct V1DisplayCommitSnapshot {
    uint32_t seq = 0;              // monotonic commit id
    uint32_t millisTs = 0;         // DUT millis at commit
    uint32_t renderUs = 0;         // resolve + paint + dispatch duration
    uint32_t pushes = 0;           // physical panel transfers this commit caused
    uint32_t droppedSnapshots = 0; // commits lost to queue pressure, cumulative
    uint32_t qualificationSessionToken = 0;
    // Parser-published table digest: joins this commit's alert revision to the
    // complete semantic rows in the encounter CSV.
    uint32_t alertTableDigest = 0;
    V1DisplayCommitPath path = V1DisplayCommitPath::Live;
    V1DisplayCommitDispatch dispatch = V1DisplayCommitDispatch::None;
    uint8_t arrowsToShow = 0; // resolved visible arrow set, after the priority-arrow setting
    uint8_t blinkPhase = 0;   // renderer's shared blink phase at commit
    uint8_t arrowPainted = 0; // the arrow region actually repainted this commit
    uint8_t alertCount = 0;
    int16_t regionX = 0;
    int16_t regionY = 0;
    int16_t regionW = 0;
    int16_t regionH = 0;
    // The state the renderer consumed, stored whole. The CSV projects the fields that
    // reach pixels; keeping the struct means widening that projection later is a
    // formatter change, not another firmware change.
    DisplayState state{};
    // Full priority semantics supplied to the renderer. The complete ordered
    // all-alert input is joined through alertRevision + alertTableDigest to the
    // per-alert encounter rows, avoiding a 15-alert copy in every queue item.
    AlertData priority{};
    uint64_t clockSegment = 0;
    uint64_t renderRequestDutMicros = 0;
    uint64_t displayCommitDutMicros = 0;
};

class V1DisplayCommitLog {
  public:
    void setBootId(uint32_t bootId, uint32_t bootToken = 0);
    void begin(bool sdAvailable);
    void record(const V1DisplayCommitSnapshot& snapshot);
    bool beginQualificationSession(uint32_t sessionToken);
    void endQualificationSession(uint32_t sessionToken);
    void drainAndClose(uint32_t timeoutMs);
    bool tryDrainAndClose();

    bool isEnabled() const { return enabled_; }
    bool isQualificationSessionActive() const {
        return qualificationSessionToken_.load(std::memory_order_acquire) != 0;
    }
    const char* csvPath() const { return csvPathBuf_; }
    uint32_t nextSeq() { return ++seq_; }

#ifdef UNIT_TEST
    const char* testGetLastLine() const { return lastLineBuf_; }
    uint32_t testCommitCount() const { return commitsWritten_; }
    uint32_t testDroppedCount() const { return droppedSnapshots_.load(std::memory_order_relaxed); }
    void testForceDrop() { droppedSnapshots_.fetch_add(1, std::memory_order_relaxed); }
#endif

  private:
    bool enqueueSnapshot(const V1DisplayCommitSnapshot& snapshot);
    bool appendSnapshot(const V1DisplayCommitSnapshot& snapshot);
    bool formatCsvLine(const V1DisplayCommitSnapshot& snapshot, char* out, size_t outLen) const;

#ifndef UNIT_TEST
    static void writerTaskEntry(void* context);
    void writerTaskLoop();
    bool ensureWriter();
    bool ensureFileReady();
#endif

    bool enabled_ = false;
    uint32_t seq_ = 0;
    std::atomic<uint32_t> droppedSnapshots_{0};
    std::atomic<uint32_t> pendingWrites_{0};
    std::atomic<uint32_t> qualificationSessionToken_{0};
    char csvPathBuf_[64] = {0};

#ifndef UNIT_TEST
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t writerTask_ = nullptr;
    PsramQueueAllocation queueAllocation_ = {};
    File persistentFile_{};
    bool directoryReady_ = false;
    bool headerReady_ = false;
    uint16_t rowsSinceFlush_ = 0;
    uint32_t lastFlushMs_ = 0;
#else
    uint32_t commitsWritten_ = 0;
    char lastLineBuf_[640] = {0};
#endif
};

extern V1DisplayCommitLog v1DisplayCommitLog;
