#include "ble_queue_module.h"
#include "v1_packet_framing.h"

#include <algorithm>
#include <cstring>

#include "config.h"
#include "modules/health/health_journal.h"

#ifndef UNIT_TEST
#include "modules/display/display_preview_module.h"
#include "modules/power/power_module.h"
#include "v1_profiles.h"
#endif

// Maximum bytes in the BLE RX frame-reassembly buffer.
static constexpr size_t RX_BUFFER_MAX = 1024;
static constexpr size_t RX_COMPACT_THRESHOLD = RX_BUFFER_MAX / 2;

// The RX buffer reassembles a framed byte stream across BLE notifications. Queue
// admission is the ownership boundary: after tryOnNotify() returns true, process()
// must either consume that whole notification or leave it queued. It may never
// dequeue a notification and then discard it because this staging buffer is full.
bool BleQueueModule::begin(V1BLEClient* bleClient, PacketParser* parserPtr, V1ProfileManager* profileMgr,
                           DisplayPreviewModule* previewModule, PowerModule* powerModule, Config cfg) {
    ble_ = bleClient;
    parser_ = parserPtr;
    profiles_ = profileMgr;
    preview_ = previewModule;
    power_ = powerModule;
    config_ = cfg;

    if (queueHandle_ != nullptr) {
        end();
    }

    rxBuffer_.clear();
    rxIngressSequences_.clear();
    rxDiscontinuities_.clear();
    clearLongRxState();
    rxReadPos_ = 0;
    lastRxMillis_ = 0;
    lastNotifyTsMs_ = 0;
    hadSuccessfulParse_ = false;
    backpressureActive_ = false;
    lastDequeuedIngressSequence_ = 0;
    pendingStreamDiscontinuity_ = false;
    tooLargeWarningLog_ = BleLogRateLimitState{};
    missingEndWarningLog_ = BleLogRateLimitState{};
    sessionGeneration_.store(0, std::memory_order_relaxed);
    acceptNotifications_.store(false, std::memory_order_release);

    queueHandle_ = xQueueCreate(config_.queueDepth, sizeof(BLEDataPacket));
    rxBufferReady_ = false;
    if (!queueHandle_) {
        Serial.printf("[BLE_QUEUE] FATAL: queue allocation failed (depth=%u item=%u)\n",
                      static_cast<unsigned>(config_.queueDepth), static_cast<unsigned>(sizeof(BLEDataPacket)));
        backpressureActive_ = false;
        return false;
    }

    const size_t desiredRxCap = std::max(config_.rxBufferCap, RX_BUFFER_MAX);
    rxBuffer_.reserve(desiredRxCap);
    rxIngressSequences_.reserve(desiredRxCap);
    rxDiscontinuities_.reserve(desiredRxCap);
    if (rxBuffer_.capacity() < desiredRxCap || rxIngressSequences_.capacity() < desiredRxCap ||
        rxDiscontinuities_.capacity() < desiredRxCap) {
        Serial.printf("[BLE_QUEUE] FATAL: RX buffer reserve failed (cap=%u have=%u)\n",
                      static_cast<unsigned>(desiredRxCap), static_cast<unsigned>(rxBuffer_.capacity()));
        vQueueDelete(queueHandle_);
        queueHandle_ = nullptr;
        backpressureActive_ = false;
        return false;
    }
    rxBufferReady_ = true;
    backpressureActive_ = false;
    return true;
}

void BleQueueModule::end() {
    acceptNotifications_.store(false, std::memory_order_release);
    if (queueHandle_ != nullptr) {
        vQueueDelete(queueHandle_);
        queueHandle_ = nullptr;
    }
    rxBufferReady_ = false;
    clearRxState();
    clearLongRxState();
    backpressureActive_ = false;
}

void BleQueueModule::openSession(uint32_t sessionGeneration) {
    // Purge again before opening: an outgoing callback may have completed its
    // queue send after closeSession() drained the queue.
    closeSession();
    sessionGeneration_.store(sessionGeneration, std::memory_order_release);
    acceptNotifications_.store(true, std::memory_order_release);
}

