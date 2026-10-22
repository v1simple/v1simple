
#include "ble_client.h"
#include "ble_internals.h"
#include "config.h"
#include "v1_firmware_compat.h"
#include <atomic>
#include <cstring>
#include <esp_heap_caps.h>


static inline uint8_t calcV1Checksum(const uint8_t* data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return sum;
}


bool V1BLEClient::sendCommand(const uint8_t* data, size_t length) {
    return sendCommandWithResult(data, length) == SendResult::SENT;
}

bool V1BLEClient::localV1WriteSuppressedByProxy(const char* operation) const {
    (void)operation;
    if (!proxyClientConnected_.load(std::memory_order_relaxed)) {
        return false;
    }
    return true;
}

void V1BLEClient::commandTaskFunc(void* param) {
    const CommandTaskContext context = *static_cast<CommandTaskContext*>(param);
    V1BLEClient* self = context.owner;
    const bool accepted = self->acceptClientCallbacks_.load(std::memory_order_acquire) &&
        self->sessionGeneration_.load(std::memory_order_acquire) == context.generation &&
        self->sessionPublicationGate_.accepts(context.generation) &&
        (context.phoneGeneration == 0 || self->acceptsPhoneSession(context.phoneGeneration));
    bool result = false;
    if (accepted && context.characteristic) {
        self->commandWorkerIngress_.store(self->latestV1NotificationIngressSequence(), std::memory_order_relaxed);
        self->commandWorkerStarted_.store(true, std::memory_order_release);
        result = context.characteristic->writeValue(context.data, context.length, context.response);
    }
    self->commandWorkerResult_.store(result, std::memory_order_relaxed);
    self->discoveryTaskRunning_.store(false, std::memory_order_release);
    // The same gate as discovery/subscription pins the client and attributes.
    // No owner/context access after releasing it.
    vTaskDeleteWithCaps(nullptr);
}

void V1BLEClient::cancelCommand(V1CommandOwner owner) {
    if (commandInFlight(owner) && bleState_ != BLEState::QUIESCING) beginClientQuiesce();
}

void V1BLEClient::serviceCommandWrite() {
    if (!commandActive_ || bleState_ == BLEState::QUIESCING) return;
    const auto& context = commandTaskContext_;
    const bool stale = !sessionPublicationGate_.accepts(context.generation) ||
        (context.phoneGeneration != 0 && !acceptsPhoneSession(context.phoneGeneration));
    const bool running = discoveryTaskRunning_.load(std::memory_order_acquire);
    const bool timedOut = static_cast<uint32_t>(millis() - commandStartedMs_) >= COMMAND_WRITE_TIMEOUT_MS;
    if (stale || timedOut || (!running && !commandWorkerResult_.load(std::memory_order_relaxed))) {
        Serial.printf("[BLE] Command %s id=%02X; retiring session\n", timedOut ? "timeout" : "failed",
                      context.length > 3 ? context.data[3] : 0);
        beginClientQuiesce();
    }
}

void V1BLEClient::promoteCommandCaptureBoundary(uint8_t requestId, uint32_t boundary) {
    switch (requestId) {
    case PACKET_ID_REQ_USER_BYTES: sessionUserBytesIngressBoundary_ = boundary; break;
    case PACKET_ID_REQ_ALL_VOLUME: sessionAllVolumeIngressBoundary_ = boundary; break;
    case PACKET_ID_REQ_SWEEP_SECTIONS: sessionSweepSectionsIngressBoundary_ = boundary; break;
    case PACKET_ID_REQ_MAX_SWEEP_INDEX: sessionSweepMaxIngressBoundary_ = boundary; break;
    case PACKET_ID_REQ_ALL_SWEEP_DEFINITIONS: sessionSweepDefinitionsIngressBoundary_ = boundary; break;
    default: break;
    }
}

