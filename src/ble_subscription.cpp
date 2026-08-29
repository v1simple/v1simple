/**
 * Valentine One Gen2 characteristic discovery and subscription state machine.
 */

#include "ble_client.h"

#include "ble_internals.h"
#include "ble_log_rate_limit.h"
#include "config.h"

// Process SUBSCRIBING state - non-blocking step machine.
// Each call executes one step then yields to allow loop() to run.
void V1BLEClient::processSubscribing() {
    const uint32_t now = static_cast<uint32_t>(millis());
    const uint32_t elapsed = now - connectStartMs_;

    if (elapsed > CONNECT_TIMEOUT_MS + DISCOVERY_TIMEOUT_MS + SUBSCRIBE_TIMEOUT_MS) {
        static BleLogRateLimitState subscribeTimeoutLog;
        if (shouldLogBleConnectionEvent(subscribeTimeoutLog, now)) {
            Serial.println("[BLE] Subscribe timeout");
        }
        {
            SemaphoreGuard lock(bleMutex_, pdMS_TO_TICKS(20)); // COLD: subscribe timeout
            shouldConnect_ = false;
        }
        connectInProgress_ = false;
        connectStartMs_ = 0;
        beginClientQuiesce();
        return;
    }

    const SubscribeStepResult result = executeSubscribeStep();
    if (result == SubscribeStepResult::Failed) {
        {
            SemaphoreGuard lock(bleMutex_, pdMS_TO_TICKS(20));
            shouldConnect_ = false;
        }
        connectInProgress_ = false;
        connectStartMs_ = 0;
        beginClientQuiesce();
        return;
    }

    if (result == SubscribeStepResult::Complete) {
        // All steps complete. Revalidate the session before each externally
        // visible publication. The connected flag is published before the
        // second gate read: if callback teardown closed the gate first, this
        // path retracts the flag; if teardown closes it later, its false store
        // is necessarily the later publication.
        const uint32_t completedGeneration = activeDiscoveryGeneration_;
        const auto sessionStillAccepted = [this, completedGeneration]() {
            return acceptClientCallbacks_.load(std::memory_order_acquire) &&
                   sessionGeneration_.load(std::memory_order_acquire) == completedGeneration &&
                   sessionPublicationGate_.accepts(completedGeneration);
        };
        if (!sessionStillAccepted()) {
            connected_.store(false, std::memory_order_release);
            beginClientQuiesce();
            return;
        }
        // Publish before the RMW claim. If callback teardown already closed
        // the gate, claim() must observe that close and this path retracts the
        // optimistic store. If claim() wins first, teardown's later false
        // store is ordered after this one and therefore remains authoritative.
        connected_.store(true, std::memory_order_release);
        if (!sessionPublicationGate_.claim(completedGeneration) || !sessionStillAccepted()) {
            connected_.store(false, std::memory_order_release);
            beginClientQuiesce();
            return;
        }
        const uint32_t connectedNowMs = static_cast<uint32_t>(millis());
        lastV1ConnectionEventMs_.store(connectedNowMs, std::memory_order_relaxed);
        connectCompletedAtMs_.store(connectedNowMs, std::memory_order_relaxed);
        firstRxAfterConnectMs_.store(0, std::memory_order_relaxed);
        connectBurstStableLoopCount_ = 0;
        connectedFollowupNextAttemptMs_ = 0;
        connectedFollowupSendDeadlineMs_ = connectedNowMs + CONNECTED_FOLLOWUP_SEND_TIMEOUT_MS;
        connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_ALERT_DATA;
        connectInProgress_ = false;
        connectStartMs_ = 0;
        if (!sessionStillAccepted()) {
            connected_.store(false, std::memory_order_release);
            beginClientQuiesce();
            return;
        }
        setBLEState(BLEState::CONNECTED);
        if (connectImmediateCallback_ && sessionStillAccepted()) {
            connectImmediateCallback_();
        }
        static BleLogRateLimitState subscribeOkLog;
        if (shouldLogBleConnectionEvent(subscribeOkLog, connectedNowMs)) {
            Serial.println("[BLE] OK");
        }
        return;
    }

    subscribeYieldUntilMs_ = static_cast<uint32_t>(millis()) + SUBSCRIBE_YIELD_MS;
    setBLEState(BLEState::SUBSCRIBE_YIELD);
}

void V1BLEClient::processSubscribeYield() {
    const uint32_t nowMs = static_cast<uint32_t>(millis());
    if (static_cast<int32_t>(nowMs - subscribeYieldUntilMs_) >= 0) {
        setBLEState(BLEState::SUBSCRIBING);
    }
}