void BleQueueModule::closeSession() {
    acceptNotifications_.store(false, std::memory_order_release);

    BLEDataPacket discarded;
    while (queueHandle_ && xQueueReceive(queueHandle_, &discarded, 0) == pdTRUE) {
        // Receiving each queued item is the complete drain operation.
    }

    clearRxState();
    clearLongRxState();
    lastRxMillis_ = 0;
    lastNotifyTsMs_ = 0;
    hadSuccessfulParse_ = false;
    backpressureActive_ = false;
    lastDequeuedIngressSequence_ = 0;
    pendingStreamDiscontinuity_ = false;
}

void BleQueueModule::onNotify(const uint8_t* data, size_t length, uint16_t charUUID, uint32_t sessionGeneration,
                              uint32_t callbackMillis, uint32_t ingressSequence) {
    (void)tryOnNotify(data, length, charUUID, sessionGeneration, callbackMillis, ingressSequence);
}

bool BleQueueModule::tryOnNotify(const uint8_t* data, size_t length, uint16_t charUUID, uint32_t sessionGeneration,
                                 uint32_t callbackMillis, uint32_t ingressSequence) {
    if (!queueHandle_ || !acceptNotifications_.load(std::memory_order_acquire) ||
        sessionGeneration != sessionGeneration_.load(std::memory_order_acquire))
        return false;

    if (length > 0 && length <= sizeof(BLEDataPacket::data)) {
        BLEDataPacket pkt;
        memcpy(pkt.data, data, length);
        pkt.length = length;
        pkt.charUUID = charUUID;
        pkt.tsMs = callbackMillis;
        pkt.sessionGeneration = sessionGeneration;
        pkt.ingressSequence = ingressSequence;

        // closeSession() can race this callback between its first admission
        // check and packet construction. Recheck before publishing; if it
        // races after this point, process() rejects the stamped generation.
        if (!acceptNotifications_.load(std::memory_order_acquire) ||
            sessionGeneration != sessionGeneration_.load(std::memory_order_acquire)) {
            return false;
        }

        BaseType_t result = xQueueSend(queueHandle_, &pkt, 0);
        if (result != pdTRUE) {
            HealthCounters::recordInputDrop();
        }
        return result == pdTRUE;
    }
    return false;
}

#ifdef UNIT_TEST
bool BleQueueModule::enqueueStampedForTest(const uint8_t* data, size_t length, uint16_t charUUID,
                                           uint32_t sessionGeneration) {
    if (!queueHandle_ || !data || length == 0 || length > sizeof(BLEDataPacket::data)) {
        return false;
    }
    BLEDataPacket pkt{};
    memcpy(pkt.data, data, length);
    pkt.length = length;
    pkt.charUUID = charUUID;
    pkt.tsMs = millis();
    pkt.sessionGeneration = sessionGeneration;
    pkt.ingressSequence = ble_ ? ble_->noteV1NotificationIngress() : 0;
    return xQueueSend(queueHandle_, &pkt, 0) == pdTRUE;
}
#endif

void BleQueueModule::clearRxState() {
    rxBuffer_.clear();
    rxIngressSequences_.clear();
    rxDiscontinuities_.clear();
    rxReadPos_ = 0;
}

void BleQueueModule::compactRxState() {
    if (rxReadPos_ == 0) {
        return;
    }
    const size_t shift = std::min(rxReadPos_, rxBuffer_.size());
    if (shift >= rxBuffer_.size()) {
        clearRxState();
        return;
    }
    const size_t unread = rxBuffer_.size() - shift;
    memmove(rxBuffer_.data(), rxBuffer_.data() + shift, unread);
    rxBuffer_.resize(unread);
    memmove(rxIngressSequences_.data(), rxIngressSequences_.data() + shift,
            unread * sizeof(rxIngressSequences_[0]));
    rxIngressSequences_.resize(unread);
    memmove(rxDiscontinuities_.data(), rxDiscontinuities_.data() + shift,
            unread * sizeof(rxDiscontinuities_[0]));
    rxDiscontinuities_.resize(unread);
    rxReadPos_ = 0;
}

