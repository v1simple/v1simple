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
    "# encounter_schema=3,timebase=millis,monotonic_timebase=esp_timer_us,v1_assignments=raw,no_gps=1,no_speed=1,"
    "alert_table_digest=fnv1a32(count_then_ordered_alert_fields_no_padding)\n"
    "millis,encounter_id,sample_seq,event,v1_index,alert_count,band,frequency_mhz,direction,front_raw,rear_raw,"
    "front_bars,rear_bars,priority,junk,photo_type,dropped_snapshots,qualification_session_token,state_revision,"
    "alert_revision,alert_table_fnv1a32,state_event_seq,state_rx_first_seq,state_rx_last_seq,alert_event_seq,"
    "alert_rx_first_seq,alert_rx_last_seq,valid,raw_band_bits,is_ku,clock_segment,sample_dut_micros,"
    "state_published_dut_micros,alert_published_dut_micros,state_rx_dut_micros,alert_rx_dut_micros\n";
constexpr const char* CAUSAL_TRACE_HEADER =
    "# causal_trace_schema=3,timebase=dut_millis,monotonic_timebase=esp_timer_us,payload_digest=fnv1a32,"
    "payload_digest_bytes=exact_payload_unit_bytes,rx_range=inclusive,"
    "exact_payload_hex=ble_rx_notification_bytes\n"
    "trace_seq,qualification_session_token,stage_dut_millis,rx_dut_millis,stage,outcome,ble_session_generation,"
    "rx_first_seq,rx_last_seq,event_seq,characteristic,payload_unit,payload_length,payload_fnv1a32,packet_id,"
    "parse_ok,state_revision,alert_revision,alert_table_fnv1a32,ble_source_losses,lost_trace_records,clock_segment,"
    "stage_dut_micros,rx_dut_micros,exact_payload_hex\n";
constexpr const char* TRACE_EXPORT_MARKER_FORMAT =
    "# causal_trace_export_schema=1,terminal_trace_seq=%lu,lost_trace_records=%lu,"
    "terminal_encounter_sample_seq=%lu,lost_encounter_snapshots=%lu\n";
constexpr uint32_t SAMPLE_INTERVAL_MS = 250;
constexpr uint32_t KEEPALIVE_INTERVAL_MS = 5000;

#ifndef UNIT_TEST
constexpr UBaseType_t ENCOUNTER_QUEUE_DEPTH = 8;
constexpr UBaseType_t CAUSAL_TRACE_QUEUE_DEPTH = 96;
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

const char* causalStageName(V1CausalStage stage) {
    switch (stage) {
    case V1CausalStage::SessionStart:
        return "SESSION_START";
    case V1CausalStage::Rx:
        return "BLE_RX";
    case V1CausalStage::Framing:
        return "FRAME_BOUNDARY";
    case V1CausalStage::Parse:
        return "PACKET_PARSE";
    case V1CausalStage::PublishState:
        return "STATE_PUBLISH";
    case V1CausalStage::PublishAlerts:
        return "ALERT_TABLE_PUBLISH";
    case V1CausalStage::SessionEnd:
        return "SESSION_END";
    case V1CausalStage::StateBaseline:
        return "STATE_BASELINE";
    case V1CausalStage::AlertTableBaseline:
        return "ALERT_TABLE_BASELINE";
    }
    return "UNKNOWN";
}

const char* causalOutcomeName(V1CausalOutcome outcome) {
    switch (outcome) {
    case V1CausalOutcome::Started:
        return "STARTED";
    case V1CausalOutcome::Accepted:
        return "ACCEPTED";
    case V1CausalOutcome::BufferDropped:
        return "BUFFER_DROPPED";
    case V1CausalOutcome::Parsed:
        return "PARSED";
    case V1CausalOutcome::Rejected:
        return "REJECTED";
    case V1CausalOutcome::Handled:
        return "HANDLED";
    case V1CausalOutcome::Published:
        return "PUBLISHED";
    case V1CausalOutcome::Ended:
        return "ENDED";
    case V1CausalOutcome::ResyncDiscardedPrefix:
        return "RESYNC_DISCARDED_PREFIX";
    case V1CausalOutcome::ResyncNoStart:
        return "RESYNC_NO_START";
    case V1CausalOutcome::ResyncZeroLength:
        return "RESYNC_ZERO_LENGTH";
    case V1CausalOutcome::ResyncTooLarge:
        return "RESYNC_TOO_LARGE";
    case V1CausalOutcome::ResyncMissingEnd:
        return "RESYNC_MISSING_END";
    case V1CausalOutcome::SessionClosedIncomplete:
        return "SESSION_CLOSED_INCOMPLETE";
    case V1CausalOutcome::Retained:
        return "RETAINED";
    }
    return "UNKNOWN";
}

