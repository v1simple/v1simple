/**
 * Compact SD logger for complete V1 alert tables.
 *
 * Parser callbacks only copy a bounded snapshot into a zero-wait queue. A
 * low-priority Core 0 task owns all filesystem work.
 */

#pragma once

#include <array>
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

class PacketParser;

inline constexpr size_t V1_ENCOUNTER_MAX_ALERTS = 15;

enum class V1EncounterEvent : uint8_t { Start, Sample, End };

struct V1EncounterAlertSample {
    uint32_t frequency = 0;
    uint8_t v1Index = 0;
    uint8_t band = 0;
    uint8_t direction = 0;
    uint8_t frontRaw = 0;
    uint8_t rearRaw = 0;
    uint8_t frontBars = 0;
    uint8_t rearBars = 0;
    uint8_t photoType = 0;
    bool priority = false;
    bool junk = false;
};

struct V1EncounterSnapshot {
    uint32_t millisTs = 0;
    uint32_t encounterId = 0;
    uint32_t sampleSeq = 0;
    uint32_t droppedSnapshots = 0;
    V1EncounterEvent event = V1EncounterEvent::Sample;
    uint8_t alertCount = 0;
    std::array<V1EncounterAlertSample, V1_ENCOUNTER_MAX_ALERTS> alerts{};
};

class V1EncounterLogger {
  public:
    void setBootId(uint32_t bootId, uint32_t bootToken = 0);
    void begin(bool sdAvailable);
    void attach(PacketParser& parser);
    void onAlertTable(const AlertData* alerts, size_t count, uint32_t nowMs);
    void drainAndClose(uint32_t timeoutMs);

    bool isEnabled() const { return enabled_; }
    const char* csvPath() const { return csvPathBuf_; }

#ifdef UNIT_TEST
    const char* testGetLastLine() const { return lastLineBuf_; }
    uint32_t testSnapshotCount() const { return snapshotsWritten_; }
    uint32_t testLineCount() const { return linesWritten_; }
#endif

  private:
    static void parserObserver(const AlertData* alerts, size_t count, uint32_t nowMs, void* context);
    bool enqueueSnapshot(const V1EncounterSnapshot& snapshot);
    bool appendSnapshot(const V1EncounterSnapshot& snapshot);
    bool formatCsvLine(const V1EncounterSnapshot& snapshot, size_t alertIndex, char* out, size_t outLen) const;
    V1EncounterSnapshot makeSnapshot(V1EncounterEvent event, uint32_t nowMs,
                                     const std::array<V1EncounterAlertSample, V1_ENCOUNTER_MAX_ALERTS>& alerts,
                                     size_t count);

#ifndef UNIT_TEST
    static void writerTaskEntry(void* context);
    void writerTaskLoop();
    bool ensureWriter();
    bool ensureFileReady();
#endif

    bool enabled_ = false;
    bool encounterActive_ = false;
    uint32_t encounterId_ = 0;
    uint32_t encounterSampleSeq_ = 0;
    uint32_t lastSnapshotMs_ = 0;
    uint8_t lastObservedCount_ = 0;
    uint8_t lastEmittedCount_ = 0;
    std::array<V1EncounterAlertSample, V1_ENCOUNTER_MAX_ALERTS> lastObservedAlerts_{};
    std::array<V1EncounterAlertSample, V1_ENCOUNTER_MAX_ALERTS> lastEmittedAlerts_{};
    std::atomic<uint32_t> droppedSnapshots_{0};
    std::atomic<uint32_t> pendingWrites_{0};
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
    uint32_t snapshotsWritten_ = 0;
    uint32_t linesWritten_ = 0;
    char lastLineBuf_[256] = {0};
#endif
};

extern V1EncounterLogger v1EncounterLogger;