void V1BLEClient::publishCommandSuccess(const uint8_t* data, size_t length, uint32_t sentSession,
                                       uint32_t ingressBoundary, uint32_t rejectedIngress) {
    const bool canonical = length >= 7 && data[0] == ESP_PACKET_START && data[1] == 0xDA &&
                           data[2] == 0xE6 && data[4] + 6u == length &&
                           data[length - 1] == ESP_PACKET_END &&
                           data[length - 2] == calcV1Checksum(data, length - 2);
    if (canonical && length == 7) {
        if (data[3] == 0x42) alertDataRequestGate_.recordStop(sentSession);
        else if (data[3] == PACKET_ID_REQ_START_ALERT && rejectedIngress == 0)
            alertDataRequestGate_.recordSuccessfulSend(sentSession, static_cast<uint32_t>(millis()));
    }
    if (canonical && length == 10 && data[3] == PACKET_ID_REQ_WRITE_VOLUME) {
        volumeWriteReceipt_.sent(sentSession, ingressBoundary);
        if (rejectedIngress) volumeWriteReceipt_.reject(data[3], sentSession, rejectedIngress);
    }
}

SendResult V1BLEClient::sendCharacteristicCommand(NimBLERemoteCharacteristic* characteristic,
    const uint8_t* data, size_t length, bool response, uint32_t phoneGeneration) {
    if (!data || length == 0 || length > sizeof(commandTaskContext_.data)) return SendResult::FAILED;
    serviceCommandWrite();
    if (!isConnected() || !characteristic || bleState_ == BLEState::QUIESCING) return SendResult::FAILED;
    if (commandActive_) {
        const auto& context = commandTaskContext_;
        if (context.commandOwner != commandOwner_) return SendResult::NOT_YET;
        if (context.characteristic != characteristic || context.length != length || context.response != response ||
            context.phoneGeneration != phoneGeneration || memcmp(context.data, data, length) != 0) {
            // A changed intent cannot leave the previous write queued for later.
            cancelCommand(commandOwner_);
            return SendResult::FAILED;
        }
        if (discoveryTaskRunning_.load(std::memory_order_acquire)) return SendResult::IN_FLIGHT;
        const uint32_t boundary = commandWorkerIngress_.load(std::memory_order_relaxed);
        commandCompletedIngress_[static_cast<size_t>(commandOwner_)] = boundary;
        if (length > 3) promoteCommandCaptureBoundary(data[3], boundary);
        publishCommandSuccess(data, length, context.generation, boundary, commandRejectionIngress_);
        commandActive_ = false;
        return SendResult::SENT;
    }
    if (discoveryTaskRunning_.load(std::memory_order_acquire))
        return SendResult::NOT_YET;
    const uint32_t now = static_cast<uint32_t>(millis());
    // Preserve the long-channel queue's existing contiguous chunk delivery.
    // Only the short command channel has the five-millisecond pacing gate.
    const bool paced = characteristic != pCommandCharLong_;
    if (paced && commandLastSentMs_ != 0 && now - commandLastSentMs_ < 5) return SendResult::NOT_YET;
    if (paced) commandLastSentMs_ = now;
    const uint32_t generation = sessionGeneration();
    // NimBLE promotes oversized no-response writes to acknowledged long writes.
    // Only a single ATT write command is safe on the shared loop.
    const uint16_t mtu = pClient_ ? pClient_->getMTU() : 23;
    if (!response && length <= static_cast<size_t>(mtu >= 3 ? mtu - 3 : 0)) {
        const uint32_t boundary = latestV1NotificationIngressSequence();
        if (!characteristic->writeValue(data, length, false)) return SendResult::NOT_YET;
        if (!sessionPublicationGate_.accepts(generation)) return SendResult::FAILED;
        // A synchronous write already reached this V1 session. The phone queue
        // binds STOP ownership to its captured sender, so a phone replacement
        // cannot inherit it and recovery still observes the delivered STOP.
        commandCompletedIngress_[static_cast<size_t>(commandOwner_)] = boundary;
        publishCommandSuccess(data, length, generation, boundary);
        return SendResult::SENT;
    }
    commandTaskContext_ = {};
    commandTaskContext_.owner = this;
    commandTaskContext_.characteristic = characteristic;
    commandTaskContext_.generation = generation;
    commandTaskContext_.phoneGeneration = phoneGeneration;
    commandTaskContext_.commandOwner = commandOwner_;
    commandTaskContext_.length = length;
    commandTaskContext_.response = response;
    memcpy(commandTaskContext_.data, data, length);
    commandStartedMs_ = now;
    commandRejectionIngress_ = 0;
    commandWorkerStarted_.store(false, std::memory_order_relaxed);
    commandWorkerResult_.store(false, std::memory_order_relaxed);
    commandActive_ = true;
    discoveryTaskRunning_.store(true, std::memory_order_release);
    if (xTaskCreatePinnedToCoreWithCaps(commandTaskFunc, "cmd", 4096, &commandTaskContext_, 1,
                                     nullptr, tskNO_AFFINITY, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL) != pdPASS) {
        discoveryTaskRunning_.store(false, std::memory_order_release);
        commandActive_ = false;
        return SendResult::FAILED;
    }
    return SendResult::IN_FLIGHT;
}