const char* causalPayloadUnitName(V1CausalPayloadUnit unit) {
    switch (unit) {
    case V1CausalPayloadUnit::Notification:
        return "NOTIFICATION";
    case V1CausalPayloadUnit::Frame:
        return "FRAME";
    case V1CausalPayloadUnit::Candidate:
        return "CANDIDATE";
    case V1CausalPayloadUnit::None:
    default:
        return "NONE";
    }
}
} // namespace

V1EncounterLogger v1EncounterLogger;

void V1EncounterLogger::setBootId(uint32_t bootId, uint32_t bootToken) {
    if (bootToken != 0) {
        snprintf(csvPathBuf_, sizeof(csvPathBuf_), "/encounters/encounters_%lu-%08lx.csv",
                 static_cast<unsigned long>(bootId), static_cast<unsigned long>(bootToken));
        snprintf(causalTracePathBuf_, sizeof(causalTracePathBuf_), "/encounters/causal_trace_%lu-%08lx.csv",
                 static_cast<unsigned long>(bootId), static_cast<unsigned long>(bootToken));
    } else {
        snprintf(csvPathBuf_, sizeof(csvPathBuf_), "/encounters/encounters_%lu.csv",
                 static_cast<unsigned long>(bootId));
        snprintf(causalTracePathBuf_, sizeof(causalTracePathBuf_), "/encounters/causal_trace_%lu.csv",
                 static_cast<unsigned long>(bootId));
    }
#ifndef UNIT_TEST
    headerReady_ = false;
    traceHeaderReady_ = false;
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
    droppedTraceRecords_.store(0, std::memory_order_relaxed);
    pendingTraceWrites_.store(0, std::memory_order_relaxed);
    qualificationSessionToken_.store(0, std::memory_order_relaxed);
    qualificationSessionActive_.store(false, std::memory_order_relaxed);
    traceSeq_ = 0;

    if (!sdAvailable) {
        return;
    }

#ifndef UNIT_TEST
    directoryReady_ = false;
    headerReady_ = false;
    traceHeaderReady_ = false;
    rowsSinceFlush_ = 0;
    lastFlushMs_ = 0;
    traceRowsSinceFlush_ = 0;
    traceLastFlushMs_ = 0;
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
                                                  uint32_t sourceLossCount) {
    V1SemanticRevisionEvidence retainedEvidence;
    const V1SemanticRevisionEvidence* retainedEvidencePtr = nullptr;
    if (parser_) {
        // Copy once so both baselines describe one coherent parser snapshot.
        retainedEvidence = parser_->getCausalEvidence();
        retainedEvidencePtr = &retainedEvidence;
    }
    return beginQualificationSessionWithEvidence(sessionToken, startedAtDutMs, sourceLossCount, retainedEvidencePtr);
}

bool V1EncounterLogger::beginQualificationSessionWithEvidence(uint32_t sessionToken, uint32_t startedAtDutMs,
                                                              uint32_t sourceLossCount,
                                                              const V1SemanticRevisionEvidence* retainedEvidence) {
    if (!enabled_ || sessionToken == 0) {
        return false;
    }
#ifndef UNIT_TEST
    // The exact-payload trace queue is sizeable. Keep it out of ordinary
    // runtime and allocate it only when qualification evidence is requested.
    traceQueueRequired_.store(true, std::memory_order_release);
    if (!ensureTraceQueue()) {
        Serial.println("[Encounter] WARN: causal trace queue unavailable");
        return false;
    }
#endif
    qualificationSessionToken_.store(sessionToken, std::memory_order_relaxed);
    qualificationSessionActive_.store(true, std::memory_order_release);

    V1CausalTraceRecord record;
    record.stage = V1CausalStage::SessionStart;
    record.outcome = V1CausalOutcome::Started;
    record.stageDutMillis = startedAtDutMs;
    record.identity.dutMillis = startedAtDutMs;
    record.identity.clockSegment = QualificationClock::segment();
    record.stageDutMicros = QualificationClock::nowMicros();
    record.identity.dutMicros = record.stageDutMicros;
    record.alertTableDigest = v1AlertTableFnv1a32(nullptr, 0);
    record.sourceLossCount = sourceLossCount;
    recordCausalTrace(record);

    if (retainedEvidence) {
        recordRetainedBaselines(*retainedEvidence, startedAtDutMs, sourceLossCount);
    }
    return true;
}