bool BleQueueModule::appendRxPacket(const BLEDataPacket& packet, bool beginsAfterDiscontinuity) {
    return appendRxBytes(packet.data, packet.length, packet.ingressSequence, beginsAfterDiscontinuity);
}

bool BleQueueModule::appendRxBytes(const uint8_t* data, size_t length, uint32_t ingressSequence,
                                   bool beginsAfterDiscontinuity) {
    if (!data || length == 0) {
        return false;
    }
    if (rxReadPos_ > 0) {
        const size_t unread = rxReadPos_ < rxBuffer_.size() ? rxBuffer_.size() - rxReadPos_ : 0;
        if (rxBuffer_.size() >= RX_BUFFER_MAX || (unread + length) > RX_BUFFER_MAX) {
            compactRxState();
        }
    }
    const size_t unread = rxReadPos_ < rxBuffer_.size() ? rxBuffer_.size() - rxReadPos_ : 0;
    if (unread >= RX_BUFFER_MAX || (unread + length) > RX_BUFFER_MAX) {
        return false;
    }
    const size_t begin = rxBuffer_.size();
    rxBuffer_.resize(begin + length);
    memcpy(rxBuffer_.data() + begin, data, length);
    rxIngressSequences_.resize(begin + length);
    std::fill(rxIngressSequences_.begin() + begin, rxIngressSequences_.end(), ingressSequence);
    rxDiscontinuities_.resize(begin + length, 0);
    if (beginsAfterDiscontinuity) {
        rxDiscontinuities_[begin] = 1;
    }
    return true;
}

bool BleQueueModule::ingressWouldBeDiscontinuous(uint32_t ingressSequence) const {
    if (lastDequeuedIngressSequence_ == 0) {
        return false;
    }
    if (ingressSequence == 0) {
        return true;
    }
    if (ingressSequence == lastDequeuedIngressSequence_) {
        return false;
    }
    const uint32_t expected =
        (lastDequeuedIngressSequence_ == UINT32_MAX) ? 1u : (lastDequeuedIngressSequence_ + 1u);
    return ingressSequence != expected;
}

void BleQueueModule::acceptIngressSequence(uint32_t ingressSequence) {
    lastDequeuedIngressSequence_ = ingressSequence;
}

void BleQueueModule::markAlertStreamDiscontinuous() {
    if (parser_) {
        parser_->markAlertStreamDiscontinuous();
    }
}

void BleQueueModule::applyPendingDiscontinuityIfStreamDrained() {
    if (pendingStreamDiscontinuity_ && rxReadPos_ >= rxBuffer_.size() && !longRxComplete()) {
        markAlertStreamDiscontinuous();
        pendingStreamDiscontinuity_ = false;
    }
}

void BleQueueModule::clearLongRxState() {
    longRx_ = LongRxAssembly{};
}

bool BleQueueModule::longRxComplete() const {
    if (longRx_.count == 0 || longRx_.count > MAX_LONG_CHUNKS) {
        return false;
    }
    const uint16_t required = static_cast<uint16_t>((uint16_t{1} << longRx_.count) - 1u);
    return longRx_.presentMask == required;
}