SendResult V1BLEClient::sendCommandWithResult(const uint8_t* data, size_t length) {
    NimBLERemoteCharacteristic* const characteristic = pCommandChar_;
    if (!characteristic || (!characteristic->canWrite() && !characteristic->canWriteNoResponse()))
        return SendResult::FAILED;
    if (!commandInFlight(commandOwner_) && isV1WriteHeldOff(data, length)) return SendResult::NOT_YET;
    return sendCharacteristicCommand(characteristic, data, length, characteristic->canWrite());
}


bool V1BLEClient::requestAlertData() {
    if (!isConnected() || phoneOwnsStoppedAlertStream()) {
        return false;
    }

    const uint32_t requestGeneration = sessionGeneration_.load(std::memory_order_acquire);
    const uint32_t requestNowMs = static_cast<uint32_t>(millis());
    if (!alertDataRequestGate_.permits(requestGeneration, requestNowMs)) {
        // A cross-owner duplicate is satisfied, not a transport failure.
        return true;
    }

    // Keep protocol bytes explicit for direct verification.
    uint8_t packet[] = {ESP_PACKET_START,
                        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
                        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
                        PACKET_ID_REQ_START_ALERT,
                        0x01,
                        0x00,
                        ESP_PACKET_END};

    packet[5] = calcV1Checksum(packet, 5);

    return sendCommand(packet, sizeof(packet));
}

void V1BLEClient::onV1FirmwareVersionReceived(uint32_t version) {
    if (version != 0) {
        v1FirmwareVersion_.store(version, std::memory_order_release);
    }
}

bool V1BLEClient::requestAllVolume() {
    return requestAllVolumeResult() == SendResult::SENT;
}

SendResult V1BLEClient::requestAllVolumeResult() {
    // The empty request returns [main, muted, savedMain, savedMuted].
    uint8_t packet[] = {ESP_PACKET_START,
                        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
                        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
                        PACKET_ID_REQ_ALL_VOLUME,
                        0x01, // payload length byte (1 = no payload, only checksum follows)
                        0x00, // checksum placeholder
                        ESP_PACKET_END};

    packet[5] = calcV1Checksum(packet, 5);

    return sendCommandWithResult(packet, sizeof(packet));
}

