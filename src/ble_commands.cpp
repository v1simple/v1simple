
#include "ble_client.h"
#include "ble_internals.h"
#include "config.h"
#include "v1_firmware_compat.h"
#include <atomic>
#include <cstring>


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

SendResult V1BLEClient::sendCommandWithResult(const uint8_t* data, size_t length) {
    // Remote handles are main-loop-owned and remain stable for the connected
    // session. Keep one local snapshot so every capability check and write is
    // performed on the same object.
    NimBLERemoteCharacteristic* const commandChar = pCommandChar_;

    if (!isConnected() || !commandChar) {
        return SendResult::FAILED;
    }
    if (!data || length == 0 || length > 64) {
        return SendResult::FAILED;
    }
    if (isV1WriteHeldOff(data, length)) {
        return SendResult::NOT_YET;
    }

    // Five-millisecond non-blocking gate; the caller retains NOT_YET packets.
    static std::atomic<uint32_t> lastCommandMs{0};
    uint32_t nowMs = millis();
    uint32_t last = lastCommandMs.load(std::memory_order_relaxed);
    if (last != 0 && nowMs - last < 5) {
        return SendResult::NOT_YET;
    }
    lastCommandMs.store(nowMs, std::memory_order_relaxed);

    bool ok = false;
    if (commandChar->canWrite()) {
        ok = commandChar->writeValue(data, length, true);
    } else if (commandChar->canWriteNoResponse()) {
        ok = commandChar->writeValue(data, length, false);
    } else {
        return SendResult::FAILED; // Characteristic doesn't support write
    }

    if (!ok) {
        // A post-check write failure may be transient; the next retry rechecks connection state.
        return SendResult::NOT_YET;
    }
    return SendResult::SENT;
}


bool V1BLEClient::requestAlertData() {
    if (!isConnected()) {
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

    const bool sent = sendCommand(packet, sizeof(packet));
    if (sent) {
        // Measure the retry interval from a successful transmission.
        alertDataRequestGate_.recordSuccessfulSend(requestGeneration, static_cast<uint32_t>(millis()));
    }
    return sent;
}

bool V1BLEClient::requestVersion() {
    uint8_t packet[] = {ESP_PACKET_START,
                        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
                        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
                        PACKET_ID_VERSION,
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
    // The empty request returns [main, muted, savedMain, savedMuted].
    uint8_t packet[] = {ESP_PACKET_START,
                        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
                        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
                        PACKET_ID_REQ_ALL_VOLUME,
                        0x01, // payload length byte (1 = no payload, only checksum follows)
                        0x00, // checksum placeholder
                        ESP_PACKET_END};

    packet[5] = calcV1Checksum(packet, 5);

    return sendCommand(packet, sizeof(packet));
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
    if (localV1WriteSuppressedByProxy("display")) {
        return false;
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

        return sendCommand(packet, sizeof(packet));
    } else if (keepBluetoothIndicatorOn) {
        if (v1FirmwareVersion() < V1FirmwareCompat::kKeepBluetoothLedOnVersion) return false;
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
        return sendCommand(packet, sizeof(packet));
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

        return sendCommand(packet, sizeof(packet));
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
    if (localV1WriteSuppressedByProxy("mode")) {
        return false;
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

    return sendCommand(packet, sizeof(packet));
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
    uint8_t packet[] = {ESP_PACKET_START,
                        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
                        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
                        PACKET_ID_REQ_USER_BYTES,
                        0x01, // length
                        0x00, // checksum placeholder
                        ESP_PACKET_END};

    packet[5] = calcV1Checksum(packet, 5);

    return sendCommand(packet, sizeof(packet));
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
    if (localV1WriteSuppressedByProxy("user-bytes") || !bytes) return false;

    uint8_t packet[13];
    packet[0] = ESP_PACKET_START;
    packet[1] = static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1);
    packet[2] = static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE);
    packet[3] = PACKET_ID_WRITE_USER_BYTES;
    packet[4] = 0x07;
    memcpy(&packet[5], bytes, 6);
    packet[11] = calcV1Checksum(packet, 11);
    packet[12] = ESP_PACKET_END;
    return sendCommand(packet, sizeof(packet));
}

V1BLEClient::WriteVerifyResult V1BLEClient::writeUserBytesVerified(const uint8_t* bytes, int maxRetries) {
    if (!bytes || !isConnected()) {
        return VERIFY_WRITE_FAILED;
    }

    // Read-back responses arrive asynchronously through the main-loop queue,
    // so this blocking helper retries transmission but cannot verify completion.

    Serial.println("[VerifyPush] Writing to V1 (async verification not possible in blocking context)");

    for (int attempt = 1; attempt <= maxRetries; attempt++) {
        if (writeUserBytes(bytes)) {
            Serial.printf("[VerifyPush] Write command sent successfully (attempt %d/%d)\n", attempt, maxRetries);
            requestUserBytes();
            return VERIFY_OK;
        }
        Serial.printf("[VerifyPush] Write attempt %d/%d failed, retrying...\n", attempt, maxRetries);
        // BLE write failures are immediate, so retries need no delay.
    }

    Serial.println("[VerifyPush] All write attempts failed");
    return VERIFY_WRITE_FAILED;
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
                                 static_cast<int32_t>(ingressSequence - sessionUserBytesIngressBoundary_) > 0;
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
