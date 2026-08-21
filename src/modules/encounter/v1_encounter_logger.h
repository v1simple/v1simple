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
#include "../../causal_evidence_types.h"

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
    uint8_t rawBandBits = 0;
    bool valid = false;
    bool priority = false;
    bool junk = false;
    bool isKu = false;
};

struct V1EncounterSnapshot {
    uint32_t millisTs = 0;
    uint32_t encounterId = 0;
    uint32_t sampleSeq = 0;
    uint32_t droppedSnapshots = 0;
    uint32_t qualificationSessionToken = 0;
    uint32_t stateRevision = 0;
    uint32_t alertRevision = 0;
    uint32_t alertTableDigest = 0;
    V1CausalIdentity stateSource{};
    V1CausalIdentity alertSource{};
    V1EncounterEvent event = V1EncounterEvent::Sample;
    uint8_t alertCount = 0;
    std::array<V1EncounterAlertSample, V1_ENCOUNTER_MAX_ALERTS> alerts{};
};

struct V1PersistedCausalTraceRecord {
    uint32_t traceSeq = 0;
    uint32_t qualificationSessionToken = 0;
    uint32_t lostTraceRecords = 0;
    V1CausalTraceRecord record{};
};

class V1EncounterLogger {
  public:
    void setBootId(uint32_t bootId, uint32_t bootToken = 0);
    void begin(bool sdAvailable);
    void attach(PacketParser& parser);
    void onAlertTable(const AlertData* alerts, size_t count, uint32_t nowMs);
    void beginQualificationSession(uint32_t sessionToken, uint32_t startedAtDutMs, uint32_t sourceLossCount = 0);
    void endQualificationSession(uint32_t sessionToken, uint32_t endedAtDutMs, uint32_t sourceLossCount = 0);
    void recordCausalTrace(const V1CausalTraceRecord& record);
    bool tryDrainQualificationEvidence();
    void drainAndClose(uint32_t timeoutMs);

    bool isEnabled() const { return enabled_; }
    const char* csvPath() const { return csvPathBuf_; }
    const char* causalTraceCsvPath() const { return causalTracePathBuf_; }

#ifdef UNIT_TEST
    const char* testGetLastLine() const { return lastLineBuf_; }
    uint32_t testSnapshotCount() const { return snapshotsWritten_; }
    uint32_t testLineCount() const { return linesWritten_; }
    const char* testGetLastTraceLine() const { return lastTraceLineBuf_; }
    uint32_t testTraceCount() const { return tracesWritten_; }
    const V1PersistedCausalTraceRecord* testGetTraceRecord(size_t index) const {
        return index < tracesWritten_ && index < testTraceRecords_.size() ? &testTraceRecords_[index] : nullptr;
    }
    void testBeginQualificationSession(uint32_t sessionToken, uint32_t startedAtDutMs,
                                       const V1SemanticRevisionEvidence& retainedEvidence,
                                       uint32_t sourceLossCount = 0);
#endif

  private:
    static void parserObserver(const AlertData* alerts, size_t count, uint32_t nowMs, void* context);
    bool enqueueSnapshot(const V1EncounterSnapshot& snapshot);
    bool appendSnapshot(const V1EncounterSnapshot& snapshot);
    bool enqueueTrace(const V1PersistedCausalTraceRecord& record);
    bool appendTrace(const V1PersistedCausalTraceRecord& record);
    bool formatCsvLine(const V1EncounterSnapshot& snapshot, size_t alertIndex, char* out, size_t outLen) const;
    bool formatTraceCsvLine(const V1PersistedCausalTraceRecord& record, char* out, size_t outLen) const;
    void beginQualificationSessionWithEvidence(uint32_t sessionToken, uint32_t startedAtDutMs, uint32_t sourceLossCount,
                                               const V1SemanticRevisionEvidence* retainedEvidence);
    void recordRetainedBaselines(const V1SemanticRevisionEvidence& evidence, uint32_t startedAtDutMs,
                                 uint32_t sourceLossCount);
    V1EncounterSnapshot makeSnapshot(V1EncounterEvent event, uint32_t nowMs,
                                     const std::array<V1EncounterAlertSample, V1_ENCOUNTER_MAX_ALERTS>& alerts,
                                     size_t count);

#ifndef UNIT_TEST
    static void writerTaskEntry(void* context);
    void writerTaskLoop();
    bool ensureWriter();
    bool ensureFileReady();
    bool ensureTraceFileReady();
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
    std::atomic<uint32_t> droppedTraceRecords_{0};
    std::atomic<uint32_t> pendingTraceWrites_{0};
    std::atomic<uint32_t> qualificationSessionToken_{0};
    std::atomic<bool> qualificationSessionActive_{false};
    // Main-loop owned: qualification commands and the BLE queue parser observer
    // both call recordCausalTrace synchronously from the Arduino loop task.
    uint32_t traceSeq_ = 0;
    PacketParser* parser_ = nullptr;
    char csvPathBuf_[64] = {0};
    char causalTracePathBuf_[64] = {0};

#ifndef UNIT_TEST
    QueueHandle_t queue_ = nullptr;
    QueueHandle_t traceQueue_ = nullptr;
    TaskHandle_t writerTask_ = nullptr;
    PsramQueueAllocation queueAllocation_ = {};
    PsramQueueAllocation traceQueueAllocation_ = {};
    File persistentFile_{};
    File traceFile_{};
    bool directoryReady_ = false;
    bool headerReady_ = false;
    bool traceHeaderReady_ = false;
    uint16_t rowsSinceFlush_ = 0;
    uint32_t lastFlushMs_ = 0;
    uint16_t traceRowsSinceFlush_ = 0;
    uint32_t traceLastFlushMs_ = 0;
#else
    uint32_t snapshotsWritten_ = 0;
    uint32_t linesWritten_ = 0;
    char lastLineBuf_[384] = {0};
    uint32_t tracesWritten_ = 0;
    char lastTraceLineBuf_[384] = {0};
    std::array<V1PersistedCausalTraceRecord, 8> testTraceRecords_{};
#endif
};

extern V1EncounterLogger v1EncounterLogger;
