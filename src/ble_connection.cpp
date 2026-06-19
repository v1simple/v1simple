/**
 * BLE connection state machine for Valentine1 Gen2
 *
 * Extracted from ble_client.cpp — scanning, connecting, service discovery,
 * characteristic subscription, and NimBLE callbacks.
 */

#include "ble_client.h"
#include "ble_log_rate_limit.h"
#include "ble_internals.h"
#include "config.h"
#include "perf_metrics.h"
#include <cstring>
#include <esp_heap_caps.h>

// --- scan callbacks ---

void V1BLEClient::ScanCallbacks::onResult(const NimBLEAdvertisedDevice* advertisedDevice) {
    if (!bleClient) {
        return;
    }

    const std::string& name = advertisedDevice->getName();
    const std::string& addrStr = advertisedDevice->getAddress().toString();
    int rssi = advertisedDevice->getRSSI();

    // Ignore our own proxy advertisement to avoid self-connect loops
    if (bleClient->proxyEnabled_) {
        NimBLEAddress selfAddr = NimBLEDevice::getAddress();
        if (advertisedDevice->getAddress() == selfAddr) {
            return;
        }
    }

    // V1 discovery should tolerate missing scan-response names: some stacks expose
    // the V1 service UUID without a readable name in the advertisement payload.
    bool nameLooksV1 = false;
    if (name.length() >= 3) {
        const char c0 = static_cast<char>(name[0] | 0x20);  // lowercase
        const char c1 = static_cast<char>(name[1] | 0x20);
        const char c2 = static_cast<char>(name[2] | 0x20);
        // Field variants: V1G..., V1C..., and V1-... have all been observed.
        nameLooksV1 = (c0 == 'v' && c1 == '1' && (c2 == 'g' || c2 == 'c' || c2 == '-'));
    }
    static const NimBLEUUID kV1ServiceUuid(V1_SERVICE_UUID);
    const bool serviceLooksV1 = advertisedDevice->isAdvertisingService(kV1ServiceUuid);
    const bool isV1 = nameLooksV1 || serviceLooksV1;

    if (!isV1) {
        // Not a V1 device, keep scanning
        return;
    }

    // *** FOUND V1! Stop scan and queue connection ***
    int advAddrType = advertisedDevice->getAddressType();

    // Check if we're already connecting or connected_
    if (bleClient->bleState_ == BLEState::CONNECTING ||
        bleClient->bleState_ == BLEState::CONNECTED) {
        return;
    }

    // Save this address and advertised name for future fast reconnect/proxy
    // identity (deferred to main loop).
    bleClient->deferLastV1Address(addrStr.c_str(), nameLooksV1 ? name.c_str() : nullptr);

    // Stop scanning - state machine will handle the connection after settle time
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan->isScanning()) {
        pScan->stop();
    }

    // Queue connection to this V1 device (non-blocking lock to avoid BLE callback stalls)
    // IMPORTANT: Don't copy full NimBLEAdvertisedDevice - it allocates memory which can fail
    // during heap pressure. Just store the address and type.
    SemaphoreGuard lock(bleClient->bleMutex_, 0);
    if (lock.locked()) {
        // Store just the address (no heap allocation)
        bleClient->targetAddress_ = advertisedDevice->getAddress();
        bleClient->targetAddressType_ = advAddrType;  // Save for reconnect
        bleClient->hasTargetDevice_ = true;
        bleClient->shouldConnect_ = true;
        bleClient->scanStopRequestedMs_ = static_cast<uint32_t>(millis());
        bleClient->setBLEState(BLEState::SCAN_STOPPING, "V1 found");
    } else {
        // Defer update to main loop if mutex is busy
        portENTER_CRITICAL(&pendingAddrMux);
        snprintf(bleClient->pendingScanTargetAddress_, sizeof(bleClient->pendingScanTargetAddress_), "%s", addrStr.c_str());
        bleClient->pendingScanTargetAddressType_ = static_cast<uint8_t>(advAddrType);
        bleClient->pendingScanTargetUpdate_ = true;
        portEXIT_CRITICAL(&pendingAddrMux);
    }
}

