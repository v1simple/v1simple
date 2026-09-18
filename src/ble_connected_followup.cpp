#include "ble_client.h"

#include "config.h"
#include "ble_internals.h"
#include "v1_firmware_compat.h"

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
    const auto consumeBoundedRetry = [this](uint8_t packetId) {
        if (!v1RequestFlowControl_.consumeReleasedRetry(packetId)) return false;
        if (connectedFollowupRetryUsed_.test(packetId)) return false;
        connectedFollowupRetryUsed_.set(packetId);
        return true;
    };
    switch (connectedFollowupStep_) {
    case ConnectedFollowupStep::NONE:
        return;
    case ConnectedFollowupStep::REQUEST_ALERT_DATA: {
        const uint32_t nowMs = static_cast<uint32_t>(millis());
        if (connectedFollowupNextAttemptMs_ != 0 &&
            static_cast<int32_t>(nowMs - connectedFollowupNextAttemptMs_) < 0) {
            return;
        }
        const bool ok = requestAlertData();
        if (!ok) {
            const bool retryTimedOut = static_cast<int32_t>(nowMs - connectedFollowupSendDeadlineMs_) >= 0;
            if (!retryTimedOut) {
                connectedFollowupNextAttemptMs_ = nowMs + CONNECTED_FOLLOWUP_RETRY_MS;
                return;
            }
            logNonCriticalFollowupFailure(followupRequestAlertFailLog_,
                                          "[BLE] Alert-data start deferred to stream recovery");
        }
        connectedFollowupNextAttemptMs_ = 0;
        connectedFollowupSendDeadlineMs_ = 0;
        connectBurstStableLoopCount_ = 0;
        connectedFollowupStep_ = ConnectedFollowupStep::WAIT_CONNECT_BURST_SETTLE;
        return;
    }
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
            connectedFollowupSendDeadlineMs_ = nowMs + CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
            settingsCaptureTimedOut_ = true;
            connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_USER_BYTES;
            return;
        }
        versionRequestStartedMs_ = nowMs;
        connectedFollowupNextAttemptMs_ = 0;
        connectedFollowupSendDeadlineMs_ = 0;
        connectedFollowupStep_ = ConnectedFollowupStep::WAIT_VERSION;
        return;
    }
    case ConnectedFollowupStep::WAIT_VERSION: {
        const uint32_t nowMs = static_cast<uint32_t>(millis());
        if (consumeBoundedRetry(PACKET_ID_VERSION)) {
            connectedFollowupNextAttemptMs_ = 0;
            connectedFollowupSendDeadlineMs_ = nowMs + CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
            connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_VERSION;
            return;
        }
        if (!hasV1FirmwareVersion()) {
            const bool timedOut = static_cast<int32_t>(
                                      nowMs - (versionRequestStartedMs_ + VERSION_RESPONSE_TIMEOUT_MS)) >= 0;
            if (!timedOut) return;
            settingsCaptureTimedOut_ = true;
            logNonCriticalFollowupFailure(followupRequestVersionFailLog_,
                                          "[BLE] Version response timed out (snapshot partial)");
            connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_USER_BYTES;
        } else if (V1FirmwareCompat::capabilities(v1FirmwareVersion()).allVolume) {
            expectsSessionAllVolume_ = true;
            connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_ALL_VOLUME;
        } else {
            connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_USER_BYTES;
        }
        connectedFollowupNextAttemptMs_ = 0;
        connectedFollowupSendDeadlineMs_ = nowMs + CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
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
            settingsCaptureTimedOut_ = true;
            expectsSessionAllVolume_ = false;
            connectedFollowupNextAttemptMs_ = 0;
            connectedFollowupSendDeadlineMs_ = nowMs + CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
            connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_USER_BYTES;
            return;
        }
        beginSessionAllVolumeCapture(latestV1NotificationIngressSequence());
        connectedFollowupNextAttemptMs_ = 0;
        connectedFollowupSendDeadlineMs_ = nowMs + CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
        connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_USER_BYTES;
        return;
    }
    case ConnectedFollowupStep::REQUEST_USER_BYTES: {
        const uint32_t nowMs = static_cast<uint32_t>(millis());
        if (connectedFollowupNextAttemptMs_ != 0 && static_cast<int32_t>(nowMs - connectedFollowupNextAttemptMs_) < 0) {
            return;
        }
        const SendResult result = sendEmptyPayloadFollowupRequest(*this, PACKET_ID_REQ_USER_BYTES);
        if (result != SendResult::SENT) {
            const bool retryTimedOut = static_cast<int32_t>(nowMs - connectedFollowupSendDeadlineMs_) >= 0;
            if (result == SendResult::NOT_YET && !retryTimedOut) {
                connectedFollowupNextAttemptMs_ = nowMs + CONNECTED_FOLLOWUP_RETRY_MS;
                return;
            }
            logNonCriticalFollowupFailure(followupRequestUserBytesFailLog_,
                                          result == SendResult::FAILED
                                              ? "[BLE] Failed to request pre-apply user bytes (snapshot partial)"
                                              : "[BLE] User-byte request retry timed out (snapshot partial)");
            settingsCaptureTimedOut_ = true;
            connectedFollowupNextAttemptMs_ = 0;
            connectedFollowupSendDeadlineMs_ = 0;
            connectedFollowupStep_ = ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK;
            return;
        }
        beginSessionUserBytesCapture(latestV1NotificationIngressSequence());
        settingsCaptureRequestStartedMs_ = nowMs;
        connectedFollowupNextAttemptMs_ = 0;
        connectedFollowupSendDeadlineMs_ = 0;
        connectedFollowupStep_ = ConnectedFollowupStep::WAIT_SETTINGS_SNAPSHOT;
        return;
    }
    case ConnectedFollowupStep::WAIT_SETTINGS_SNAPSHOT: {
        const uint32_t nowMs = static_cast<uint32_t>(millis());
        if (consumeBoundedRetry(PACKET_ID_REQ_ALL_VOLUME)) {
            connectedFollowupNextAttemptMs_ = 0;
            connectedFollowupSendDeadlineMs_ = nowMs + CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
            connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_ALL_VOLUME;
            return;
        }
        if (consumeBoundedRetry(PACKET_ID_REQ_USER_BYTES)) {
            connectedFollowupNextAttemptMs_ = 0;
            connectedFollowupSendDeadlineMs_ = nowMs + CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
            connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_USER_BYTES;
            return;
        }
        const bool timedOut = static_cast<int32_t>(
                                  nowMs - (settingsCaptureRequestStartedMs_ + SETTINGS_SNAPSHOT_RESPONSE_TIMEOUT_MS)) >=
                              0;
        const bool missingRequiredResponse = !hasSessionUserBytes_ ||
                                             (expectsSessionAllVolume_ && !hasSessionAllVolume_);
        if (missingRequiredResponse && !timedOut) return;
        if (timedOut && missingRequiredResponse) {
            settingsCaptureTimedOut_ = true;
            logNonCriticalFollowupFailure(followupRequestUserBytesFailLog_,
                                          "[BLE] Pre-apply snapshot response timed out (snapshot partial)");
        }
        if (V1FirmwareCompat::capabilities(v1FirmwareVersion()).customSweeps) {
            connectedFollowupNextAttemptMs_ = 0;
            connectedFollowupSendDeadlineMs_ = nowMs + CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
            connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_SWEEP_SECTIONS;
        } else {
            connectedFollowupStep_ = ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK;
        }
        return;
    }
    case ConnectedFollowupStep::REQUEST_SWEEP_SECTIONS: {
        const uint32_t nowMs = static_cast<uint32_t>(millis());
        if (connectedFollowupNextAttemptMs_ != 0 && static_cast<int32_t>(nowMs - connectedFollowupNextAttemptMs_) < 0) {
            return;
        }
        const SendResult result = sendEmptyPayloadFollowupRequest(*this, PACKET_ID_REQ_SWEEP_SECTIONS);
        if (result != SendResult::SENT) {
            if (result == SendResult::NOT_YET && static_cast<int32_t>(nowMs - connectedFollowupSendDeadlineMs_) < 0) {
                connectedFollowupNextAttemptMs_ = nowMs + CONNECTED_FOLLOWUP_RETRY_MS;
                return;
            }
            settingsCaptureTimedOut_ = true;
            connectedFollowupStep_ = ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK;
            return;
        }
        beginSessionSweepSectionsCapture(latestV1NotificationIngressSequence());
        connectedFollowupNextAttemptMs_ = 0;
        connectedFollowupSendDeadlineMs_ = nowMs + CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
        connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_MAX_SWEEP_INDEX;
        return;
    }
    case ConnectedFollowupStep::REQUEST_MAX_SWEEP_INDEX: {
        const uint32_t nowMs = static_cast<uint32_t>(millis());
        if (connectedFollowupNextAttemptMs_ != 0 && static_cast<int32_t>(nowMs - connectedFollowupNextAttemptMs_) < 0) return;
        const SendResult result = sendEmptyPayloadFollowupRequest(*this, PACKET_ID_REQ_MAX_SWEEP_INDEX);
        if (result != SendResult::SENT) {
            if (result == SendResult::NOT_YET && static_cast<int32_t>(nowMs - connectedFollowupSendDeadlineMs_) < 0) {
                connectedFollowupNextAttemptMs_ = nowMs + CONNECTED_FOLLOWUP_RETRY_MS;
                return;
            }
            settingsCaptureTimedOut_ = true;
            connectedFollowupStep_ = ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK;
            return;
        }
        beginSessionSweepMaxCapture(latestV1NotificationIngressSequence());
        connectedFollowupNextAttemptMs_ = 0;
        connectedFollowupSendDeadlineMs_ = nowMs + CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
        connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_ALL_SWEEP_DEFINITIONS;
        return;
    }
    case ConnectedFollowupStep::REQUEST_ALL_SWEEP_DEFINITIONS: {
        const uint32_t nowMs = static_cast<uint32_t>(millis());
        if (connectedFollowupNextAttemptMs_ != 0 && static_cast<int32_t>(nowMs - connectedFollowupNextAttemptMs_) < 0) return;
        const SendResult result = sendEmptyPayloadFollowupRequest(*this, PACKET_ID_REQ_ALL_SWEEP_DEFINITIONS);
        if (result != SendResult::SENT) {
            if (result == SendResult::NOT_YET && static_cast<int32_t>(nowMs - connectedFollowupSendDeadlineMs_) < 0) {
                connectedFollowupNextAttemptMs_ = nowMs + CONNECTED_FOLLOWUP_RETRY_MS;
                return;
            }
            settingsCaptureTimedOut_ = true;
            connectedFollowupStep_ = ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK;
            return;
        }
        beginSessionSweepDefinitionsCapture(latestV1NotificationIngressSequence());
        settingsCaptureRequestStartedMs_ = nowMs;
        connectedFollowupNextAttemptMs_ = 0;
        connectedFollowupSendDeadlineMs_ = 0;
        connectedFollowupStep_ = ConnectedFollowupStep::WAIT_SWEEP_SNAPSHOT;
        return;
    }
    case ConnectedFollowupStep::WAIT_SWEEP_SNAPSHOT: {
        const uint32_t nowMs = static_cast<uint32_t>(millis());
        if (consumeBoundedRetry(PACKET_ID_REQ_SWEEP_SECTIONS)) {
            connectedFollowupNextAttemptMs_ = 0;
            connectedFollowupSendDeadlineMs_ = nowMs + CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
            connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_SWEEP_SECTIONS;
            return;
        }
        if (consumeBoundedRetry(PACKET_ID_REQ_MAX_SWEEP_INDEX)) {
            connectedFollowupNextAttemptMs_ = 0;
            connectedFollowupSendDeadlineMs_ = nowMs + CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
            connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_MAX_SWEEP_INDEX;
            return;
        }
        if (consumeBoundedRetry(PACKET_ID_REQ_ALL_SWEEP_DEFINITIONS)) {
            connectedFollowupNextAttemptMs_ = 0;
            connectedFollowupSendDeadlineMs_ = nowMs + CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
            connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_ALL_SWEEP_DEFINITIONS;
            return;
        }
        const bool complete = hasSessionSweepSections_ && hasSessionSweepMax_ && hasSessionSweepDefinitions_;
        const bool timedOut = static_cast<int32_t>(nowMs -
            (settingsCaptureRequestStartedMs_ + SETTINGS_SNAPSHOT_RESPONSE_TIMEOUT_MS)) >= 0;
        if (!complete && !timedOut) return;
        if (!complete) settingsCaptureTimedOut_ = true;
        connectedFollowupStep_ = ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK;
        return;
    }
    case ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK:
        if (connectStableCallback_) {
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

bool V1BLEClient::beginSettingsRecapture() {
    if (!isConnected() || connectedFollowupStep_ != ConnectedFollowupStep::NONE ||
        !hasV1FirmwareVersion()) {
        return false;
    }
    resetSessionSettingsCapture();
    v1RequestFlowControl_.clearReleasedRetries();
    connectedFollowupRetryUsed_.reset();
    const uint32_t nowMs = static_cast<uint32_t>(millis());
    connectedFollowupNextAttemptMs_ = 0;
    connectedFollowupSendDeadlineMs_ = nowMs + CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
    if (V1FirmwareCompat::capabilities(v1FirmwareVersion()).allVolume) {
        expectsSessionAllVolume_ = true;
        connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_ALL_VOLUME;
    } else {
        connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_USER_BYTES;
    }
    return true;
}

void V1BLEClient::schedulePostDeleteBondBackup(uint32_t nowMs) {
    lastBondBackupCount_ = 0xFF;
    pendingBondBackup_ = true;
    pendingBondBackupCount_ = static_cast<uint8_t>(NimBLEDevice::getNumBonds());
    pendingBondBackupRetryAtMs_ = 0;
    serviceDeferredBondBackup(nowMs);
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
