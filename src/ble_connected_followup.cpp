#include "ble_client.h"

#include "config.h"
#include "ble_internals.h"
#include "perf_metrics.h"

namespace {

void logNonCriticalFollowupFailure(BleLogRateLimitState& state, const char* message) {
    const uint32_t nowMs = static_cast<uint32_t>(millis());
    if (shouldLogBleConnectionEvent(state, nowMs)) {
        Serial.println(message);
    }
}

}  // namespace

void V1BLEClient::processConnectedFollowup() {
    switch (connectedFollowupStep_) {
        case ConnectedFollowupStep::NONE:
            return;
        case ConnectedFollowupStep::REQUEST_ALERT_DATA:
            {
                const uint32_t startUs = micros();
                const bool ok = requestAlertData();
                perfRecordBleFollowupRequestAlertUs(micros() - startUs);
                if (!ok) {
                    logNonCriticalFollowupFailure(
                        followupRequestAlertFailLog_,
                        "[BLE] Failed to request alert data (non-critical)");
                }
            }
            connectBurstStableLoopCount_ = 0;
            connectedFollowupStep_ = ConnectedFollowupStep::WAIT_CONNECT_BURST_SETTLE;
            return;
        case ConnectedFollowupStep::WAIT_CONNECT_BURST_SETTLE: {
            const uint32_t bleProcessUs =
                lastBleProcessDurationUs_.load(std::memory_order_relaxed);
            const uint32_t displayPipeUs =
                lastDisplayPipelineDurationUs_.load(std::memory_order_relaxed);
            const bool bleStable = bleProcessUs <= CONNECT_BURST_STABLE_BLE_MAX_US;
            const bool displayStable =
                (displayPipeUs == 0u || displayPipeUs <= CONNECT_BURST_STABLE_DISP_MAX_US);
            if (bleStable && displayStable) {
                if (connectBurstStableLoopCount_ < 0xFF) {
                    ++connectBurstStableLoopCount_;
                }
            } else {
                connectBurstStableLoopCount_ = 0;
            }

            const uint32_t nowMs = static_cast<uint32_t>(millis());
            const uint32_t connectedAtMs =
                connectCompletedAtMs_.load(std::memory_order_relaxed);
            const uint32_t firstRxMs =
                firstRxAfterConnectMs_.load(std::memory_order_relaxed);
            const bool firstRxSeen =
                firstRxMs != 0u &&
                connectedAtMs != 0u &&
                static_cast<int32_t>(firstRxMs - connectedAtMs) >= 0;
            const uint32_t settleStartMs = firstRxSeen ? firstRxMs : connectedAtMs;
            const uint32_t settleBudgetMs =
                firstRxSeen ? CONNECT_BURST_SETTLE_AFTER_FIRST_RX_MS
                            : CONNECT_BURST_SETTLE_AFTER_CONNECTED_MS;
            const bool timedOut =
                settleStartMs != 0u &&
                static_cast<int32_t>(nowMs - (settleStartMs + settleBudgetMs)) >= 0;

            if (connectBurstStableLoopCount_ >= CONNECT_BURST_STABLE_CONSECUTIVE_LOOPS || timedOut) {
                connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_VERSION;
            }
            return;
        }
        case ConnectedFollowupStep::REQUEST_VERSION:
            {
                const uint32_t startUs = micros();
                const bool ok = requestVersion();
                perfRecordBleFollowupRequestVersionUs(micros() - startUs);
                if (!ok) {
                    logNonCriticalFollowupFailure(
                        followupRequestVersionFailLog_,
                        "[BLE] Failed to request version (non-critical)");
                }
                // Also request authoritative volume
                // settings (RESPALLVOLUME 0x3D).  Best-effort; the existing
                // aux2-nibble inference in display packets remains as fallback.
                if (!requestAllVolume()) {
                    logNonCriticalFollowupFailure(
                        followupRequestAllVolumeFailLog_,
                        "[BLE] Failed to request all-volume (non-critical)");
                }
            }
            connectedFollowupStep_ = ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK;
            return;
        case ConnectedFollowupStep::NOTIFY_STABLE_CALLBACK:
            if (connectStableCallback_) {
                const uint32_t startUs = micros();
                connectStableCallback_();
                perfRecordBleConnectStableCallbackUs(micros() - startUs);
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

    if (pendingBondBackupRetryAtMs_ != 0 &&
        static_cast<int32_t>(nowMs - pendingBondBackupRetryAtMs_) < 0) {
        return;
    }

    const int backed = tryBackupBondsToSD();
    if (backed >= 0) {
        lastBondBackupCount_ = pendingBondBackupCount_;
        pendingBondBackup_ = false;
        pendingBondBackupRetryAtMs_ = 0;
        return;
    }

    pendingBondBackupRetryAtMs_ = nowMs + DEFERRED_BOND_BACKUP_RETRY_MS;
}