void V1BLEClient::ScanCallbacks::onScanEnd(const NimBLEScanResults& scanResults, int reason) {
    // If we were SCANNING and scan ended without finding V1, go back to DISCONNECTED
    // to allow process() to restart the scan
    if (instancePtr) {
        SemaphoreGuard lock(instancePtr->bleMutex_, 0);
        if (lock.locked()) {
            if (instancePtr->bleState_ == BLEState::SCANNING) {
                // Scan ended without finding V1, go back to DISCONNECTED
                instancePtr->setBLEState(BLEState::DISCONNECTED, "scan ended without finding V1");
            }
            // If SCAN_STOPPING, process() will handle the transition
        } else {
            instancePtr->pendingScanEndUpdate_ = true;
        }
    }
}

// --- client callbacks ---

void V1BLEClient::ClientCallbacks::onPhyUpdate(NimBLEClient* pClient_, uint8_t txPhy, uint8_t rxPhy) {
    // BLE callback: keep this notify-free.
}

void V1BLEClient::ClientCallbacks::onConnect(NimBLEClient* pClient_) {
    // NOTE: BLE callback - keep fast, no blocking operations
    if (instancePtr) {
        // Signal async connect success (non-blocking atomic write)
        instancePtr->asyncConnectSuccess_ = true;
        instancePtr->asyncConnectPending_ = false;

        SemaphoreGuard lock(instancePtr->bleMutex_, 0);
        if (lock.locked()) {
            instancePtr->connected_.store(true, std::memory_order_relaxed);
            // Don't set CONNECTED state here - let state machine handle it
            // The async state machine will transition through DISCOVERING -> SUBSCRIBING -> CONNECTED
        } else {
            instancePtr->pendingConnectStateUpdate_ = true;
        }
    }
}

void V1BLEClient::ClientCallbacks::onDisconnect(NimBLEClient* pClient_, int reason) {
    // NOTE: BLE callback - minimize blocking. Log disconnect reason for diagnostics.
    PERF_INC(disconnects);  // Count V1 disconnections
    // If the disconnect was unexpected (e.g., V1 powered off), clear bonding info
    // to ensure a clean reconnect next time.
    // NOTE: deleteBond() does NVS flash write — defer to main loop to avoid
    // blocking NimBLE host task.
    if (reason != 0 && reason != BLE_HS_ETIMEOUT) {
        if (instancePtr) {
            instancePtr->pendingDeleteBondAddr_ = pClient_->getPeerAddress();
            instancePtr->pendingDeleteBond_ = true;
        }
    }

    if (instancePtr) {
        instancePtr->lastV1ConnectionEventMs_.store(static_cast<uint32_t>(millis()),
                                                    std::memory_order_relaxed);
        instancePtr->verifyPushMatchEdgePending_.store(false, std::memory_order_relaxed);
        ProxyCallbackEvent event{};
        event.type = ProxyCallbackEventType::V1_DISCONNECTED;
        instancePtr->enqueueProxyCallbackEvent(event);

        SemaphoreGuard lock(instancePtr->bleMutex_, 0);
        if (lock.locked()) {
            instancePtr->connected_.store(false, std::memory_order_release);
            instancePtr->connectInProgress_ = false;  // Clear connection guard
            instancePtr->connectStartMs_ = 0;  // Clear async connect timer
            instancePtr->connectedFollowupStep_ = ConnectedFollowupStep::NONE;
            // Do NOT clear pClient_ - we reuse it to prevent memory leaks
            instancePtr->pRemoteService_ = nullptr;
            instancePtr->pDisplayDataChar_ = nullptr;
            instancePtr->pCommandChar_ = nullptr;
            instancePtr->pCommandCharLong_ = nullptr;
            // Store IDs before pointers: a reader that sees null pointer is guaranteed
            // to also see the zeroed ID (release ordering on both).
            instancePtr->notifyShortCharId_.store(0, std::memory_order_release);
            instancePtr->notifyShortChar_.store(nullptr, std::memory_order_release);
            instancePtr->notifyLongCharId_.store(0, std::memory_order_release);
            instancePtr->notifyLongChar_.store(nullptr, std::memory_order_release);
            // Reset verification state in case a write-verify was in progress
            instancePtr->verifyPending_ = false;
            instancePtr->verifyComplete_ = false;
            instancePtr->verifyMatch_ = false;
            // Clear backoff state so the reconnection cycle starts fresh.
            // A spontaneous V1 disconnect (power-off) is not a connect failure —
            // carrying forward failure counters causes escalating backoff that
            // prevents timely reconnection when the V1 comes back.
            instancePtr->consecutiveConnectFailures_ = 0;
            instancePtr->nextConnectAllowedMs_ = 0;
            // Clear stale scan target so the state machine does a full fresh scan
            // instead of trying to connect to a potentially-stale address.
            instancePtr->shouldConnect_ = false;
            instancePtr->hasTargetDevice_ = false;
            // Set state to DISCONNECTED - will trigger scan restart in process()
            instancePtr->setBLEState(BLEState::DISCONNECTED, "onDisconnect callback");
        } else {
            instancePtr->pendingDisconnectCleanup_ = true;
        }
    }
}

