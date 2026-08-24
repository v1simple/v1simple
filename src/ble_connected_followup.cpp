#include "ble_client.h"

#include "config.h"
#include "ble_internals.h"

namespace {

SendResult sendEmptyPayloadFollowupRequest(V1BLEClient& client, uint8_t packetId) {
    uint8_t packet[] = {ESP_PACKET_START,
                        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
                        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
                        packetId,
                        0x01,
                        0x00,
                        ESP_PACKET_END};
    uint8_t checksum = 0;
    for (size_t i = 0; i < 5; ++i) {
        checksum = static_cast<uint8_t>(checksum + packet[i]);
    }
    packet[5] = checksum;
    return client.sendCommandWithResult(packet, sizeof(packet));
}

void logNonCriticalFollowupFailure(BleLogRateLimitState& state, const char* message) {
    const uint32_t nowMs = static_cast<uint32_t>(millis());
    if (shouldLogBleConnectionEvent(state, nowMs)) {
        Serial.println(message);
    }
}

} // namespace

void V1BLEClient::processConnectedFollowup() {
    switch (connectedFollowupStep_) {
    case ConnectedFollowupStep::NONE:
        return;
    case ConnectedFollowupStep::REQUEST_ALERT_DATA: {
        const bool ok = requestAlertData();
        if (!ok) {
            logNonCriticalFollowupFailure(followupRequestAlertFailLog_,
                                          "[BLE] Failed to request alert data (non-critical)");
        }
    }
        connectBurstStableLoopCount_ = 0;
        connectedFollowupStep_ = ConnectedFollowupStep::WAIT_CONNECT_BURST_SETTLE;
        return;
    case ConnectedFollowupStep::WAIT_CONNECT_BURST_SETTLE: {
        if (connectBurstStableLoopCount_ < 0xFF) {
            ++connectBurstStableLoopCount_;
        }

        const uint32_t nowMs = static_cast<uint32_t>(millis());
        const uint32_t connectedAtMs = connectCompletedAtMs_.load(std::memory_order_relaxed);
        const uint32_t firstRxMs = firstRxAfterConnectMs_.load(std::memory_order_relaxed);
        const bool firstRxSeen =
            firstRxMs != 0u && connectedAtMs != 0u && static_cast<int32_t>(firstRxMs - connectedAtMs) >= 0;
        const uint32_t settleStartMs = firstRxSeen ? firstRxMs : connectedAtMs;
        const uint32_t settleBudgetMs =
            firstRxSeen ? CONNECT_BURST_SETTLE_AFTER_FIRST_RX_MS : CONNECT_BURST_SETTLE_AFTER_CONNECTED_MS;
        const bool timedOut =
            settleStartMs != 0u && static_cast<int32_t>(nowMs - (settleStartMs + settleBudgetMs)) >= 0;

        if (connectBurstStableLoopCount_ >= CONNECT_BURST_STABLE_CONSECUTIVE_LOOPS || timedOut) {
            connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_VERSION;
            connectedFollowupNextAttemptMs_ = 0;
            connectedFollowupSendDeadlineMs_ = nowMs + CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
        }
        return;
    }
    case ConnectedFollowupStep::REQUEST_VERSION: {
        const uint32_t nowMs = static_cast<uint32_t>(millis());
        if (connectedFollowupNextAttemptMs_ != 0 && static_cast<int32_t>(nowMs - connectedFollowupNextAttemptMs_) < 0) {
            return;
        }
        const SendResult result = sendEmptyPayloadFollowupRequest(*this, PACKET_ID_VERSION);
        if (result != SendResult::SENT) {
            const bool retryTimedOut = static_cast<int32_t>(nowMs - connectedFollowupSendDeadlineMs_) >= 0;
            if (result == SendResult::NOT_YET && !retryTimedOut) {
                connectedFollowupNextAttemptMs_ = nowMs + CONNECTED_FOLLOWUP_RETRY_MS;
                return;
            }
            if (result == SendResult::FAILED) {
                logNonCriticalFollowupFailure(followupRequestVersionFailLog_,
                                              "[BLE] Failed to request version (non-critical)");
            } else {
                logNonCriticalFollowupFailure(followupRequestVersionFailLog_,
                                              "[BLE] Version request retry timed out (non-critical)");
            }
            connectedFollowupNextAttemptMs_ = 0;
            connectedFollowupSendDeadlineMs_ = 0;
            connectedFollowupStep_ = ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK;
            return;
        }
        versionRequestStartedMs_ = nowMs;
        connectedFollowupNextAttemptMs_ = nowMs + CONNECTED_FOLLOWUP_RETRY_MS;
        connectedFollowupSendDeadlineMs_ = nowMs + CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
        connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_ALL_VOLUME;
        return;
    }
    case ConnectedFollowupStep::REQUEST_ALL_VOLUME: {
        const uint32_t nowMs = static_cast<uint32_t>(millis());
        if (connectedFollowupNextAttemptMs_ != 0 && static_cast<int32_t>(nowMs - connectedFollowupNextAttemptMs_) < 0) {
            return;
        }
        const SendResult result = sendEmptyPayloadFollowupRequest(*this, PACKET_ID_REQ_ALL_VOLUME);
        if (result != SendResult::SENT) {
            const bool retryTimedOut = static_cast<int32_t>(nowMs - connectedFollowupSendDeadlineMs_) >= 0;
            if (result == SendResult::NOT_YET && !retryTimedOut) {
                connectedFollowupNextAttemptMs_ = nowMs + CONNECTED_FOLLOWUP_RETRY_MS;
                return;
            }
            if (result == SendResult::FAILED) {
                logNonCriticalFollowupFailure(followupRequestAllVolumeFailLog_,
                                              "[BLE] Failed to request all-volume (non-critical)");
            } else {
                logNonCriticalFollowupFailure(followupRequestAllVolumeFailLog_,
                                              "[BLE] All-volume request retry timed out (non-critical)");
            }
            connectedFollowupNextAttemptMs_ = 0;
            connectedFollowupSendDeadlineMs_ = 0;
            connectedFollowupStep_ = ConnectedFollowupStep::WAIT_VERSION;
            return;
        }
        connectedFollowupNextAttemptMs_ = 0;
        connectedFollowupSendDeadlineMs_ = 0;
        connectedFollowupStep_ = ConnectedFollowupStep::WAIT_VERSION;
        return;
    }
    case ConnectedFollowupStep::WAIT_VERSION: {
        const uint32_t nowMs = static_cast<uint32_t>(millis());
        const bool timedOut =
            static_cast<int32_t>(nowMs - (versionRequestStartedMs_ + VERSION_RESPONSE_TIMEOUT_MS)) >= 0;
        if (!hasV1FirmwareVersion() && !timedOut) {
            return;
        }
        if (timedOut && !hasV1FirmwareVersion()) {
            logNonCriticalFollowupFailure(followupRequestVersionFailLog_,
                                          "[BLE] V1 version response timed out; using legacy-safe user bytes");
        }
        connectedFollowupStep_ = ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK;
        return;
    }
    case ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK:
        if (connectStableCallback_) {
            const uint32_t startUs = micros();
            connectStableCallback_();
        }
        connectedFollowupStep_ = ConnectedFollowupStep::BACKUP_BONDS;
        return;
    case ConnectedFollowupStep::BACKUP_BONDS: {
        const uint8_t currentBondCount = static_cast<uint8_t>(NimBLEDevice::getNumBonds());
        if (lastBondBackupCount_ != currentBondCount) {
            pendingBondBackup_ = true;
            pendingBondBackupCount_ = currentBondCount;
            pendingBondBackupRetryAtMs_ = 0;
        }
        connectedFollowupStep_ = ConnectedFollowupStep::NONE;
        return;
    }
    }
}

void V1BLEClient::serviceDeferredBondBackup(uint32_t nowMs) {
    if (!pendingBondBackup_) {
        return;
    }

    if (pendingBondBackupCount_ == lastBondBackupCount_) {
        pendingBondBackup_ = false;
        pendingBondBackupRetryAtMs_ = 0;
        return;
    }

    if (pendingBondBackupRetryAtMs_ != 0 && static_cast<int32_t>(nowMs - pendingBondBackupRetryAtMs_) < 0) {
        return;
    }

    const int backed = enqueueCurrentBondBackupSnapshot();
    if (backed >= 0) {
        lastBondBackupCount_ = pendingBondBackupCount_;
        pendingBondBackup_ = false;
        pendingBondBackupRetryAtMs_ = 0;
        return;
    }

    pendingBondBackupRetryAtMs_ = nowMs + DEFERRED_BOND_BACKUP_RETRY_MS;
}