void V1EncounterLogger::recordRetainedBaselines(const V1SemanticRevisionEvidence& evidence, uint32_t startedAtDutMs,
                                                uint32_t sourceLossCount) {
    auto recordBaseline = [&](V1CausalStage stage, const V1CausalIdentity& source) {
        V1CausalTraceRecord record;
        record.identity = source;
        record.stageDutMillis = startedAtDutMs;
        record.stageDutMicros = QualificationClock::nowMicros();
        record.stage = stage;
        record.outcome = V1CausalOutcome::Retained;
        record.payloadUnit = V1CausalPayloadUnit::Frame;
        record.parseOk = true;
        record.stateRevision = evidence.stateRevision;
        record.alertRevision = evidence.alertRevision;
        record.alertTableDigest = evidence.alertTableDigest;
        record.sourceLossCount = sourceLossCount;
        recordCausalTrace(record);
    };

    // eventSeq is the parser-owned join key. A revision with an all-zero
    // source can come from a synthetic/direct parser caller and must not be
    // represented as observed BLE provenance.
    if (evidence.stateRevision != 0 && evidence.stateSource.eventSeq != 0) {
        recordBaseline(V1CausalStage::StateBaseline, evidence.stateSource);
    }
    if (evidence.alertRevision != 0 && evidence.alertSource.eventSeq != 0) {
        recordBaseline(V1CausalStage::AlertTableBaseline, evidence.alertSource);
    }
}

#ifdef UNIT_TEST
void V1EncounterLogger::testBeginQualificationSession(uint32_t sessionToken, uint32_t startedAtDutMs,
                                                      const V1SemanticRevisionEvidence& retainedEvidence,
                                                      uint32_t sourceLossCount) {
    beginQualificationSessionWithEvidence(sessionToken, startedAtDutMs, sourceLossCount, &retainedEvidence);
}
#endif

void V1EncounterLogger::endQualificationSession(uint32_t sessionToken, uint32_t endedAtDutMs,
                                                uint32_t sourceLossCount) {
    if (!qualificationSessionActive_.load(std::memory_order_acquire) ||
        qualificationSessionToken_.load(std::memory_order_relaxed) != sessionToken) {
        return;
    }
    V1CausalTraceRecord record;
    record.stage = V1CausalStage::SessionEnd;
    record.outcome = V1CausalOutcome::Ended;
    record.stageDutMillis = endedAtDutMs;
    record.identity.dutMillis = endedAtDutMs;
    record.identity.clockSegment = QualificationClock::segment();
    record.stageDutMicros = QualificationClock::nowMicros();
    record.identity.dutMicros = record.stageDutMicros;
    record.sourceLossCount = sourceLossCount;
    record.alertTableDigest = parser_ ? parser_->getCausalEvidence().alertTableDigest : v1AlertTableFnv1a32(nullptr, 0);
    if (parser_) {
        record.stateRevision = parser_->getCausalEvidence().stateRevision;
        record.alertRevision = parser_->getCausalEvidence().alertRevision;
    }
    recordCausalTrace(record);
    qualificationSessionActive_.store(false, std::memory_order_release);
    qualificationSessionToken_.store(0, std::memory_order_relaxed);
}