// --- connection state machine ---

bool V1BLEClient::connectToServer() {
    // ============================================================================
    // CONNECTION GUARDS
    // ============================================================================
    // Prevent overlapping connection attempts which cause EBUSY errors

    // Guard 1: Check if already connecting
    if (connectInProgress_) {
        return false;
    }

    // Guard 2: Check if scanning is still active - must be fully stopped
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
        pScan->stop();
        scanStopRequestedMs_ = static_cast<uint32_t>(millis());
        setBLEState(BLEState::SCAN_STOPPING, "connectToServer guard");
        return false;
    }

    // Set connection guard; CONNECTING state will initiate one async attempt
    // per loop() pass to avoid monopolizing a single iteration.
    connectInProgress_ = true;
    connectStartMs_ = static_cast<uint32_t>(millis());
    connectAttemptNumber_ = 0;  // Reset for new connection sequence
    asyncConnectPending_ = false;
    asyncConnectSuccess_ = false;
    connectPhaseStartUs_ = micros();  // Start timing connect phase
    setBLEState(BLEState::CONNECTING, "connectToServer");
    return true;
}

bool V1BLEClient::startAsyncConnect() {
    connectAttemptNumber_++;

    // CRITICAL: Stop proxy advertising - this competes with client connect!
    if (proxyEnabled_ && NimBLEDevice::getAdvertising()->isAdvertising()) {
        NimBLEDevice::stopAdvertising();
        perfRecordProxyAdvertisingTransition(
            false,
            static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopBeforeV1Connect),
            millis());
        // No delay - stopAdvertising is quick, radio will settle during connect
    }

    // Extra verify scan is stopped (should already be from SCAN_STOPPING state)
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
        pScan->stop();
    }

    // DON'T delete/recreate client - causes heap corruption and callback issues
    // Create client only if it doesn't exist
    if (!pClient_) {
        pClient_ = NimBLEDevice::createClient();
        if (!pClient_) {
            Serial.println("[BLE] ERROR: Failed to create client");
            connectInProgress_ = false;
            connectStartMs_ = 0;
            setBLEState(BLEState::DISCONNECTED, "client creation failed");
            return false;
        }
        // Create client callbacks if not already created
        if (!pClientCallbacks_) {
            pClientCallbacks_.reset(new ClientCallbacks());
        }
        pClient_->setClientCallbacks(pClientCallbacks_.get());
    }

    // Connection parameters: 12-24 (15-30ms interval), balanced for stability
    pClient_->setConnectionParams(NIMBLE_CONN_INTERVAL_MIN,
                                 NIMBLE_CONN_INTERVAL_MAX,
                                 NIMBLE_CONN_LATENCY,
                                 NIMBLE_CONN_SUPERVISION_TIMEOUT);
    // Preserve current active-connect timeout behavior.
    pClient_->setConnectTimeout(NIMBLE_CONNECT_TIMEOUT_ACTIVE_MS);
    // NimBLE 2.5.0 added a built-in connect retry (default=2). Disable it:
    // this project manages its own MAX_CONNECT_ATTEMPTS retry loop, and
    // NimBLE's internal retry would stack on top of the 3s wall-clock timeout
    // in processConnectingWait(), causing premature disconnect mid-retry.
    pClient_->setConnectRetries(0);

    // Ensure client is disconnected before attempting
    if (pClient_->isConnected()) {
        // Never block loop() waiting for disconnect. Drop back to scan flow and
        // let the next discovered V1 restart the connection sequence cleanly.
        Serial.println("[BLE] Client thinks it's connected_; clearing stale session");
        pClient_->disconnect();
        nextConnectAllowedMs_ = 0;
        connectInProgress_ = false;
        connectStartMs_ = 0;
        setBLEState(BLEState::DISCONNECTED, "waiting stale disconnect");
        return false;
    }

    // Clear async state before initiating connect
    asyncConnectPending_ = true;
    asyncConnectSuccess_ = false;

    // Use ASYNCHRONOUS connect - returns immediately, callback will set asyncConnectSuccess_
    // NimBLE 2.x: connect(address, deleteAttributes, asyncConnect, exchangeMTU)
    bool initiated = pClient_->connect(targetAddress_, true, true);

    if (!initiated) {
        int err = pClient_->getLastError();
        static BleLogRateLimitState connectInitiationFailedLog;
        if (shouldLogBleConnectionEvent(connectInitiationFailedLog,
                                        static_cast<uint32_t>(millis()))) {
            Serial.printf("[BLE] Async connect initiation failed (error: %d)\n", err);
        }
        asyncConnectPending_ = false;

        // Check if we should retry
        if (connectAttemptNumber_ < MAX_CONNECT_ATTEMPTS) {
            // Retry next loop pass; don't spin multiple attempts in one process() call.
            return true;  // Keep state machine going
        }

        // All attempts exhausted
        consecutiveConnectFailures_++;
        perfRecordBleConnectUs(micros() - connectPhaseStartUs_);

        if (hitsV1BleHardResetThreshold(consecutiveConnectFailures_)) {
            hardResetBLEClient();
            return false;
        }

        nextConnectAllowedMs_ = 0;
        connectInProgress_ = false;
        connectStartMs_ = 0;
        setBLEState(BLEState::DISCONNECTED, "connect initiation failed");
        return false;
    }

    // Async connect initiated - transition to CONNECTING_WAIT
    setBLEState(BLEState::CONNECTING_WAIT, "async connect initiated");
    return true;
}

