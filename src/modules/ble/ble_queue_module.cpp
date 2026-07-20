#include "ble_queue_module.h"

#include <algorithm>
#include <cstring>

#include "config.h"
#include "modules/system/system_event_bus.h"
#include "perf_metrics.h"

#ifndef UNIT_TEST
#include "modules/display/display_preview_module.h"
#include "modules/power/power_module.h"
#include "v1_profiles.h"
#endif

// Maximum bytes to buffer from BLE RX before dropping
static constexpr size_t RX_BUFFER_MAX = 1024;
static constexpr size_t RX_COMPACT_THRESHOLD = RX_BUFFER_MAX / 2;

static void compactRxBuffer(std::vector<uint8_t>& rxBuffer, size_t& readPos) {
    if (readPos == 0) {
        return;
    }
    if (readPos >= rxBuffer.size()) {
        rxBuffer.clear();
        readPos = 0;
        return;
    }
    const size_t unread = rxBuffer.size() - readPos;
    memmove(rxBuffer.data(), rxBuffer.data() + readPos, unread);
    rxBuffer.resize(unread);
    readPos = 0;
}

// RX overflow policy: a chunk is admitted whole or not at all.
//
// This buffer reassembles a *framed* byte stream (ESP_PACKET_START .. length ..
// ESP_PACKET_END) out of BLE notifications: one V1 packet may span several
// notifications, and one notification may carry several packets. Copying only the
// head of a chunk and discarding its tail splices unrelated wire bytes together
// *inside* the buffer. That is worse than losing the chunk: an interior splice can
// forge a frame that still passes the start/length/end-marker check, so the parser
// publishes a well-formed alert row built from two unrelated notifications and has
// no way to detect it. Refusing the chunk outright keeps every buffered byte a
// contiguous run of the wire stream, and costs at most one notification that the
// parser resyncs past by scanning to the next ESP_PACKET_START.
//
// Which end to drop is set by the other overflow point on this same path: when the
// notification queue is full, onNotify()'s xQueueSend() with a 0-tick timeout keeps
// the already-queued head and rejects the incoming packet
// (test_ble_queue_full_drops_incoming_without_evicting_valid_head pins that
// deliberately). Evicting the head there would destroy the front of a packet the RX
// buffer is mid-way through reassembling, so both ends preserve what has already
// been committed and refuse the newest whole unit instead. This function now
// follows the same rule at chunk granularity.
//
// Refusing input cannot wedge the buffer: process() compacts consumed bytes every
// cycle and clears the buffer outright when no start marker is present, so space is
// reclaimed as soon as the parser catches up.
static size_t appendRxClamped(std::vector<uint8_t>& rxBuffer, size_t& readPos, const uint8_t* data, size_t length) {
    if (!data || length == 0) {
        return 0;
    }

    if (readPos > 0) {
        const size_t unread = (readPos < rxBuffer.size()) ? (rxBuffer.size() - readPos) : 0;
        if (rxBuffer.size() >= RX_BUFFER_MAX || (unread + length) > RX_BUFFER_MAX) {
            compactRxBuffer(rxBuffer, readPos);
        }
    }

    const size_t unread = (readPos < rxBuffer.size()) ? (rxBuffer.size() - readPos) : 0;
    if (unread >= RX_BUFFER_MAX || (unread + length) > RX_BUFFER_MAX) {
        // Does not fit whole after compaction - drop the whole chunk.
        return 0;
    }
    const size_t oldSize = rxBuffer.size();
    rxBuffer.resize(oldSize + length);
    memcpy(rxBuffer.data() + oldSize, data, length);
    return length;
}

bool BleQueueModule::begin(V1BLEClient* bleClient, PacketParser* parserPtr, V1ProfileManager* profileMgr,
                           DisplayPreviewModule* previewModule, PowerModule* powerModule, SystemEventBus* eventBus,
                           Config cfg) {
    ble_ = bleClient;
    parser_ = parserPtr;
    profiles_ = profileMgr;
    preview_ = previewModule;
    power_ = powerModule;
    bus_ = eventBus;
    config_ = cfg;

    if (queueHandle_ != nullptr) {
        end();
    }

    rxBuffer_.clear();
    rxReadPos_ = 0;
    lastRxMillis_ = 0;
    lastNotifyTsMs_ = 0;
    lastParsedTsMs_ = 0;
    hadSuccessfulParse_ = false;
    parsedEventSeq_ = 0;
    backpressureActive_ = false;
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
    if (rxBuffer_.capacity() < desiredRxCap) {
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
    }

    rxBuffer_.clear();
    rxReadPos_ = 0;
    lastRxMillis_ = 0;
    lastNotifyTsMs_ = 0;
    lastParsedTsMs_ = 0;
    hadSuccessfulParse_ = false;
    backpressureActive_ = false;
}