bool V1BLEClient::requestCurrentVolume() {
    uint8_t packet[] = {ESP_PACKET_START,
                        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
                        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
                        PACKET_ID_REQ_CURRENT_VOLUME,
                        0x01,
                        0x00,
                        ESP_PACKET_END};
    packet[5] = calcV1Checksum(packet, 5);
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::setDisplayOn(bool on) {
    return setDisplayOn(on, false);
}

bool V1BLEClient::setDisplayOn(bool on, bool keepBluetoothIndicatorOn) {
    return setDisplayOnResult(on, keepBluetoothIndicatorOn) == SendResult::SENT;
}

SendResult V1BLEClient::setDisplayOnResult(bool on, bool keepBluetoothIndicatorOn) {
    if (localV1WriteSuppressedByProxy("display")) {
        return SendResult::FAILED;
    }

    if (on) {
        uint8_t packet[] = {
            ESP_PACKET_START,                                // [0] 0xAA
            static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1), // [1] 0xDA
            static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),  // [2] 0xE6 (0xE0 + ESP_PACKET_REMOTE=0x06)
            PACKET_ID_TURN_ON_DISPLAY,                       // [3] 0x33
            0x01,                                            // [4] payload length
            0x00,                                            // [5] checksum placeholder
            ESP_PACKET_END                                   // [6] 0xAB
        };

        packet[5] = calcV1Checksum(packet, 5);

        return sendCommandWithResult(packet, sizeof(packet));
    } else if (keepBluetoothIndicatorOn) {
        if (v1FirmwareVersion() < V1FirmwareCompat::kKeepBluetoothLedOnVersion) return SendResult::FAILED;
        uint8_t packet[] = {
            ESP_PACKET_START,
            static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
            static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
            PACKET_ID_TURN_OFF_DISPLAY,
            0x02,
            0x01, // Aux0 bit 0: keep Bluetooth indicator on
            0x00,
            ESP_PACKET_END
        };
        packet[6] = calcV1Checksum(packet, 6);
        return sendCommandWithResult(packet, sizeof(packet));
    } else {
        uint8_t packet[] = {
            ESP_PACKET_START,                                // [0] 0xAA
            static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1), // [1] 0xDA
            static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),  // [2] 0xE6 (0xE0 + ESP_PACKET_REMOTE=0x06)
            PACKET_ID_TURN_OFF_DISPLAY,                      // [3] 0x32
            0x01,                                            // [4] no data; checksum only
            0x00,                                            // [5] checksum placeholder
            ESP_PACKET_END                                   // [6] 0xAB
        };

        packet[5] = calcV1Checksum(packet, 5);

        return sendCommandWithResult(packet, sizeof(packet));
    }
}

bool V1BLEClient::setMute(bool muted) {
    return setMuteResult(muted) == SendResult::SENT;
}

SendResult V1BLEClient::setMuteResult(bool muted) {
    if (localV1WriteSuppressedByProxy("mute")) {
        return SendResult::FAILED;
    }

    uint8_t packetId = muted ? PACKET_ID_MUTE_ON : PACKET_ID_MUTE_OFF;
    // Empty-payload commands encode their length as 1.
    uint8_t packet[] = {
        ESP_PACKET_START,                                // [0] 0xAA
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1), // [1] 0xDA
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),  // [2] 0xE6 (0xE0 + ESP_PACKET_REMOTE=0x06)
        packetId,                                        // [3] 0x34 or 0x35
        0x01,                                            // [4] payload length
        0x00,                                            // [5] checksum placeholder
        ESP_PACKET_END                                   // [6] 0xAB
    };

    packet[5] = calcV1Checksum(packet, 5);

    return sendCommandWithResult(packet, sizeof(packet));
}

bool V1BLEClient::setMode(uint8_t mode) {
    return setModeResult(mode) == SendResult::SENT;
}

SendResult V1BLEClient::setModeResult(uint8_t mode) {
    if (localV1WriteSuppressedByProxy("mode")) {
        return SendResult::FAILED;
    }

    // Mode: 0x01 = All Bogeys, 0x02 = Logic, 0x03 = Advanced Logic
    uint8_t packet[] = {
        ESP_PACKET_START,                                // [0] 0xAA
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1), // [1] 0xDA
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),  // [2] 0xE6 (0xE0 + ESP_PACKET_REMOTE=0x06)
        0x36,                                            // [3] REQCHANGEMODE
        0x02,                                            // [4] payload length = 2
        mode,                                            // [5] mode byte
        0x00,                                            // [6] checksum placeholder
        ESP_PACKET_END                                   // [7] 0xAB
    };

    packet[6] = calcV1Checksum(packet, 6);

    return sendCommandWithResult(packet, sizeof(packet));
}