// Called from CONNECTING_WAIT when async connect succeeds.
// Post-connect setup before service discovery.
bool V1BLEClient::finishConnection() {
    // Success!
    consecutiveConnectFailures_ = 0;
    nextConnectAllowedMs_ = 0;

    // Record connect phase time
    perfRecordBleConnectUs(micros() - connectPhaseStartUs_);
    PERF_INC(reconnects);  // Count successful (re)connections

    // Log the negotiated connection parameters (interval units = 1.25ms, timeout units = 10ms)
    logConnParams("post-connect");

    // Transition to discovery phase
    connectPhaseStartUs_ = micros();  // Reset timer for discovery phase
    discoveryTaskRunning_.store(false);
    discoveryTaskDone_.store(false);
    discoveryTaskResult_.store(false);
    setBLEState(BLEState::DISCOVERING, "ready for discovery");
    return true;
}

// Process CONNECTING_WAIT state - polls for async connect completion
void V1BLEClient::processConnectingWait() {
    const uint32_t now = static_cast<uint32_t>(millis());
    const uint32_t elapsed = now - connectStartMs_;

    // Check for async connect success (set by onConnect callback)
    if (asyncConnectSuccess_) {
        finishConnection();
        return;
    }

    // Check if still pending
    if (asyncConnectPending_) {
        // Check for overall timeout
        if (elapsed > CONNECT_TIMEOUT_MS) {
            asyncConnectPending_ = false;

            // Try to abort the pending connect
            if (pClient_) {
                pClient_->disconnect();
            }

            consecutiveConnectFailures_++;
            perfRecordBleConnectUs(micros() - connectPhaseStartUs_);

            if (hitsV1BleHardResetThreshold(consecutiveConnectFailures_)) {
                hardResetBLEClient();
                return;
            }

            nextConnectAllowedMs_ = 0;
            connectInProgress_ = false;
            connectStartMs_ = 0;
            setBLEState(BLEState::DISCONNECTED, "connect timeout");
        }
        return;  // Still waiting
    }

    // Async connect failed (asyncConnectPending_ cleared without asyncConnectSuccess_)
    int err = pClient_ ? pClient_->getLastError() : -1;
    static BleLogRateLimitState connectAttemptFailedLog;
    if (shouldLogBleConnectionEvent(connectAttemptFailedLog, now)) {
        Serial.printf("[BLE] Async connect attempt %d failed (error: %d)\n", connectAttemptNumber_, err);
    }

    // Check if we should retry
    if (connectAttemptNumber_ < MAX_CONNECT_ATTEMPTS) {
        if (err == 13) {  // EBUSY - defer via backoff instead of blocking main loop
            nextConnectAllowedMs_ = 0;
            connectInProgress_ = false;
            connectStartMs_ = 0;
            setBLEState(BLEState::DISCONNECTED, "EBUSY retry");
            return;
        }

        // Retry on the next loop iteration to keep each process() slice bounded.
        nextConnectAllowedMs_ = static_cast<uint32_t>(millis()) + 20;
        setBLEState(BLEState::CONNECTING, "async connect retry");
        return;
    }

    // All attempts exhausted
    consecutiveConnectFailures_++;
    perfRecordBleConnectUs(micros() - connectPhaseStartUs_);

    if (hitsV1BleHardResetThreshold(consecutiveConnectFailures_)) {
        hardResetBLEClient();
        return;
    }

    nextConnectAllowedMs_ = 0;
    connectInProgress_ = false;
    connectStartMs_ = 0;
    setBLEState(BLEState::DISCONNECTED, "all connect attempts failed");
}