V1BLEClient::SubscribeStepResult V1BLEClient::executeSubscribeStep() {
    switch (subscribeStep_) {
    case SubscribeStep::GET_SERVICE: {
        pRemoteService_ = pClient_->getService(V1_SERVICE_UUID);
        if (!pRemoteService_) {
            static BleLogRateLimitState subscribeFailServiceLog;
            if (shouldLogBleConnectionEvent(subscribeFailServiceLog, static_cast<uint32_t>(millis()))) {
                Serial.println("[BLE] FAIL service");
            }
            return SubscribeStepResult::Failed;
        }
        subscribeStep_ = SubscribeStep::GET_DISPLAY_CHAR;
        return SubscribeStepResult::InProgress;
    }

    case SubscribeStep::GET_DISPLAY_CHAR: {
        pDisplayDataChar_ = pRemoteService_->getCharacteristic(V1_DISPLAY_DATA_UUID);
        if (!pDisplayDataChar_) {
            static BleLogRateLimitState subscribeFailDisplayCharLog;
            if (shouldLogBleConnectionEvent(subscribeFailDisplayCharLog, static_cast<uint32_t>(millis()))) {
                Serial.println("[BLE] FAIL display char");
            }
            return SubscribeStepResult::Failed;
        }
        notifyShortChar_.store(pDisplayDataChar_, std::memory_order_release);
        notifyShortCharId_.store(shortUuid(pDisplayDataChar_->getUUID()), std::memory_order_release);
        subscribeStep_ = SubscribeStep::GET_COMMAND_CHAR;
        return SubscribeStepResult::InProgress;
    }

    case SubscribeStep::GET_COMMAND_CHAR: {
        pCommandChar_ = pRemoteService_->getCharacteristic(V1_COMMAND_WRITE_UUID);
        NimBLERemoteCharacteristic* altCommandChar = pRemoteService_->getCharacteristic(V1_COMMAND_WRITE_ALT_UUID);

        // Prefer primary, fall back to alt if needed.
        if (!pCommandChar_ || (!pCommandChar_->canWrite() && !pCommandChar_->canWriteNoResponse())) {
            if (altCommandChar && (altCommandChar->canWrite() || altCommandChar->canWriteNoResponse())) {
                pCommandChar_ = altCommandChar;
            } else {
                pCommandChar_ = nullptr;
                static BleLogRateLimitState subscribeFailCommandCharLog;
                if (shouldLogBleConnectionEvent(subscribeFailCommandCharLog, static_cast<uint32_t>(millis()))) {
                    Serial.println("[BLE] FAIL command char");
                }
                return SubscribeStepResult::Failed;
            }
        }
        subscribeStep_ = SubscribeStep::GET_COMMAND_LONG;
        return SubscribeStepResult::InProgress;
    }

    case SubscribeStep::GET_COMMAND_LONG:
        pCommandCharLong_ = pRemoteService_->getCharacteristic(V1_COMMAND_WRITE_LONG_UUID);
        subscribeStep_ = SubscribeStep::SUBSCRIBE_DISPLAY;
        return SubscribeStepResult::InProgress;

    case SubscribeStep::SUBSCRIBE_DISPLAY: {
        bool subscribed = false;
        if (pDisplayDataChar_->canNotify()) {
            subscribed = pDisplayDataChar_->subscribe(true, notifyCallback, true);
        } else if (pDisplayDataChar_->canIndicate()) {
            subscribed = pDisplayDataChar_->subscribe(false, notifyCallback);
        }

        if (!subscribed) {
            static BleLogRateLimitState subscribeFailB2ceLog;
            if (shouldLogBleConnectionEvent(subscribeFailB2ceLog, static_cast<uint32_t>(millis()))) {
                Serial.println("[BLE] FAIL subscribe B2CE");
            }
            return SubscribeStepResult::Failed;
        }
        subscribeStep_ = SubscribeStep::GET_DISPLAY_LONG;
        return SubscribeStepResult::InProgress;
    }

    case SubscribeStep::GET_DISPLAY_LONG: {
        // B4E0 is optional and used for voltage passthrough.
        NimBLERemoteCharacteristic* pDisplayLong = pRemoteService_->getCharacteristic(V1_DISPLAY_DATA_LONG_UUID);
        notifyLongChar_.store(pDisplayLong, std::memory_order_release);
        notifyLongCharId_.store(pDisplayLong ? shortUuid(pDisplayLong->getUUID()) : 0, std::memory_order_release);
        subscribeStep_ = (pDisplayLong && pDisplayLong->canNotify()) ? SubscribeStep::SUBSCRIBE_LONG
                                                                    : SubscribeStep::REQUEST_ALERT_DATA;
        return SubscribeStepResult::InProgress;
    }

    case SubscribeStep::SUBSCRIBE_LONG: {
        NimBLERemoteCharacteristic* pDisplayLong = pRemoteService_->getCharacteristic(V1_DISPLAY_DATA_LONG_UUID);
        if (pDisplayLong) {
            (void)pDisplayLong->subscribe(true, notifyCallback, true);
        }
        subscribeStep_ = SubscribeStep::REQUEST_ALERT_DATA;
        return SubscribeStepResult::InProgress;
    }

    case SubscribeStep::REQUEST_ALERT_DATA:
        subscribeStep_ = SubscribeStep::REQUEST_VERSION;
        return SubscribeStepResult::InProgress;

    case SubscribeStep::REQUEST_VERSION:
        subscribeStep_ = SubscribeStep::COMPLETE;
        return SubscribeStepResult::Complete;

    case SubscribeStep::COMPLETE:
        return SubscribeStepResult::Complete;
    }

    return SubscribeStepResult::Failed;
}
