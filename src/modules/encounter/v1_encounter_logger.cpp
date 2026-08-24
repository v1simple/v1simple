#include "v1_encounter_logger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../../packet_parser.h"
#include "../../qualification_clock.h"

#ifndef UNIT_TEST
#include <Arduino.h>

#include "../../storage_manager.h"
#endif

namespace {
constexpr const char* ENCOUNTER_DIR_PATH = "/encounters";
constexpr const char* ENCOUNTER_HEADER =
    "# encounter_schema=4,timebase=millis,monotonic_timebase=esp_timer_us,v1_assignments=raw,no_gps=1,no_speed=1,"
    "alert_table_digest=fnv1a32(count_then_ordered_alert_fields_no_padding)\n"
    "millis,encounter_id,sample_seq,event,v1_index,alert_count,band,frequency_mhz,direction,front_raw,rear_raw,"
    "front_bars,rear_bars,priority,junk,photo_type,dropped_snapshots,qualification_session_token,"
    "valid,raw_band_bits,is_ku,clock_segment,sample_dut_micros\n";
constexpr uint32_t SAMPLE_INTERVAL_MS = 250;
constexpr uint32_t KEEPALIVE_INTERVAL_MS = 5000;

#ifndef UNIT_TEST
constexpr UBaseType_t ENCOUNTER_QUEUE_DEPTH = 8;
constexpr uint32_t ENCOUNTER_WRITER_STACK_SIZE = 6144;
constexpr UBaseType_t ENCOUNTER_WRITER_PRIORITY = 1;
constexpr uint16_t FLUSH_EVERY_ROWS = 20;
constexpr uint32_t FLUSH_INTERVAL_MS = 5000;
#endif

V1EncounterAlertSample compactAlert(const AlertData& alert) {
    V1EncounterAlertSample result;
    result.frequency = alert.frequency;
    result.v1Index = alert.v1Index;
    result.band = static_cast<uint8_t>(alert.band);
    result.direction = static_cast<uint8_t>(alert.direction);
    result.frontRaw = alert.frontRawStrength;
    result.rearRaw = alert.rearRawStrength;
    result.frontBars = alert.frontStrength;
    result.rearBars = alert.rearStrength;
    result.photoType = alert.photoType;
    result.rawBandBits = alert.rawBandBits;
    result.valid = alert.isValid;
    result.priority = alert.isPriority;
    result.junk = alert.isJunk;
    result.isKu = alert.isKu;
    return result;
}

bool sameImmediateState(const V1EncounterAlertSample& a, const V1EncounterAlertSample& b) {
    return a.v1Index == b.v1Index && a.band == b.band && a.direction == b.direction && a.photoType == b.photoType &&
           a.priority == b.priority && a.junk == b.junk;
}

bool sameSampleState(const V1EncounterAlertSample& a, const V1EncounterAlertSample& b) {
    return sameImmediateState(a, b) && a.frequency == b.frequency && a.frontBars == b.frontBars &&
           a.rearBars == b.rearBars;
}

template <typename Compare>
bool sameTable(const std::array<V1EncounterAlertSample, V1_ENCOUNTER_MAX_ALERTS>& a, size_t aCount,
               const std::array<V1EncounterAlertSample, V1_ENCOUNTER_MAX_ALERTS>& b, size_t bCount, Compare compare) {
    if (aCount != bCount) {
        return false;
    }
    for (size_t i = 0; i < aCount; ++i) {
        if (!compare(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

const char* eventName(V1EncounterEvent event) {
    switch (event) {
    case V1EncounterEvent::Start:
        return "START";
    case V1EncounterEvent::End:
        return "END";
    case V1EncounterEvent::Sample:
    default:
        return "SAMPLE";
    }
}

const char* directionName(uint8_t direction) {
    switch (static_cast<Direction>(direction)) {
    case DIR_FRONT:
        return "FRONT";
    case DIR_SIDE:
        return "SIDE";
    case DIR_REAR:
        return "REAR";
    case DIR_NONE:
    default:
        return "NONE";
    }
}

const char* encounterBandName(uint8_t band) {
    switch (static_cast<Band>(band)) {
    case BAND_LASER:
        return "Laser";
    case BAND_KA:
        return "Ka";
    case BAND_K:
        return "K";
    case BAND_X:
        return "X";
    case BAND_KU:
        return "Ku";
    case BAND_NONE:
    default:
        return "None";
    }
}

} // namespace

V1EncounterLogger v1EncounterLogger;

void V1EncounterLogger::setBootId(uint32_t bootId, uint32_t bootToken) {
    if (bootToken != 0) {
        snprintf(csvPathBuf_, sizeof(csvPathBuf_), "/encounters/encounters_%lu-%08lx.csv",
                 static_cast<unsigned long>(bootId), static_cast<unsigned long>(bootToken));
    } else {
        snprintf(csvPathBuf_, sizeof(csvPathBuf_), "/encounters/encounters_%lu.csv",
                 static_cast<unsigned long>(bootId));
    }
#ifndef UNIT_TEST
    headerReady_ = false;
#endif
}

void V1EncounterLogger::begin(bool sdAvailable) {
    enabled_ = false;
    encounterActive_ = false;
    encounterId_ = 0;
    encounterSampleSeq_ = 0;
    lastObservedCount_ = 0;
    lastEmittedCount_ = 0;
    lastSnapshotMs_ = 0;
    droppedSnapshots_.store(0, std::memory_order_relaxed);
    pendingWrites_.store(0, std::memory_order_relaxed);
    qualificationSessionToken_.store(0, std::memory_order_relaxed);
    qualificationSessionActive_.store(false, std::memory_order_relaxed);

    if (!sdAvailable) {
        return;
    }

#ifndef UNIT_TEST
    directoryReady_ = false;
    headerReady_ = false;
    rowsSinceFlush_ = 0;
    lastFlushMs_ = 0;
    if (!ensureWriter()) {
        Serial.println("[Encounter] WARN: async SD writer unavailable");
        return;
    }
    {
        // Warm the encounters file now, during setup: directory + create +
        // header are the FAT-allocation writes (10-25 ms each on a worn card)
        // that otherwise land on the first alert of the session, where they
        // can stall the shared SD path mid-encounter. Failure is not fatal —
        // ensureFileReady() runs again lazily on the first append.
        StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
        if (!lock || !ensureFileReady()) {
            Serial.println("[Encounter] WARN: storage warm-up deferred to first alert");
        }
    }
#endif
    enabled_ = true;
}

void V1EncounterLogger::attach(PacketParser& parser) {
    parser_ = &parser;
    parser.setAlertTableObserver(parserObserver, this);
}

bool V1EncounterLogger::beginQualificationSession(uint32_t sessionToken, uint32_t startedAtDutMs,
                                                  uint32_t /*sourceLossCount*/) {
    if (!enabled_) {
        return false;
    }
    qualificationSessionToken_.store(sessionToken, std::memory_order_relaxed);
    qualificationSessionActive_.store(true, std::memory_order_release);
    (void)startedAtDutMs;
    return true;
}

void V1EncounterLogger::endQualificationSession(uint32_t sessionToken, uint32_t endedAtDutMs,
                                                uint32_t sourceLossCount) {
    if (!qualificationSessionActive_.load(std::memory_order_acquire) ||
        qualificationSessionToken_.load(std::memory_order_relaxed) != sessionToken) {
        return;
    }
    qualificationSessionActive_.store(false, std::memory_order_release);
    qualificationSessionToken_.store(0, std::memory_order_relaxed);
}

void V1EncounterLogger::parserObserver(const AlertData* alerts, size_t count, uint32_t nowMs, void* context) {
    if (context) {
        static_cast<V1EncounterLogger*>(context)->onAlertTable(alerts, count, nowMs);
    }
}

void V1EncounterLogger::onAlertTable(const AlertData* alerts, size_t count, uint32_t nowMs) {
    if (!enabled_) {
        return;
    }

    const bool qualificationActive = qualificationSessionActive_.load(std::memory_order_acquire);
    if (!alerts || count == 0) {
        if (!encounterActive_) {
            if (qualificationActive) {
                const std::array<V1EncounterAlertSample, V1_ENCOUNTER_MAX_ALERTS> empty{};
                enqueueSnapshot(makeSnapshot(V1EncounterEvent::End, nowMs, empty, 0));
            }
            return;
        }
        if (qualificationActive) {
            const std::array<V1EncounterAlertSample, V1_ENCOUNTER_MAX_ALERTS> empty{};
            enqueueSnapshot(makeSnapshot(V1EncounterEvent::End, nowMs, empty, 0));
        } else {
            enqueueSnapshot(makeSnapshot(V1EncounterEvent::End, nowMs, lastObservedAlerts_, lastObservedCount_));
        }
        encounterActive_ = false;
        lastObservedCount_ = 0;
        lastEmittedCount_ = 0;
        return;
    }

    const size_t boundedCount = std::min(count, V1_ENCOUNTER_MAX_ALERTS);
    std::array<V1EncounterAlertSample, V1_ENCOUNTER_MAX_ALERTS> current{};
    for (size_t i = 0; i < boundedCount; ++i) {
        current[i] = compactAlert(alerts[i]);
    }
    lastObservedAlerts_ = current;
    lastObservedCount_ = static_cast<uint8_t>(boundedCount);

    if (!encounterActive_) {
        encounterActive_ = true;
        ++encounterId_;
        encounterSampleSeq_ = 0;
        lastEmittedAlerts_ = current;
        lastEmittedCount_ = static_cast<uint8_t>(boundedCount);
        lastSnapshotMs_ = nowMs;
        enqueueSnapshot(makeSnapshot(V1EncounterEvent::Start, nowMs, current, boundedCount));
        return;
    }

    // During a qualification run every complete parser publication is
    // evidence. Outside qualification, retain the existing compact sampler.
    if (qualificationActive) {
        lastEmittedAlerts_ = current;
        lastEmittedCount_ = static_cast<uint8_t>(boundedCount);
        lastSnapshotMs_ = nowMs;
        enqueueSnapshot(makeSnapshot(V1EncounterEvent::Sample, nowMs, current, boundedCount));
        return;
    }

    const bool immediateChanged =
        !sameTable(current, boundedCount, lastEmittedAlerts_, lastEmittedCount_, sameImmediateState);
    const bool sampleChanged =
        !sameTable(current, boundedCount, lastEmittedAlerts_, lastEmittedCount_, sameSampleState);
    const uint32_t elapsedMs = nowMs - lastSnapshotMs_;
    if (!immediateChanged && !(sampleChanged && elapsedMs >= SAMPLE_INTERVAL_MS) && elapsedMs < KEEPALIVE_INTERVAL_MS) {
        return;
    }

    lastEmittedAlerts_ = current;
    lastEmittedCount_ = static_cast<uint8_t>(boundedCount);
    lastSnapshotMs_ = nowMs;
    enqueueSnapshot(makeSnapshot(V1EncounterEvent::Sample, nowMs, current, boundedCount));
}

V1EncounterSnapshot
V1EncounterLogger::makeSnapshot(V1EncounterEvent event, uint32_t nowMs,
                                const std::array<V1EncounterAlertSample, V1_ENCOUNTER_MAX_ALERTS>& alerts,
                                size_t count) {
    V1EncounterSnapshot snapshot;
    snapshot.millisTs = nowMs;
    snapshot.encounterId = encounterId_;
    snapshot.sampleSeq = ++encounterSampleSeq_;
    snapshot.droppedSnapshots = droppedSnapshots_.load(std::memory_order_relaxed);
    snapshot.qualificationSessionToken = qualificationSessionToken_.load(std::memory_order_relaxed);
    snapshot.clockSegment = QualificationClock::segment();
    snapshot.dutMicros = QualificationClock::nowMicros();
    snapshot.event = event;
    snapshot.alertCount = static_cast<uint8_t>(std::min(count, V1_ENCOUNTER_MAX_ALERTS));
    snapshot.alerts = alerts;
    return snapshot;
}

bool V1EncounterLogger::enqueueSnapshot(const V1EncounterSnapshot& snapshot) {
#ifndef UNIT_TEST
    if (!enabled_ || !queue_) {
        return false;
    }
    pendingWrites_.fetch_add(1, std::memory_order_relaxed);
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

bool V1EncounterLogger::formatCsvLine(const V1EncounterSnapshot& snapshot, size_t alertIndex, char* out,
                                      size_t outLen) const {
    if (!out || outLen == 0 || (snapshot.alertCount > 0 && alertIndex >= snapshot.alertCount)) {
        return false;
    }
    const V1EncounterAlertSample emptyAlert{};
    const V1EncounterAlertSample& alert = snapshot.alertCount > 0 ? snapshot.alerts[alertIndex] : emptyAlert;
    const int written = snprintf(
        out, outLen,
        "%lu,%lu,%lu,%s,%u,%u,%s,%lu,%s,%u,%u,%u,%u,%u,%u,%u,%lu,%08lX,%u,%u,%u,%llu,%llu\n",
        static_cast<unsigned long>(snapshot.millisTs), static_cast<unsigned long>(snapshot.encounterId),
        static_cast<unsigned long>(snapshot.sampleSeq), eventName(snapshot.event), static_cast<unsigned>(alert.v1Index),
        static_cast<unsigned>(snapshot.alertCount), encounterBandName(alert.band),
        static_cast<unsigned long>(alert.frequency), directionName(alert.direction),
        static_cast<unsigned>(alert.frontRaw), static_cast<unsigned>(alert.rearRaw),
        static_cast<unsigned>(alert.frontBars), static_cast<unsigned>(alert.rearBars), alert.priority ? 1u : 0u,
        alert.junk ? 1u : 0u, static_cast<unsigned>(alert.photoType),
        static_cast<unsigned long>(snapshot.droppedSnapshots),
        static_cast<unsigned long>(snapshot.qualificationSessionToken),
        alert.valid ? 1u : 0u, static_cast<unsigned>(alert.rawBandBits), alert.isKu ? 1u : 0u,
        static_cast<unsigned long long>(snapshot.clockSegment), static_cast<unsigned long long>(snapshot.dutMicros));
    return written > 0 && static_cast<size_t>(written) < outLen;
}

bool V1EncounterLogger::appendSnapshot(const V1EncounterSnapshot& snapshot) {
#ifdef UNIT_TEST
    const size_t rowCount = snapshot.alertCount > 0 ? snapshot.alertCount : 1;
    for (size_t i = 0; i < rowCount; ++i) {
        if (!formatCsvLine(snapshot, i, lastLineBuf_, sizeof(lastLineBuf_))) {
            return false;
        }
        ++linesWritten_;
    }
    ++snapshotsWritten_;
    return true;
#else
    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock || !ensureFileReady()) {
        return false;
    }

    char line[640];
    const size_t rowCount = snapshot.alertCount > 0 ? snapshot.alertCount : 1;
    for (size_t i = 0; i < rowCount; ++i) {
        if (!formatCsvLine(snapshot, i, line, sizeof(line)) || persistentFile_.print(line) != strlen(line)) {
            persistentFile_.close();
            headerReady_ = false;
            return false;
        }
        ++rowsSinceFlush_;
    }

    const uint32_t nowMs = millis();
    if (snapshot.event == V1EncounterEvent::Start || snapshot.event == V1EncounterEvent::End ||
        rowsSinceFlush_ >= FLUSH_EVERY_ROWS || (nowMs - lastFlushMs_) >= FLUSH_INTERVAL_MS) {
        persistentFile_.flush();
        rowsSinceFlush_ = 0;
        lastFlushMs_ = nowMs;
    }
    return true;
#endif
}

#ifndef UNIT_TEST
bool V1EncounterLogger::ensureWriter() {
    if (!queue_) {
        bool queueInPsram = false;
        queue_ =
            createQueuePreferPsram(ENCOUNTER_QUEUE_DEPTH, sizeof(V1EncounterSnapshot), queueAllocation_, &queueInPsram);
        if (!queue_) {
            return false;
        }
        if (!queueInPsram) {
            Serial.println("[Encounter] WARN: queue using internal SRAM fallback");
        }
    }
    if (!writerTask_) {
        const BaseType_t result =
            createTaskPinnedToCoreInternalStack(writerTaskEntry, "EncounterWriter", ENCOUNTER_WRITER_STACK_SIZE, this,
                                                ENCOUNTER_WRITER_PRIORITY, &writerTask_, 0);
        if (result != pdPASS) {
            return false;
        }
    }
    return true;
}

void V1EncounterLogger::writerTaskEntry(void* context) {
    static_cast<V1EncounterLogger*>(context)->writerTaskLoop();
}

void V1EncounterLogger::writerTaskLoop() {
    while (true) {
        V1EncounterSnapshot snapshot;
        if (xQueueReceive(queue_, &snapshot, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (!appendSnapshot(snapshot)) {
                droppedSnapshots_.fetch_add(1, std::memory_order_relaxed);
            }
            pendingWrites_.fetch_sub(1, std::memory_order_relaxed);
            taskYIELD();
        }
    }
}

bool V1EncounterLogger::ensureFileReady() {
    fs::FS* filesystem = storageManager.getFilesystem();
    if (!filesystem) {
        return false;
    }
    if (!directoryReady_) {
        directoryReady_ = filesystem->mkdir(ENCOUNTER_DIR_PATH) || filesystem->exists(ENCOUNTER_DIR_PATH);
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
        if (persistentFile_.size() == 0 && persistentFile_.print(ENCOUNTER_HEADER) != strlen(ENCOUNTER_HEADER)) {
            persistentFile_.close();
            return false;
        }
        headerReady_ = true;
    }
    return true;
}

#endif

bool V1EncounterLogger::tryDrainQualificationEvidence() {
#ifndef UNIT_TEST
    if (!enabled_ || !queue_) {
        return true;
    }
    if (pendingWrites_.load(std::memory_order_relaxed) > 0 || uxQueueMessagesWaiting(queue_) > 0) {
        return false;
    }
    StorageManager::SDTryLock lock(storageManager.getSDMutex());
    if (!lock || !ensureFileReady()) {
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

void V1EncounterLogger::drainAndClose(uint32_t timeoutMs) {
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