// --- discovery ---

// Static trampoline for async discovery task
void V1BLEClient::discoveryTaskFunc(void* param) {
    V1BLEClient* self = static_cast<V1BLEClient*>(param);
    bool result = self->pClient_->discoverAttributes();
    self->discoveryTaskResult_.store(result);
    self->discoveryTaskDone_.store(true);
    self->discoveryTaskRunning_.store(false);
    // Self-delete: IDF recommends external deletion, but this fire-and-forget
    // task has no external owner.  Deferred cleanup (prvTaskDeleteWithCapsTask)
    // handles the stack free safely.
    vTaskDeleteWithCaps(nullptr);
}

// Process DISCOVERING state - spawns discovery in a short-lived task
// so the main loop stays responsive during the ~2s GATT discovery
void V1BLEClient::processDiscovering() {
    const uint32_t now = static_cast<uint32_t>(millis());
    const uint32_t elapsed = now - connectStartMs_;

    // Check for timeout
    // Safe even if discovery task is blocked: disconnect() sends HCI terminate,
    // NimBLE host task wakes the blocked task with BLE_HS_ENOTCONN, task exits cleanly
    if (elapsed > CONNECT_TIMEOUT_MS + DISCOVERY_TIMEOUT_MS) {
        static BleLogRateLimitState discoveryTimeoutLog;
        if (shouldLogBleConnectionEvent(discoveryTimeoutLog, now)) {
            Serial.println("[BLE] Discovery timeout");
        }
        perfRecordBleDiscoveryUs(micros() - connectPhaseStartUs_);
        disconnect();
        connectInProgress_ = false;
        connectStartMs_ = 0;
        setBLEState(BLEState::DISCONNECTED, "discovery timeout");
        return;
    }

    // Spawn discovery task on first entry
    // Guard: wait for any prior discovery task to finish (e.g. after timeout/disconnect)
    if (discoveryTaskRunning_.load()) {
        return;  // Previous task still winding down — yield
    }
    if (!discoveryTaskDone_.load()) {
        discoveryTaskRunning_.store(true);
        BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
            discoveryTaskFunc, "disc", 4096, this, 1, nullptr, tskNO_AFFINITY,
            MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (rc != pdPASS) {
            // Never run discovery synchronously on the main loop; back off and retry.
            static BleLogRateLimitState discoveryTaskCreateFailedLog;
            if (shouldLogBleConnectionEvent(discoveryTaskCreateFailedLog, now)) {
                Serial.println("[BLE] disc task create failed - backing off");
            }
            PERF_INC(bleDiscTaskCreateFail);
            discoveryTaskResult_.store(false);
            discoveryTaskDone_.store(true);
            discoveryTaskRunning_.store(false);
            consecutiveConnectFailures_++;
            if (hitsV1BleHardResetThreshold(consecutiveConnectFailures_)) {
                hardResetBLEClient();
                return;
            }

            nextConnectAllowedMs_ = 0;
            connectInProgress_ = false;
            connectStartMs_ = 0;
            disconnect();
            setBLEState(BLEState::DISCONNECTED, "discovery task create failed");
        }
        return;  // Yield to loop while task runs
    }

    perfRecordBleDiscoveryUs(micros() - connectPhaseStartUs_);

    if (!discoveryTaskResult_.load()) {
        static BleLogRateLimitState discoveryFailedLog;
        if (shouldLogBleConnectionEvent(discoveryFailedLog, now)) {
            Serial.println("[BLE] FAIL discovery");
        }
        disconnect();
        connectInProgress_ = false;
        connectStartMs_ = 0;
        setBLEState(BLEState::DISCONNECTED, "discovery failed");
        return;
    }

    // Transition to subscribe phase (uses step machine to break up CCCD writes)
    connectPhaseStartUs_ = micros();  // Reset timer for subscribe phase
    subscribeStep_ = SubscribeStep::GET_SERVICE;
    subscribeStepStartUs_ = micros();
    setBLEState(BLEState::SUBSCRIBING, "discovery complete");
}