void V1EncounterLogger::recordCausalTrace(const V1CausalTraceRecord& record, const uint8_t* exactPayload,
                                          size_t exactPayloadLength) {
    if (!enabled_ || !qualificationSessionActive_.load(std::memory_order_acquire)) {
        return;
    }
    V1PersistedCausalTraceRecord persisted;
    persisted.traceSeq = ++traceSeq_;
    persisted.qualificationSessionToken = qualificationSessionToken_.load(std::memory_order_relaxed);
    persisted.lostTraceRecords = droppedTraceRecords_.load(std::memory_order_relaxed);
    persisted.record = record;
    if (exactPayload && exactPayloadLength > 0) {
        persisted.exactPayloadLength =
            static_cast<uint16_t>(std::min(exactPayloadLength, V1PersistedCausalTraceRecord::MAX_EXACT_PAYLOAD));
        memcpy(persisted.exactPayload, exactPayload, persisted.exactPayloadLength);
    }
    if (persisted.record.stageDutMillis == 0) {
        persisted.record.stageDutMillis = persisted.record.identity.dutMillis;
    }
    if (persisted.record.identity.clockSegment == 0) {
        persisted.record.identity.clockSegment = QualificationClock::segment();
    }
    if (persisted.record.stageDutMicros == 0) {
        persisted.record.stageDutMicros = persisted.record.identity.dutMicros != 0 ? persisted.record.identity.dutMicros
                                                                                   : QualificationClock::nowMicros();
    }
    if (persisted.record.stage == V1CausalStage::Rx && persisted.record.identity.dutMicros == 0) {
        persisted.record.identity.dutMicros = persisted.record.stageDutMicros;
    }
    enqueueTrace(persisted);
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
    if (parser_) {
        const V1SemanticRevisionEvidence& evidence = parser_->getCausalEvidence();
        snapshot.stateRevision = evidence.stateRevision;
        snapshot.alertRevision = evidence.alertRevision;
        snapshot.alertTableDigest = evidence.alertTableDigest;
        snapshot.stateSource = evidence.stateSource;
        snapshot.alertSource = evidence.alertSource;
        snapshot.statePublishedDutMicros = evidence.statePublishedDutMicros;
        snapshot.alertPublishedDutMicros = evidence.alertPublishedDutMicros;
    } else {
        snapshot.alertTableDigest = v1AlertTableFnv1a32(nullptr, 0);
    }
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

bool V1EncounterLogger::enqueueTrace(const V1PersistedCausalTraceRecord& record) {
#ifndef UNIT_TEST
    if (!enabled_) {
        return false;
    }
    QueueHandle_t traceQueue = traceQueue_.load(std::memory_order_acquire);
    if (!traceQueue) {
        droppedTraceRecords_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    pendingTraceWrites_.fetch_add(1, std::memory_order_relaxed);
    if (xQueueSend(traceQueue, &record, 0) != pdTRUE) {
        pendingTraceWrites_.fetch_sub(1, std::memory_order_relaxed);
        droppedTraceRecords_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
#else
    return appendTrace(record);
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
        "%lu,%lu,%lu,%s,%u,%u,%s,%lu,%s,%u,%u,%u,%u,%u,%u,%u,%lu,%08lX,%lu,%lu,%08lX,%lu,%lu,%lu,"
        "%lu,%lu,%lu,%u,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu\n",
        static_cast<unsigned long>(snapshot.millisTs), static_cast<unsigned long>(snapshot.encounterId),
        static_cast<unsigned long>(snapshot.sampleSeq), eventName(snapshot.event), static_cast<unsigned>(alert.v1Index),
        static_cast<unsigned>(snapshot.alertCount), encounterBandName(alert.band),
        static_cast<unsigned long>(alert.frequency), directionName(alert.direction),
        static_cast<unsigned>(alert.frontRaw), static_cast<unsigned>(alert.rearRaw),
        static_cast<unsigned>(alert.frontBars), static_cast<unsigned>(alert.rearBars), alert.priority ? 1u : 0u,
        alert.junk ? 1u : 0u, static_cast<unsigned>(alert.photoType),
        static_cast<unsigned long>(snapshot.droppedSnapshots),
        static_cast<unsigned long>(snapshot.qualificationSessionToken),
        static_cast<unsigned long>(snapshot.stateRevision), static_cast<unsigned long>(snapshot.alertRevision),
        static_cast<unsigned long>(snapshot.alertTableDigest),
        static_cast<unsigned long>(snapshot.stateSource.eventSeq),
        static_cast<unsigned long>(snapshot.stateSource.rxFirstSeq),
        static_cast<unsigned long>(snapshot.stateSource.rxLastSeq),
        static_cast<unsigned long>(snapshot.alertSource.eventSeq),
        static_cast<unsigned long>(snapshot.alertSource.rxFirstSeq),
        static_cast<unsigned long>(snapshot.alertSource.rxLastSeq), alert.valid ? 1u : 0u,
        static_cast<unsigned>(alert.rawBandBits), alert.isKu ? 1u : 0u,
        static_cast<unsigned long long>(snapshot.clockSegment), static_cast<unsigned long long>(snapshot.dutMicros),
        static_cast<unsigned long long>(snapshot.statePublishedDutMicros),
        static_cast<unsigned long long>(snapshot.alertPublishedDutMicros),
        static_cast<unsigned long long>(snapshot.stateSource.dutMicros),
        static_cast<unsigned long long>(snapshot.alertSource.dutMicros));
    return written > 0 && static_cast<size_t>(written) < outLen;
}

bool V1EncounterLogger::formatTraceCsvLine(const V1PersistedCausalTraceRecord& persisted, char* out,
                                           size_t outLen) const {
    if (!out || outLen == 0) {
        return false;
    }
    const V1CausalTraceRecord& record = persisted.record;
    const V1CausalIdentity& identity = record.identity;
    const int written = snprintf(
        out, outLen,
        "%lu,%08lX,%lu,%lu,%s,%s,%lu,%lu,%lu,%lu,%04X,%s,%u,%08lX,%02X,%u,%lu,%lu,%08lX,%lu,%lu,%llu,%llu,"
        "%llu,",
        static_cast<unsigned long>(persisted.traceSeq), static_cast<unsigned long>(persisted.qualificationSessionToken),
        static_cast<unsigned long>(record.stageDutMillis), static_cast<unsigned long>(identity.dutMillis),
        causalStageName(record.stage), causalOutcomeName(record.outcome),
        static_cast<unsigned long>(identity.bleSessionGeneration), static_cast<unsigned long>(identity.rxFirstSeq),
        static_cast<unsigned long>(identity.rxLastSeq), static_cast<unsigned long>(identity.eventSeq),
        static_cast<unsigned>(identity.characteristic), causalPayloadUnitName(record.payloadUnit),
        static_cast<unsigned>(identity.payloadLength), static_cast<unsigned long>(identity.payloadDigest),
        static_cast<unsigned>(record.packetId), record.parseOk ? 1u : 0u,
        static_cast<unsigned long>(record.stateRevision), static_cast<unsigned long>(record.alertRevision),
        static_cast<unsigned long>(record.alertTableDigest), static_cast<unsigned long>(record.sourceLossCount),
        static_cast<unsigned long>(persisted.lostTraceRecords), static_cast<unsigned long long>(identity.clockSegment),
        static_cast<unsigned long long>(record.stageDutMicros), static_cast<unsigned long long>(identity.dutMicros));
    if (written <= 0 || static_cast<size_t>(written) >= outLen) {
        return false;
    }

    size_t used = static_cast<size_t>(written);
    const size_t payloadLength = std::min<size_t>(persisted.exactPayloadLength, sizeof(persisted.exactPayload));
    if (used + payloadLength * 2 + 2 > outLen) {
        return false;
    }
    static constexpr char kHexDigits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < payloadLength; ++i) {
        out[used++] = kHexDigits[persisted.exactPayload[i] >> 4];
        out[used++] = kHexDigits[persisted.exactPayload[i] & 0x0F];
    }
    out[used++] = '\n';
    out[used] = '\0';
    return true;
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

bool V1EncounterLogger::appendTrace(const V1PersistedCausalTraceRecord& record) {
#ifdef UNIT_TEST
    if (!formatTraceCsvLine(record, lastTraceLineBuf_, sizeof(lastTraceLineBuf_))) {
        return false;
    }
    if (tracesWritten_ < testTraceRecords_.size()) {
        testTraceRecords_[tracesWritten_] = record;
    }
    ++tracesWritten_;
    return true;
#else
    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock || !ensureTraceFileReady()) {
        return false;
    }
    char line[1024];
    if (!formatTraceCsvLine(record, line, sizeof(line)) || traceFile_.print(line) != strlen(line)) {
        traceFile_.close();
        traceHeaderReady_ = false;
        return false;
    }
    ++traceRowsSinceFlush_;
    const uint32_t nowMs = millis();
    if (record.record.stage == V1CausalStage::SessionStart || record.record.stage == V1CausalStage::SessionEnd ||
        traceRowsSinceFlush_ >= FLUSH_EVERY_ROWS || (nowMs - traceLastFlushMs_) >= FLUSH_INTERVAL_MS) {
        traceFile_.flush();
        traceRowsSinceFlush_ = 0;
        traceLastFlushMs_ = nowMs;
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

bool V1EncounterLogger::ensureTraceQueue() {
    if (traceQueue_.load(std::memory_order_acquire)) {
        return true;
    }
    bool queueInPsram = false;
    QueueHandle_t traceQueue = createQueuePreferPsram(CAUSAL_TRACE_QUEUE_DEPTH, sizeof(V1PersistedCausalTraceRecord),
                                                      traceQueueAllocation_, &queueInPsram);
    if (traceQueue && !queueInPsram) {
        Serial.println("[Encounter] WARN: causal trace queue using internal SRAM fallback");
    }
    traceQueue_.store(traceQueue, std::memory_order_release);
    return traceQueue != nullptr;
}

void V1EncounterLogger::writerTaskEntry(void* context) {
    static_cast<V1EncounterLogger*>(context)->writerTaskLoop();
}

void V1EncounterLogger::writerTaskLoop() {
    while (true) {
        // Keep the small, higher-rate causal records moving without starving
        // the larger encounter snapshots. Both producers remain zero-wait.
        QueueHandle_t traceQueue = traceQueue_.load(std::memory_order_acquire);
        if (traceQueue) {
            for (uint8_t i = 0; i < 16; ++i) {
                V1PersistedCausalTraceRecord trace;
                if (xQueueReceive(traceQueue, &trace, 0) != pdTRUE) {
                    break;
                }
                if (!appendTrace(trace)) {
                    droppedTraceRecords_.fetch_add(1, std::memory_order_relaxed);
                }
                pendingTraceWrites_.fetch_sub(1, std::memory_order_relaxed);
            }
        }

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

bool V1EncounterLogger::ensureTraceFileReady() {
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
    if (!traceFile_) {
        traceFile_ = filesystem->open(causalTracePathBuf_, FILE_APPEND, true);
        if (!traceFile_) {
            return false;
        }
    }
    if (!traceHeaderReady_) {
        if (traceFile_.size() == 0 && traceFile_.print(CAUSAL_TRACE_HEADER) != strlen(CAUSAL_TRACE_HEADER)) {
            traceFile_.close();
            return false;
        }
        traceHeaderReady_ = true;
    }
    return true;
}
#endif

bool V1EncounterLogger::tryDrainQualificationEvidence() {
#ifndef UNIT_TEST
    if (!enabled_ || !queue_) {
        return true;
    }
    QueueHandle_t traceQueue = traceQueue_.load(std::memory_order_acquire);
    if (!traceQueue) {
        return !traceQueueRequired_.load(std::memory_order_acquire);
    }
    if (pendingWrites_.load(std::memory_order_relaxed) > 0 || pendingTraceWrites_.load(std::memory_order_relaxed) > 0 ||
        uxQueueMessagesWaiting(queue_) > 0 || uxQueueMessagesWaiting(traceQueue) > 0) {
        return false;
    }
    StorageManager::SDTryLock lock(storageManager.getSDMutex());
    if (!lock || !ensureFileReady() || !ensureTraceFileReady()) {
        return false;
    }
    char marker[224];
    const int markerLen =
        snprintf(marker, sizeof(marker), TRACE_EXPORT_MARKER_FORMAT, static_cast<unsigned long>(traceSeq_),
                 static_cast<unsigned long>(droppedTraceRecords_.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(encounterSampleSeq_),
                 static_cast<unsigned long>(droppedSnapshots_.load(std::memory_order_relaxed)));
    if (markerLen <= 0 || static_cast<size_t>(markerLen) >= sizeof(marker) ||
        traceFile_.print(marker) != static_cast<size_t>(markerLen)) {
        traceFile_.close();
        traceHeaderReady_ = false;
        return false;
    }
    persistentFile_.flush();
    traceFile_.flush();
    persistentFile_.close();
    traceFile_.close();
    rowsSinceFlush_ = 0;
    traceRowsSinceFlush_ = 0;
    lastFlushMs_ = millis();
    traceLastFlushMs_ = lastFlushMs_;
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
    QueueHandle_t traceQueue = traceQueue_.load(std::memory_order_acquire);
    const uint32_t startMs = millis();
    while (pendingWrites_.load(std::memory_order_relaxed) > 0 ||
           pendingTraceWrites_.load(std::memory_order_relaxed) > 0 || uxQueueMessagesWaiting(queue_) > 0 ||
           (traceQueue && uxQueueMessagesWaiting(traceQueue) > 0)) {
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
    if (lock && traceFile_) {
        traceFile_.flush();
        traceFile_.close();
    }
#else
    (void)timeoutMs;
#endif
}