void BleQueueModule::onNotify(const uint8_t* data, size_t length, uint16_t charUUID, uint32_t sessionGeneration) {
    if (!queueHandle_ || !acceptNotifications_.load(std::memory_order_acquire) ||
        sessionGeneration != sessionGeneration_.load(std::memory_order_acquire))
        return;

    if (length > 0 && length <= sizeof(BLEDataPacket::data)) {
        PERF_INC(rxPackets);
        PERF_ADD(rxBytes, length);
        BLEDataPacket pkt;
        memcpy(pkt.data, data, length);
        pkt.length = length;
        pkt.charUUID = charUUID;
        pkt.tsMs = millis();
        pkt.sessionGeneration = sessionGeneration;

        // closeSession() can race this callback between its first admission
        // check and packet construction. Recheck before publishing; if it
        // races after this point, process() rejects the stamped generation.
        if (!acceptNotifications_.load(std::memory_order_acquire) ||
            sessionGeneration != sessionGeneration_.load(std::memory_order_acquire)) {
            return;
        }

        BaseType_t result = xQueueSend(queueHandle_, &pkt, 0);
        if (result != pdTRUE) {
            PERF_INC(queueDrops);
        }
        UBaseType_t depth = uxQueueMessagesWaiting(queueHandle_);
        PERF_MAX(queueHighWater, depth);
    } else if (length > sizeof(BLEDataPacket::data)) {
        PERF_INC(oversizeDrops);
    }
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
    return xQueueSend(queueHandle_, &pkt, 0) == pdTRUE;
}
#endif

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
    bool parsedEventPending = false;
    uint16_t parsedEventDetail = 0;

    BLEDataPacket pkt;
    uint32_t latestPktTs = 0;
    const uint32_t activeSessionGeneration = sessionGeneration_.load(std::memory_order_acquire);
    const bool sessionOpen = acceptNotifications_.load(std::memory_order_acquire);
    queueDepthBeforeDrain = queueHandle_ ? uxQueueMessagesWaiting(queueHandle_) : 0;

    while (queueHandle_ && xQueueReceive(queueHandle_, &pkt, 0) == pdTRUE) {
        if (!sessionOpen || pkt.sessionGeneration != activeSessionGeneration) {
            continue;
        }
        appendRxClamped(rxBuffer_, rxReadPos_, pkt.data, pkt.length);
        latestPktTs = pkt.tsMs;
    }

    if (ble_) {
        ble_->processProxyQueue();
    }

    if (latestPktTs != 0) {
        lastRxMillis_ = latestPktTs;
        lastNotifyTsMs_ = latestPktTs;
        perfRecordBleTimelineEvent(PerfBleTimelineEvent::FirstRx, latestPktTs);
    }
    const uint32_t parseTimestampMs = lastNotifyTsMs_;

    if (rxReadPos_ >= rxBuffer_.size()) {
        rxBuffer_.clear();
        rxReadPos_ = 0;
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

    const uint32_t parseCycleStartUs = PERF_TIMESTAMP_US();
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
            (PERF_TIMESTAMP_US() - parseCycleStartUs) >= parseBudgetUs) {
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
            rxBuffer_.clear();
            rxReadPos_ = 0;
            break;
        }
        if (startPtr != dataBegin) {
            rxReadPos_ = static_cast<size_t>(startPtr - rxBuffer_.data());
            continue;
        }
        if (availableBytes < MIN_HEADER_SIZE) {
            break;
        }

        uint8_t lenField = rxBuffer_[rxReadPos_ + 4];
        if (lenField == 0) {
            PERF_INC(parseResyncs);
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
            PERF_INC(parseResyncs);
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
            PERF_INC(parseResyncs);
            rxReadPos_++;
            continue;
        }

        const uint8_t* packetPtr = rxBuffer_.data() + rxReadPos_;

        if (packetSize >= 12 && packetPtr[3] == PACKET_ID_RESP_USER_BYTES && profiles_) {
            uint8_t userBytes[6];
            memcpy(userBytes, &packetPtr[5], 6);
            ble_->onUserBytesReceived(userBytes);
            profiles_->setCurrentSettings(userBytes);
            rxReadPos_ += packetSize;
            packetsProcessedThisCycle++;
            continue;
        }

        uint8_t packetId = packetPtr[3];
        bool parseOk = parser_->parse(packetPtr, packetSize, parseTimestampMs);

        if (packetId == PACKET_ID_DISPLAY_DATA || packetId == PACKET_ID_ALERT_DATA) {
            if (parseOk) {
                PERF_INC(parseSuccesses);
            } else {
                PERF_INC(parseFailures);
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
            if (previewActive && preview_ && parser_->getAlertCount() > 0) {
                preview_->cancel();
                previewActive = false;
            }
            // Set flag and timestamp for main loop to drive display pipeline
            // This decouples BLE processing from slow display updates
            hadSuccessfulParse_ = true;
            lastParsedTsMs_ = lastNotifyTsMs_;
            parsedEventPending = true;
            parsedEventDetail = packetId;
        }
    }

    if (parsedEventPending && bus_) {
        SystemEvent event;
        event.type = SystemEventType::BLE_FRAME_PARSED;
        event.tsMs = lastParsedTsMs_;
        event.seq = ++parsedEventSeq_;
        event.detail = parsedEventDetail;
        bus_->publish(event);
    }

    if (rxReadPos_ >= rxBuffer_.size()) {
        rxBuffer_.clear();
        rxReadPos_ = 0;
    } else if (rxReadPos_ >= RX_COMPACT_THRESHOLD) {
        compactRxBuffer(rxBuffer_, rxReadPos_);
    }

    refreshBackpressureState();
}