// --- subscribe step machine ---

// Process SUBSCRIBING state - non-blocking step machine
// Each call executes one step then yields to allow loop() to run
void V1BLEClient::processSubscribing() {
    const uint32_t now = static_cast<uint32_t>(millis());
    const uint32_t elapsed = now - connectStartMs_;

    // Check for overall timeout
    if (elapsed > CONNECT_TIMEOUT_MS + DISCOVERY_TIMEOUT_MS + SUBSCRIBE_TIMEOUT_MS) {
        static BleLogRateLimitState subscribeTimeoutLog;
        if (shouldLogBleConnectionEvent(subscribeTimeoutLog, now)) {
            Serial.println("[BLE] Subscribe timeout");
        }
        perfRecordBleSubscribeUs(micros() - connectPhaseStartUs_);
        disconnect();
        {
            SemaphoreGuard lock(bleMutex_, pdMS_TO_TICKS(20));  // COLD: subscribe timeout
            shouldConnect_ = false;
            hasTargetDevice_ = false;
        }
        connectInProgress_ = false;
        connectStartMs_ = 0;
        setBLEState(BLEState::DISCONNECTED, "subscribe timeout");
        return;
    }

    // Execute one subscribe step
    subscribeStepStartUs_ = micros();
    bool done = executeSubscribeStep();
    uint32_t stepDuration = micros() - subscribeStepStartUs_;

    // Record step timing for attribution
    if (perfExtended.bleSubscribeMaxUs < stepDuration) {
        perfExtended.bleSubscribeMaxUs = stepDuration;
    }

    if (done) {
        // All steps complete - success!
        {
            SemaphoreGuard lock(bleMutex_, pdMS_TO_TICKS(20));  // COLD: subscribe complete
            connected_.store(true, std::memory_order_relaxed);
        }
        const uint32_t connectedNowMs = static_cast<uint32_t>(millis());
        lastV1ConnectionEventMs_.store(connectedNowMs, std::memory_order_relaxed);
        connectCompletedAtMs_.store(connectedNowMs, std::memory_order_relaxed);
        firstRxAfterConnectMs_.store(0, std::memory_order_relaxed);
        connectBurstStableLoopCount_ = 0;
        connectedFollowupStep_ = ConnectedFollowupStep::REQUEST_ALERT_DATA;
        perfRecordBleSubscribeUs(micros() - connectPhaseStartUs_);
        connectInProgress_ = false;
        connectStartMs_ = 0;
        setBLEState(BLEState::CONNECTED, "subscribe complete");
        if (connectImmediateCallback_) {
            connectImmediateCallback_();
        }
        static BleLogRateLimitState subscribeOkLog;
        if (shouldLogBleConnectionEvent(subscribeOkLog, connectedNowMs)) {
            Serial.println("[BLE] OK");
        }
        return;
    }

    // Step completed but more to do - yield to loop()
    subscribeYieldUntilMs_ = static_cast<uint32_t>(millis()) + SUBSCRIBE_YIELD_MS;
    setBLEState(BLEState::SUBSCRIBE_YIELD, "yield between steps");
}

// Process SUBSCRIBE_YIELD state - wait briefly then resume subscribing
void V1BLEClient::processSubscribeYield() {
    const uint32_t nowMs = static_cast<uint32_t>(millis());
    if (static_cast<int32_t>(nowMs - subscribeYieldUntilMs_) >= 0) {
        setBLEState(BLEState::SUBSCRIBING, "resuming subscribe");
    }
}

