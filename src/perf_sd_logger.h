/**
 * Standalone SD-backed performance CSV logger.
 *
 * Writes compact perf snapshots to /perf/perf_boot_<bootId>-<token8>.csv.
 * (legacy format /perf/perf_boot_<bootId>.csv used only when no bootToken is
 * supplied, e.g. unit tests; /perf/perf.csv if neither bootId nor token is set).
 * Uses a dedicated FreeRTOS writer task; enqueue is non-blocking and drops on queue full.
 */

#pragma once

#include <Arduino.h>
#include <FS.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "psram_freertos_alloc.h"

struct PerfSdSnapshot;

class PerfSdLogger {
public:
#ifdef UNIT_TEST
    ~PerfSdLogger();
#endif

    void begin(bool sdAvailable);
    void setBootId(uint32_t id, uint32_t bootToken = 0);
    bool enqueue(const PerfSdSnapshot& snapshot);
    bool isEnabled() const { return enabled_; }
    const char* csvPath() const { return csvPathBuf_; }
#ifndef UNIT_TEST
    bool writerTaskActive() const { return writerTask_ != nullptr; }
    bool writerTaskStackInPsram() const { return writerTaskStackInPsram_; }
    uint32_t writerStackHighWaterBytes() const;
#endif

    /// Start a new logical session within the current boot file.
    /// Emits a fresh CSV header + #session_start marker so scoring tools
    /// can isolate V1-connected data from idle boot noise.
    void startNewSession();

    /// Drain queue and close file with timeout (car power mode shutdown).
    /// Blocks until queue is empty or timeout expires.
    void drainAndClose(uint32_t timeoutMs);

    /// Non-blocking close attempt for main-loop qualification/export paths.
    /// Returns true once the queue is empty and the persistent handle is closed.
    bool tryDrainAndClose();

private:
    static void writerTaskEntry(void* param);
    void writerTaskLoop();
    bool receiveSnapshot(PerfSdSnapshot& snapshot, TickType_t timeoutTicks);
    bool ensurePerfDir(fs::FS& fs);
    bool ensureCsvHeaderAndSessionMarker(File& f);
    bool writeSessionMarker(File& f);
    bool ensureCsvBuffers();
    bool writeStaged(File& f, const uint8_t* data, size_t len);
    bool flushPersistentFile(File& f);
    bool flushPersistentFileIfDue(File& f);
    bool appendSnapshotLine(const PerfSdSnapshot& snapshot);

    bool enabled_ = false;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t writerTask_ = nullptr;
    PsramQueueAllocation queueAllocation_ = {};
    bool queueInPsram_ = false;
    bool writerTaskStackInPsram_ = false;
    char* csvLineBuffer_ = nullptr;
    uint8_t* writeStagingBuffer_ = nullptr;
    bool perfDirReady_ = false;
    bool csvHeaderReady_ = false;
    bool sessionMarkerPending_ = false;
    File persistentFile_{};
    uint32_t sessionSeq_ = 0;
    uint32_t sessionToken_ = 0;
    uint32_t sessionStartMs_ = 0;
    uint32_t bootId_ = 0;
    uint32_t bootToken_ = 0;
    uint16_t rowsSinceFlush_ = 0;
    uint32_t lastFlushMs_ = 0;
    char csvPathBuf_[64] = {0};
    std::atomic<uint32_t> pendingWrites_{0};

#ifdef UNIT_TEST
public:
    void releaseForTest();

    bool receiveSnapshotForTest(PerfSdSnapshot& snapshot, TickType_t timeoutTicks) {
        return receiveSnapshot(snapshot, timeoutTicks);
    }
    bool csvLineBufferAllocatedForTest() const { return csvLineBuffer_ != nullptr; }
    bool writeStagingBufferAllocatedForTest() const { return writeStagingBuffer_ != nullptr; }
#endif
};

extern PerfSdLogger perfSdLogger;