bool V1BLEClient::setVolume(uint8_t mainVolume, uint8_t mutedVolume) {
    return setVolumeResult(mainVolume, mutedVolume) == SendResult::SENT;
}

SendResult V1BLEClient::setVolumeResult(uint8_t mainVolume, uint8_t mutedVolume) {
    return setVolumeResult(mainVolume, mutedVolume, 0);
}

SendResult V1BLEClient::setVolumeResult(uint8_t mainVolume, uint8_t mutedVolume, uint8_t aux0) {
    if (localV1WriteSuppressedByProxy("volume")) {
        return SendResult::FAILED;
    }

    // V1 REQWRITEVOLUME sets BOTH values. Reject a non-pair rather than
    // reporting a skipped command as successful to an owning state machine.
    if (mainVolume == 0xFF || mutedVolume == 0xFF || mainVolume > 9 || mutedVolume > 9 || (aux0 & 0xF0) != 0) {
        Serial.printf("setVolume: rejected incomplete pair - main=%d mute=%d\n", mainVolume, mutedVolume);
        return SendResult::FAILED;
    }

    const uint32_t version = v1FirmwareVersion();
    if ((aux0 & 0x04) != 0 && version < V1FirmwareCompat::kSavedVolumeVersion) return SendResult::FAILED;
    if ((aux0 & 0x08) != 0 && version < V1FirmwareCompat::kKeepCurrentVolumeOnDisconnectVersion) {
        return SendResult::FAILED;
    }
    if ((aux0 & 0x0C) == 0x0C) return SendResult::FAILED;

    // REQWRITEVOLUME payload is [main, muted, aux0].
    uint8_t packet[] = {
        ESP_PACKET_START,                                // [0] 0xAA
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1), // [1] 0xDA
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),  // [2] 0xE6 (0xE0 + ESP_PACKET_REMOTE=0x06)
        PACKET_ID_REQ_WRITE_VOLUME,                      // [3] 0x39
        0x04,                                            // [4] payload length = 4 (3 data + checksum)
        mainVolume,                                      // [5] main volume 0-9
        mutedVolume,                                     // [6] muted volume 0-9
        aux0,                                            // [7] feedback/save/disconnect policy
        0x00,                                            // [8] checksum placeholder
        ESP_PACKET_END                                   // [9] 0xAB
    };

    packet[8] = calcV1Checksum(packet, 8);

    return sendCommandWithResult(packet, sizeof(packet));
}

namespace {
SendResult sendEmptyV1Request(V1BLEClient& client, uint8_t packetId) {
    uint8_t packet[] = {ESP_PACKET_START,
                        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
                        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
                        packetId,
                        0x01,
                        0x00,
                        ESP_PACKET_END};
    packet[5] = calcV1Checksum(packet, 5);
    return client.sendCommandWithResult(packet, sizeof(packet));
}
}

bool V1BLEClient::requestMaxSweepIndex() {
    return requestMaxSweepIndexResult() == SendResult::SENT;
}

bool V1BLEClient::requestSweepSections() {
    return requestSweepSectionsResult() == SendResult::SENT;
}

bool V1BLEClient::requestAllSweepDefinitions() {
    return requestAllSweepDefinitionsResult() == SendResult::SENT;
}

SendResult V1BLEClient::requestMaxSweepIndexResult() {
    return sendEmptyV1Request(*this, PACKET_ID_REQ_MAX_SWEEP_INDEX);
}

SendResult V1BLEClient::requestSweepSectionsResult() {
    return sendEmptyV1Request(*this, PACKET_ID_REQ_SWEEP_SECTIONS);
}

SendResult V1BLEClient::requestAllSweepDefinitionsResult() {
    return sendEmptyV1Request(*this, PACKET_ID_REQ_ALL_SWEEP_DEFINITIONS);
}