// Execute one subscribe step, return true when all steps complete
bool V1BLEClient::executeSubscribeStep() {
    switch (subscribeStep_) {
        case SubscribeStep::GET_SERVICE: {
            pRemoteService_ = pClient_->getService(V1_SERVICE_UUID);
            if (!pRemoteService_) {
                static BleLogRateLimitState subscribeFailServiceLog;
                if (shouldLogBleConnectionEvent(subscribeFailServiceLog,
                                                static_cast<uint32_t>(millis()))) {
                    Serial.println("[BLE] FAIL service");
                }
                return false;  // Will trigger failure handling
            }
            subscribeStep_ = SubscribeStep::GET_DISPLAY_CHAR;
            return false;  // More steps to do
        }

        case SubscribeStep::GET_DISPLAY_CHAR: {
            pDisplayDataChar_ = pRemoteService_->getCharacteristic(V1_DISPLAY_DATA_UUID);
            if (!pDisplayDataChar_) {
                static BleLogRateLimitState subscribeFailDisplayCharLog;
                if (shouldLogBleConnectionEvent(subscribeFailDisplayCharLog,
                                                static_cast<uint32_t>(millis()))) {
                    Serial.println("[BLE] FAIL display char");
                }
                return false;
            }
            notifyShortChar_.store(pDisplayDataChar_, std::memory_order_release);
            notifyShortCharId_.store(shortUuid(pDisplayDataChar_->getUUID()), std::memory_order_release);
            subscribeStep_ = SubscribeStep::GET_COMMAND_CHAR;
            return false;
        }

        case SubscribeStep::GET_COMMAND_CHAR: {
            pCommandChar_ = pRemoteService_->getCharacteristic(V1_COMMAND_WRITE_UUID);
            NimBLERemoteCharacteristic* altCommandChar = pRemoteService_->getCharacteristic(V1_COMMAND_WRITE_ALT_UUID);

            // Prefer primary, fall back to alt if needed
            if (!pCommandChar_ || (!pCommandChar_->canWrite() && !pCommandChar_->canWriteNoResponse())) {
                if (altCommandChar && (altCommandChar->canWrite() || altCommandChar->canWriteNoResponse())) {
                    pCommandChar_ = altCommandChar;
                } else {
                    pCommandChar_ = nullptr;
                    static BleLogRateLimitState subscribeFailCommandCharLog;
                    if (shouldLogBleConnectionEvent(subscribeFailCommandCharLog,
                                                    static_cast<uint32_t>(millis()))) {
                        Serial.println("[BLE] FAIL command char");
                    }
                    return false;
                }
            }
            subscribeStep_ = SubscribeStep::GET_COMMAND_LONG;
            return false;
        }

        case SubscribeStep::GET_COMMAND_LONG: {
            pCommandCharLong_ = pRemoteService_->getCharacteristic(V1_COMMAND_WRITE_LONG_UUID);
            // B8D2 is optional - don't log either way
            subscribeStep_ = SubscribeStep::SUBSCRIBE_DISPLAY;
            return false;
        }

        case SubscribeStep::SUBSCRIBE_DISPLAY: {
            bool subscribed = false;
            if (pDisplayDataChar_->canNotify()) {
                subscribed = pDisplayDataChar_->subscribe(true, notifyCallback, true);
            } else if (pDisplayDataChar_->canIndicate()) {
                subscribed = pDisplayDataChar_->subscribe(false, notifyCallback);
            }

            if (!subscribed) {
                static BleLogRateLimitState subscribeFailB2ceLog;
                if (shouldLogBleConnectionEvent(subscribeFailB2ceLog,
                                                static_cast<uint32_t>(millis()))) {
                    Serial.println("[BLE] FAIL subscribe B2CE");
                }
                return false;
            }
            subscribeStep_ = SubscribeStep::GET_DISPLAY_LONG;
            return false;
        }

        case SubscribeStep::GET_DISPLAY_LONG: {
            // Get B4E0 characteristic (non-critical, used for voltage passthrough)
            NimBLERemoteCharacteristic* pDisplayLong = pRemoteService_->getCharacteristic(V1_DISPLAY_DATA_LONG_UUID);
            notifyLongChar_.store(pDisplayLong, std::memory_order_release);
            notifyLongCharId_.store(pDisplayLong ? shortUuid(pDisplayLong->getUUID()) : 0,
                                   std::memory_order_release);
            if (pDisplayLong && pDisplayLong->canNotify()) {
                subscribeStep_ = SubscribeStep::SUBSCRIBE_LONG;
            } else {
                subscribeStep_ = SubscribeStep::REQUEST_ALERT_DATA;  // Skip LONG subscribe
            }
            return false;
        }

        case SubscribeStep::SUBSCRIBE_LONG: {
            NimBLERemoteCharacteristic* pDisplayLong = pRemoteService_->getCharacteristic(V1_DISPLAY_DATA_LONG_UUID);
            if (pDisplayLong && pDisplayLong->subscribe(true, notifyCallback, true)) {
                subscribeStep_ = SubscribeStep::REQUEST_ALERT_DATA;
            } else {
                subscribeStep_ = SubscribeStep::REQUEST_ALERT_DATA;
            }
            return false;
        }

        case SubscribeStep::REQUEST_ALERT_DATA: {
            subscribeStep_ = SubscribeStep::REQUEST_VERSION;
            return false;
        }

        case SubscribeStep::REQUEST_VERSION: {
            subscribeStep_ = SubscribeStep::COMPLETE;
            return true;  // All done!
        }

        case SubscribeStep::COMPLETE:
            return true;  // Already complete
    }

    return true;  // Shouldn't reach here
}