bool BleQueueModule::appendLongRxChunk(const BLEDataPacket& packet, bool beginsAfterDiscontinuity) {
    // The prefix is included in the 20-byte ATT value, leaving 19 bytes of
    // framed ESP data per chunk.
    if (packet.length < 2 || packet.length > (MAX_LONG_CHUNK_PAYLOAD + 1u)) {
        clearLongRxState();
        return false;
    }

    const uint8_t prefix = packet.data[0];
    const uint8_t index = static_cast<uint8_t>((prefix >> 4) & 0x0F);
    const uint8_t count = static_cast<uint8_t>(prefix & 0x0F);
    if (index == 0 || count == 0 || count > MAX_LONG_CHUNKS || index > count) {
        clearLongRxState();
        return false;
    }

    if (longRx_.count != 0 && longRx_.count != count) {
        beginsAfterDiscontinuity = true;
        clearLongRxState();
    }
    if (longRx_.count == 0) {
        longRx_.count = count;
        longRx_.firstIngressSequence = packet.ingressSequence;
        longRx_.streamDiscontinuity = beginsAfterDiscontinuity;
    } else {
        longRx_.streamDiscontinuity |= beginsAfterDiscontinuity;
    }

    const uint16_t bit = static_cast<uint16_t>(uint16_t{1} << (index - 1u));
    if ((longRx_.presentMask & bit) != 0) {
        // Match the vendor builders' ambiguity handling: a repeated index may
        // be the first observed piece of a new packet with the same count.
        // Never combine it with retained chunks from the prior candidate.
        clearLongRxState();
        longRx_.count = count;
        longRx_.firstIngressSequence = packet.ingressSequence;
        longRx_.streamDiscontinuity = true;
    }

    const size_t slot = static_cast<size_t>(index - 1u);
    const size_t payloadLength = packet.length - 1u;
    memcpy(longRx_.payloads[slot].data(), packet.data + 1, payloadLength);
    longRx_.lengths[slot] = static_cast<uint8_t>(payloadLength);
    longRx_.presentMask = static_cast<uint16_t>(longRx_.presentMask | bit);
    longRx_.latestTimestampMs = packet.tsMs;
    return true;
}

bool BleQueueModule::flushCompleteLongRx() {
    if (!longRxComplete()) {
        return false;
    }

    size_t totalLength = 0;
    for (size_t slot = 0; slot < longRx_.count; ++slot) {
        totalLength += longRx_.lengths[slot];
    }
    if (totalLength == 0 || totalLength > 512) {
        clearLongRxState();
        return false;
    }

    // Reserve the whole assembled packet before mutating the stream. This is
    // the long-packet equivalent of queue-head ownership for short packets.
    const size_t unread = rxReadPos_ < rxBuffer_.size() ? rxBuffer_.size() - rxReadPos_ : 0;
    if (rxReadPos_ > 0 && (rxBuffer_.size() >= RX_BUFFER_MAX || unread + totalLength > RX_BUFFER_MAX)) {
        compactRxState();
    }
    const size_t compactUnread = rxReadPos_ < rxBuffer_.size() ? rxBuffer_.size() - rxReadPos_ : 0;
    if (compactUnread + totalLength > RX_BUFFER_MAX) {
        return false;
    }

    const uint32_t ingressSequence = longRx_.firstIngressSequence;
    const bool streamDiscontinuity = longRx_.streamDiscontinuity || pendingStreamDiscontinuity_;
    for (size_t slot = 0; slot < longRx_.count; ++slot) {
        if (!appendRxBytes(longRx_.payloads[slot].data(), longRx_.lengths[slot], ingressSequence,
                           streamDiscontinuity && slot == 0)) {
            // Capacity was proven above; preserve the completed candidate if a
            // future implementation changes that invariant.
            return false;
        }
    }
    pendingStreamDiscontinuity_ = false;
    clearLongRxState();
    return true;
}

void BleQueueModule::refreshBackpressureState() {
    const size_t unreadBytes = (rxReadPos_ < rxBuffer_.size()) ? (rxBuffer_.size() - rxReadPos_) : 0;
    const UBaseType_t queueDepth = queueHandle_ ? uxQueueMessagesWaiting(queueHandle_) : 0;
    const size_t queuePressureThreshold = std::max<size_t>(4, config_.queueDepth / 4);
    static constexpr size_t RX_BACKPRESSURE_BYTES = 192;
    backpressureActive_ =
        (unreadBytes >= RX_BACKPRESSURE_BYTES) || (static_cast<size_t>(queueDepth) >= queuePressureThreshold);
}