SendResult V1BLEClient::writeSweepDefinition(uint8_t index, uint16_t lowerMHz, uint16_t upperMHz, bool commit) {
    if (localV1WriteSuppressedByProxy("custom-frequencies") || index > 0x3F ||
        lowerMHz == 0 || upperMHz == 0 || lowerMHz >= upperMHz ||
        !V1FirmwareCompat::capabilities(v1FirmwareVersion()).customSweeps) return SendResult::FAILED;
    // Both current vendor libraries set bit 7 on writes. Bit 6 is the
    // transaction commit marker and appears only on the final definition.
    uint8_t packet[] = {
        ESP_PACKET_START,
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
        PACKET_ID_WRITE_SWEEP_DEFINITION,
        0x06,
        static_cast<uint8_t>(0x80 | (commit ? 0x40 : 0x00) | index),
        static_cast<uint8_t>((upperMHz >> 8) & 0xFF),
        static_cast<uint8_t>(upperMHz & 0xFF),
        static_cast<uint8_t>((lowerMHz >> 8) & 0xFF),
        static_cast<uint8_t>(lowerMHz & 0xFF),
        0x00,
        ESP_PACKET_END
    };
    packet[10] = calcV1Checksum(packet, 10);
    return sendCommandWithResult(packet, sizeof(packet));
}

SendResult V1BLEClient::factoryResetDetector() {
    if (localV1WriteSuppressedByProxy("factory-default") ||
        !V1FirmwareCompat::capabilities(v1FirmwareVersion()).gen2) return SendResult::FAILED;
    uint8_t packet[] = {ESP_PACKET_START,
                        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
                        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
                        PACKET_ID_FACTORY_DEFAULT,
                        0x01,
                        0x00,
                        ESP_PACKET_END};
    packet[5] = calcV1Checksum(packet, 5);
    return sendCommandWithResult(packet, sizeof(packet));
}


bool V1BLEClient::requestUserBytes() {
    return requestUserBytesResult() == SendResult::SENT;
}

SendResult V1BLEClient::requestUserBytesResult() {
    uint8_t packet[] = {ESP_PACKET_START,
                        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
                        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
                        PACKET_ID_REQ_USER_BYTES,
                        0x01, // length
                        0x00, // checksum placeholder
                        ESP_PACKET_END};

    packet[5] = calcV1Checksum(packet, 5);

    return sendCommandWithResult(packet, sizeof(packet));
}

bool V1BLEClient::writeUserBytes(const uint8_t* bytes) {
    if (localV1WriteSuppressedByProxy("user-bytes")) {
        return false;
    }

    if (!bytes) {
        return false;
    }

    const uint32_t firmwareVersion = v1FirmwareVersion();
    uint8_t preparedBytes[V1FirmwareCompat::kUserByteCount];
    V1FirmwareCompat::prepareUserBytesForWrite(bytes, preparedBytes, firmwareVersion);

    uint8_t packet[13];
    packet[0] = ESP_PACKET_START;
    packet[1] = static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1);
    packet[2] = static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE);
    packet[3] = PACKET_ID_WRITE_USER_BYTES;
    packet[4] = 0x07; // length = 6 bytes + 1
    memcpy(&packet[5], preparedBytes, 6);

    packet[11] = calcV1Checksum(packet, 11);
    packet[12] = ESP_PACKET_END;

    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::writeUserBytesExact(const uint8_t* bytes) {
    return writeUserBytesExactResult(bytes) == SendResult::SENT;
}

SendResult V1BLEClient::writeUserBytesExactResult(const uint8_t* bytes) {
    if (localV1WriteSuppressedByProxy("user-bytes") || !bytes) return SendResult::FAILED;

    uint8_t packet[13];
    packet[0] = ESP_PACKET_START;
    packet[1] = static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1);
    packet[2] = static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE);
    packet[3] = PACKET_ID_WRITE_USER_BYTES;
    packet[4] = 0x07;
    memcpy(&packet[5], bytes, 6);
    packet[11] = calcV1Checksum(packet, 11);
    packet[12] = ESP_PACKET_END;
    return sendCommandWithResult(packet, sizeof(packet));
}

void V1BLEClient::startUserBytesVerification(const uint8_t* expected) {
    if (!expected) {
        return;
    }
    memcpy(verifyExpected_, expected, sizeof(verifyExpected_));
    verifyPending_ = true;
    verifyComplete_ = false;
    verifyMatch_ = false;
    verifyPushMatchEdgePending_.store(false, std::memory_order_relaxed);
}