// --- helpers ---

void V1BLEClient::logConnParams(const char* tag) {
    if (!pClient_) {
        return;
    }

    NimBLEConnInfo info = pClient_->getConnInfo();
    float intervalMs = info.getConnInterval() * 1.25f;

    static BleLogRateLimitState connParamsLog;
    if (shouldLogBleConnectionEvent(connParamsLog, static_cast<uint32_t>(millis()))) {
        Serial.printf("[BLE] Conn params (%s): interval=%.2f ms latency=%u\n",
                      tag ? tag : "n/a",
                      intervalMs,
                      info.getConnLatency());
    }
}

// --- notify callback ---

// Subscription setup now runs through the non-blocking executeSubscribeStep()
// state machine instead of a single monolithic helper.
void V1BLEClient::notifyCallback(NimBLERemoteCharacteristic* pChar,
                                  uint8_t* pData,
                                  size_t length,
                                  bool isNotify) {
    if (!pData || !instancePtr || !pChar) {
        return;
    }

    uint16_t charId = 0;
    NimBLERemoteCharacteristic* shortChar = instancePtr->notifyShortChar_.load(std::memory_order_acquire);
    NimBLERemoteCharacteristic* longChar = instancePtr->notifyLongChar_.load(std::memory_order_acquire);
    if (pChar == shortChar) {
        charId = instancePtr->notifyShortCharId_.load(std::memory_order_acquire);
    } else if (pChar == longChar) {
        charId = instancePtr->notifyLongCharId_.load(std::memory_order_acquire);
    }
    if (charId == 0) {
        charId = shortUuid(pChar->getUUID());
    }

    if (charId == 0) {
        charId = 0xB2CE; // sensible fallback
    }

    // Check if this is a response packet that should go to B4E0
    // V1 sends responses on B2CE but some apps expect certain responses on B4E0
    // Testing: Keep voltage (0x63) on B2CE since Kenny's code receives it there
    uint16_t routeCharId = charId;
    if (charId == 0xB2CE && length >= 5) {
        uint8_t packetId = pData[3];  // Packet ID is at offset 3 in V1 protocol
        // Route ONLY sweep/alert response packets to B4E0
        // Keep voltage (0x63), version (0x01), serial (0x03) on B2CE
        if (packetId == 0x41 ||  // respAlertData
            packetId == 0x42) {  // respSweepSection
            routeCharId = 0xB4E0;
        }
    }

    // Forward to proxy via the proxy queue only. Keep BLE callback path notify-free.
    instancePtr->forwardToProxy(pData, length, routeCharId);

    if (instancePtr->connected_.load(std::memory_order_relaxed) &&
        instancePtr->firstRxAfterConnectMs_.load(std::memory_order_relaxed) == 0) {
        instancePtr->firstRxAfterConnectMs_.store(static_cast<uint32_t>(millis()), std::memory_order_relaxed);
    }

    // Call user callback for display processing (queued to main loop for SPI safety)
    if (instancePtr->dataCallback_) {
        instancePtr->dataCallback_(pData, length, charId);
    }
}