void BleQueueModule::process() {
    bool previewActive = preview_ && preview_->isRunning();
    UBaseType_t queueDepthBeforeDrain = 0;

    BLEDataPacket pkt;
    uint32_t latestPktTs = 0;
    const uint32_t activeSessionGeneration = sessionGeneration_.load(std::memory_order_acquire);
    const bool sessionOpen = acceptNotifications_.load(std::memory_order_acquire);
    queueDepthBeforeDrain = queueHandle_ ? uxQueueMessagesWaiting(queueHandle_) : 0;

    // A completed long packet may have been held while an older partial B2CE
    // stream was parsed. Once that stream is empty, publish the assembled ESP
    // packet before consuming later notifications.
    if (longRxComplete() && rxReadPos_ >= rxBuffer_.size()) {
        clearRxState();
        const uint32_t assembledTimestamp = longRx_.latestTimestampMs;
        if (flushCompleteLongRx()) {
            latestPktTs = assembledTimestamp;
        }
    }

    // Peek before staging so an accepted short notification remains owned by
    // the queue until all of its bytes fit. Long chunks transfer ownership to
    // the fixed indexed assembly; a completed assembly is retained across
    // process() calls when an older partial B2CE stream must finish first.
    while (queueHandle_ && xQueuePeek(queueHandle_, &pkt, 0) == pdTRUE) {
        if (!sessionOpen || pkt.sessionGeneration != activeSessionGeneration) {
            (void)xQueueReceive(queueHandle_, &pkt, 0);
            continue;
        }

        if (pkt.charUUID == LONG_NOTIFY_CHARACTERISTIC) {
            // Do not overwrite a complete candidate with a later B4E0 packet.
            // Parse the older short stream first; the next process() call will
            // publish this candidate and then resume queue draining.
            if (longRxComplete()) {
                break;
            }
            bool beginsAfterDiscontinuity = ingressWouldBeDiscontinuous(pkt.ingressSequence) ||
                                            pendingStreamDiscontinuity_;
            acceptIngressSequence(pkt.ingressSequence);
            const bool acceptedChunk = appendLongRxChunk(pkt, beginsAfterDiscontinuity);
            pendingStreamDiscontinuity_ = !acceptedChunk;
            (void)xQueueReceive(queueHandle_, &pkt, 0);
            latestPktTs = pkt.tsMs;

            if (acceptedChunk && longRxComplete() && rxReadPos_ >= rxBuffer_.size()) {
                clearRxState();
                const uint32_t assembledTimestamp = longRx_.latestTimestampMs;
                if (flushCompleteLongRx()) {
                    latestPktTs = assembledTimestamp;
                }
            }
            continue;
        }

        const bool beginsAfterDiscontinuity = ingressWouldBeDiscontinuous(pkt.ingressSequence) ||
                                              pendingStreamDiscontinuity_;
        if (!appendRxPacket(pkt, beginsAfterDiscontinuity)) {
            break;
        }
        acceptIngressSequence(pkt.ingressSequence);
        pendingStreamDiscontinuity_ = false;
        (void)xQueueReceive(queueHandle_, &pkt, 0);
        latestPktTs = pkt.tsMs;
    }

    if (ble_) {
        ble_->processProxyQueue();
    }

    if (latestPktTs != 0) {
        lastRxMillis_ = latestPktTs;
        lastNotifyTsMs_ = latestPktTs;
    }
    const uint32_t parseTimestampMs = lastNotifyTsMs_;

    if (rxReadPos_ >= rxBuffer_.size()) {
        clearRxState();
        applyPendingDiscontinuityIfStreamDrained();
        refreshBackpressureState();
        return;
    }

    size_t availableBytes = rxBuffer_.size() - rxReadPos_;
    if (availableBytes == 0) {
        refreshBackpressureState();
        return;
    }

    const size_t MIN_HEADER_SIZE = 6;
    const size_t MAX_PACKET_SIZE = 512;

    // Adaptive drain budget: keep a low-latency baseline but accelerate when
    // queue/backlog indicates BLE ingest is falling behind.
    static constexpr size_t BASE_PACKETS_PER_CYCLE = 8;
    static constexpr size_t MID_PACKETS_PER_CYCLE = 16;
    static constexpr size_t HIGH_PACKETS_PER_CYCLE = 24;
    static constexpr size_t MAX_PACKETS_PER_CYCLE = 32;
    static constexpr uint32_t BASE_PARSE_BUDGET_US = 2500;
    static constexpr uint32_t BURST_PARSE_BUDGET_US = 7000;
    static constexpr size_t MID_BACKLOG_BYTES = 192;
    static constexpr size_t HIGH_BACKLOG_BYTES = 320;
    static constexpr size_t MAX_BACKLOG_BYTES = 448;

    const size_t queueDepthSnapshot = static_cast<size_t>(queueDepthBeforeDrain);
    const size_t queueHalfThreshold = std::max<size_t>(4, config_.queueDepth / 2);
    const size_t queueHighThreshold = std::max<size_t>(6, (config_.queueDepth * 3) / 4);

    size_t maxPacketsPerCycle = BASE_PACKETS_PER_CYCLE;
    if (availableBytes >= MID_BACKLOG_BYTES || queueDepthSnapshot >= queueHalfThreshold) {
        maxPacketsPerCycle = MID_PACKETS_PER_CYCLE;
    }
    if (availableBytes >= HIGH_BACKLOG_BYTES || queueDepthSnapshot >= queueHighThreshold) {
        maxPacketsPerCycle = HIGH_PACKETS_PER_CYCLE;
    }
    if (availableBytes >= MAX_BACKLOG_BYTES && queueDepthSnapshot >= queueHighThreshold) {
        maxPacketsPerCycle = MAX_PACKETS_PER_CYCLE;
    }
    if (previewActive && maxPacketsPerCycle > MID_PACKETS_PER_CYCLE) {
        maxPacketsPerCycle = MID_PACKETS_PER_CYCLE;
    }

    const uint32_t parseCycleStartUs = micros();
    const uint32_t parseBudgetUs =
        (maxPacketsPerCycle > BASE_PACKETS_PER_CYCLE) ? BURST_PARSE_BUDGET_US : BASE_PARSE_BUDGET_US;
    size_t packetsProcessedThisCycle = 0;
    bool loggedTooLargeWarning = false;
    bool loggedMissingEndWarning = false;

    while (true) {
        if (packetsProcessedThisCycle >= maxPacketsPerCycle) {
            break;
        }
        if (packetsProcessedThisCycle >= BASE_PACKETS_PER_CYCLE &&
            (micros() - parseCycleStartUs) >= parseBudgetUs) {
            break;
        }

        availableBytes = rxBuffer_.size() - rxReadPos_;
        if (availableBytes == 0)
            break;

        const uint8_t* dataBegin = rxBuffer_.data() + rxReadPos_;
        const uint8_t* startPtr =
            (rxBuffer_[rxReadPos_] == ESP_PACKET_START)
                ? dataBegin
                : static_cast<const uint8_t*>(memchr(dataBegin, ESP_PACKET_START, availableBytes));
        if (startPtr == nullptr) {
            markAlertStreamDiscontinuous();
            clearRxState();
            break;
        }
        if (startPtr != dataBegin) {
            markAlertStreamDiscontinuous();
            rxReadPos_ = static_cast<size_t>(startPtr - rxBuffer_.data());
            continue;
        }
        if (availableBytes < MIN_HEADER_SIZE) {
            break;
        }

        uint8_t lenField = rxBuffer_[rxReadPos_ + 4];
        if (lenField == 0) {
            markAlertStreamDiscontinuous();
            rxReadPos_++;
            continue;
        }

        size_t packetSize = 6 + lenField;
        if (packetSize > MAX_PACKET_SIZE) {
            if (!loggedTooLargeWarning) {
                if (shouldLogBleConnectionEvent(tooLargeWarningLog_, parseTimestampMs, kBleResyncLogMinIntervalMs)) {
                    Serial.printf("[BLE] WARN: BLE packet too large (%u bytes) - resyncing\n", (unsigned)packetSize);
                }
                loggedTooLargeWarning = true;
            }
            markAlertStreamDiscontinuous();
            rxReadPos_++;
            continue;
        }
        if (availableBytes < packetSize) {
            break;
        }
        if (rxBuffer_[rxReadPos_ + packetSize - 1] != ESP_PACKET_END) {
            if (!loggedMissingEndWarning) {
                if (shouldLogBleConnectionEvent(missingEndWarningLog_, parseTimestampMs, kBleResyncLogMinIntervalMs)) {
                    Serial.println("[BLE] WARN: Packet missing end marker - resyncing");
                }
                loggedMissingEndWarning = true;
            }
            markAlertStreamDiscontinuous();
            rxReadPos_++;
            continue;
        }

        const uint8_t* packetPtr = rxBuffer_.data() + rxReadPos_;
        const uint8_t packetId = packetPtr[3];
        const uint32_t packetIngressSequence = rxIngressSequences_[rxReadPos_];
        const auto discontinuityBegin = rxDiscontinuities_.begin() + rxReadPos_;
        const auto discontinuityEnd = discontinuityBegin + packetSize;
        if (std::find(discontinuityBegin, discontinuityEnd, uint8_t{1}) != discontinuityEnd) {
            markAlertStreamDiscontinuous();
        }

        // RESP_USER_BYTES has six settings bytes. Checksum-originator EAh uses
        // PL=7; no-checksum E9h uses PL=6. Origin-qualified width validation
        // prevents a truncated EAh frame's checksum from becoming byte six.
        if (packetPtr[3] == PACKET_ID_RESP_USER_BYTES && profiles_ &&
            V1PacketFraming::hasCanonicalResponseEvidenceForDestination(packetPtr, packetSize, 6, 0xD6)) {
            uint8_t userBytes[6];
            memcpy(userBytes, &packetPtr[5], 6);
            ble_->onUserBytesReceived(userBytes, packetIngressSequence);
            profiles_->setCurrentSettings(userBytes);
            rxReadPos_ += packetSize;
            packetsProcessedThisCycle++;
            continue;
        }

        const bool sweepResponse = packetId == PACKET_ID_RESP_SWEEP_SECTIONS ||
                                   packetId == PACKET_ID_RESP_MAX_SWEEP_INDEX ||
                                   packetId == PACKET_ID_RESP_SWEEP_DEFINITION;
        const bool eligibleSweepResponse = sweepResponse && ble_ &&
            ble_->sessionSweepResponseEligible(packetId, packetIngressSequence);
        if (eligibleSweepResponse && ble_->consumeSessionSweepParserReset(packetId)) {
            if (packetId == PACKET_ID_RESP_SWEEP_SECTIONS) parser_->resetSweepSectionsObservation();
            else if (packetId == PACKET_ID_RESP_MAX_SWEEP_INDEX) parser_->resetSweepMaxObservation();
            else {
                parser_->resetSweepDefinitionsObservation();
                // Definitions are allowed to arrive before the response to
                // the already-sent max request. Do not let a prior session's
                // max collector constrain those fresh entries; the fresh max
                // response will later validate the exact present set.
                if (!ble_->hasSessionSweepMaxCapture()) parser_->resetSweepMaxObservation();
            }
        }
        bool parseOk = parser_->parse(packetPtr, packetSize, parseTimestampMs, packetIngressSequence);
        if (!parseOk && packetId == PACKET_ID_ALERT_DATA) {
            markAlertStreamDiscontinuous();
        }

        if (parseOk && packetId == PACKET_ID_DISPLAY_DATA && ble_) {
            ble_->onV1DisplayFlowControl(parser_->getDisplayState().timeSliceHoldoff);
        }

        if (parseOk && packetId == PACKET_ID_RESP_REQUEST_NOT_PROCESSED && ble_) {
            ble_->onV1RequestNotProcessed(packetPtr[5]);
        }
        if (parseOk && packetId == PACKET_ID_INF_V1_BUSY && ble_) {
            const size_t checksumBytes = packetPtr[2] == 0xEA ? 1u : 0u;
            if (static_cast<size_t>(packetPtr[4]) >= checksumBytes) {
                const size_t busyCount = static_cast<size_t>(packetPtr[4]) - checksumBytes;
                ble_->onV1Busy(&packetPtr[5], busyCount);
            }
        }

        if (parseOk && packetId == PACKET_ID_RESP_VERSION && ble_) {
            const DisplayState& state = parser_->getDisplayState();
            if (state.hasV1Version) {
                ble_->onV1FirmwareVersionReceived(state.v1FirmwareVersion);
            }
        }
        if (parseOk && packetId == PACKET_ID_RESP_ALL_VOLUME && ble_ && parser_->getDisplayState().hasSavedVolume) {
            ble_->onAllVolumeReceived(packetIngressSequence);
        }
        if (eligibleSweepResponse) {
            if (packetId == PACKET_ID_RESP_SWEEP_SECTIONS) {
                const auto& sections = parser_->sweepSectionsObservation();
                ble_->onSweepSectionsReceived(parseOk && !sections.poisoned && sections.complete);
            } else if (packetId == PACKET_ID_RESP_MAX_SWEEP_INDEX) {
                const auto& maximum = parser_->sweepMaxObservation();
                const bool captured = parseOk && maximum.available && !maximum.poisoned;
                ble_->onSweepMaxReceived(captured);
                if (captured) {
                    const uint8_t maxIndex = maximum.maxIndex;
                    const uint64_t required = maxIndex == 63 ? UINT64_MAX : ((uint64_t{1} << (maxIndex + 1u)) - 1u);
                    const auto& definitions = parser_->sweepDefinitionsObservation();
                    ble_->onSweepDefinitionsReceived(!definitions.poisoned && definitions.presentMask == required);
                } else {
                    ble_->onSweepDefinitionsReceived(false);
                }
            } else if (packetId == PACKET_ID_RESP_SWEEP_DEFINITION &&
                       parser_->sweepMaxObservation().available) {
                const uint8_t maxIndex = parser_->sweepMaxObservation().maxIndex;
                const uint64_t required = maxIndex == 63 ? UINT64_MAX : ((uint64_t{1} << (maxIndex + 1u)) - 1u);
                const auto& definitions = parser_->sweepDefinitionsObservation();
                ble_->onSweepDefinitionsReceived(parseOk && !definitions.poisoned &&
                                                 definitions.presentMask == required);
            } else if (packetId == PACKET_ID_RESP_SWEEP_DEFINITION) {
                ble_->onSweepDefinitionsReceived(false);
            }
        }

        rxReadPos_ += packetSize;
        packetsProcessedThisCycle++;

        if (parseOk) {
            if (power_) {
                power_->onV1DataReceived();
            }
            // Only cancel preview when V1 has an actual alert (not on every packet)
            // This allows color preview to run while V1 is connected but resting
            if (previewActive && preview_ && parser_->hasAlerts()) {
                preview_->cancel();
                previewActive = false;
            }
            // Set flag for main loop to drive display pipeline
            // This decouples BLE processing from slow display updates
            hadSuccessfulParse_ = true;
        }
    }

    if (rxReadPos_ >= rxBuffer_.size()) {
        clearRxState();
        applyPendingDiscontinuityIfStreamDrained();
    } else if (rxReadPos_ >= RX_COMPACT_THRESHOLD) {
        compactRxState();
    }

    refreshBackpressureState();
}