V1BLEClient::UserBytesVerificationStatus V1BLEClient::userBytesVerificationStatus() const {
    if (verifyPending_) {
        return UserBytesVerificationStatus::PENDING;
    }
    if (!verifyComplete_) {
        return UserBytesVerificationStatus::INACTIVE;
    }
    return verifyMatch_ ? UserBytesVerificationStatus::MATCH : UserBytesVerificationStatus::MISMATCH;
}

void V1BLEClient::cancelUserBytesVerification() {
    verifyPending_ = false;
    verifyComplete_ = false;
    verifyMatch_ = false;
    verifyPushMatchEdgePending_.store(false, std::memory_order_release);
}

void V1BLEClient::publishVerifiedSettingsApplyEdge(uint32_t verifiedSessionGeneration) {
    verifiedSettingsApplyGeneration_.store(verifiedSessionGeneration, std::memory_order_release);
    verifyPushMatchEdgePending_.store(true, std::memory_order_release);
}

void V1BLEClient::onUserBytesReceived(const uint8_t* bytes, uint32_t ingressSequence) {
    const bool sessionEligible = sessionUserBytesCaptureArmed_ && ingressSequence != 0 &&
                                 commandResponseEligible(PACKET_ID_REQ_USER_BYTES, ingressSequence, sessionUserBytesIngressBoundary_);
    if (bytes && sessionEligible) {
        memcpy(sessionUserBytes_, bytes, sizeof(sessionUserBytes_));
        hasSessionUserBytes_ = true;
        ++sessionUserBytesRevision_;
        sessionUserBytesIngressSequence_ = ingressSequence;
    }
    if (verifyPending_ && bytes) {
        memcpy(verifyReceived_, bytes, 6);
        verifyComplete_ = true;
        verifyMatch_ = (memcmp(verifyExpected_, verifyReceived_, 6) == 0);
        verifyPushMatchEdgePending_.store(verifyMatch_, std::memory_order_relaxed);
        Serial.printf("[VerifyPush] Received user bytes: %02X%02X%02X%02X%02X%02X (match=%s)\n", verifyReceived_[0],
                      verifyReceived_[1], verifyReceived_[2], verifyReceived_[3], verifyReceived_[4],
                      verifyReceived_[5], verifyMatch_ ? "YES" : "NO");
        verifyPending_ = false;
    }
}

void V1BLEClient::resetSessionSettingsCapture() {
    hasSessionUserBytes_ = false;
    memset(sessionUserBytes_, 0xFF, sizeof(sessionUserBytes_));
    sessionUserBytesRevision_ = 0;
    sessionUserBytesIngressSequence_ = 0;
    sessionUserBytesIngressBoundary_ = 0;
    sessionUserBytesCaptureArmed_ = false;
    expectsSessionAllVolume_ = false;
    hasSessionAllVolume_ = false;
    sessionAllVolumeIngressSequence_ = 0;
    sessionAllVolumeIngressBoundary_ = 0;
    sessionAllVolumeCaptureArmed_ = false;
    expectsSessionSweeps_ = false;
    hasSessionSweepSections_ = false;
    hasSessionSweepMax_ = false;
    hasSessionSweepDefinitions_ = false;
    sessionSweepSectionsResetPending_ = false;
    sessionSweepMaxResetPending_ = false;
    sessionSweepDefinitionsResetPending_ = false;
    sessionSweepSectionsIngressBoundary_ = 0;
    sessionSweepMaxIngressBoundary_ = 0;
    sessionSweepDefinitionsIngressBoundary_ = 0;
    settingsCaptureTimedOut_ = false;
}

bool V1BLEClient::copySessionUserBytes(uint8_t out[6]) const {
    if (!out || !hasSessionUserBytes_) return false;
    memcpy(out, sessionUserBytes_, sizeof(sessionUserBytes_));
    return true;
}
