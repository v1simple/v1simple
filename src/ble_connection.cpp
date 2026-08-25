/**
 * Valentine One Gen2 scanning, connection, discovery, and notification callbacks.
 */

#include "ble_client.h"
#include "ble_log_rate_limit.h"
#include "ble_internals.h"
#include "config.h"
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
        const char c0 = static_cast<char>(name[0] | 0x20); // lowercase
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

    // Stop scanning and queue the discovered V1 connection.
    int advAddrType = advertisedDevice->getAddressType();

    // Record the discovered address as runtime/backup compatibility state and
    // adopt the advertised name for proxy identity (deferred to main loop).
    bleClient->deferLastV1Address(addrStr.c_str(), nameLooksV1 ? name.c_str() : nullptr);

    // Publish only an immutable address edge from callback context. The main
    // loop accepts it only while it still owns the SCANNING state; a delayed
    // callback can therefore never overwrite an active connection session.
    // Avoid copying NimBLEAdvertisedDevice because it allocates under heap pressure.
    portENTER_CRITICAL(&pendingAddrMux);
    snprintf(bleClient->pendingScanTargetAddress_, sizeof(bleClient->pendingScanTargetAddress_), "%s", addrStr.c_str());
    bleClient->pendingScanTargetAddressType_ = static_cast<uint8_t>(advAddrType);
    bleClient->pendingScanTargetUpdate_.store(true, std::memory_order_release);
    portEXIT_CRITICAL(&pendingAddrMux);

    // Publish the target before requesting stop so scan-end handling cannot
    // retire SCANNING first, even if NimBLE changes callback scheduling.
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan->isScanning()) {
        pScan->stop();
    }
}

void V1BLEClient::ScanCallbacks::onScanEnd(const NimBLEScanResults& scanResults, int reason) {
    (void)scanResults;
    (void)reason;
    // Callback context publishes an event only. The main loop exclusively owns
    // connection-state transitions and decides whether this was an empty scan
    // or a target-driven stop after inspecting pendingScanTargetUpdate_.
    if (instancePtr) {
        instancePtr->pendingScanEndUpdate_.store(true, std::memory_order_release);
    }
}

// --- client callbacks ---

void V1BLEClient::ClientCallbacks::onPhyUpdate(NimBLEClient* pClient_, uint8_t txPhy, uint8_t rxPhy) {
    // BLE callback: keep this notify-free.
}

void V1BLEClient::ClientCallbacks::onConnect(NimBLEClient* pClient_) {
    // NimBLE callback: publish state only and avoid blocking work.
    if (instancePtr) {
        const uint32_t callbackGeneration = instancePtr->sessionGeneration_.load(std::memory_order_acquire);
        // A connect completion can race a timeout/cancel edge. Once quiescing
        // begins, never publish that late completion into the next session.
        if (!instancePtr->acceptClientCallbacks_.load(std::memory_order_acquire) ||
            !instancePtr->sessionPublicationGate_.accepts(callbackGeneration)) {
            instancePtr->quiescingConnectionHandle_.store(pClient_->getConnHandle(), std::memory_order_release);
            instancePtr->disconnectCallbackPending_.store(true, std::memory_order_relaxed);
            instancePtr->asyncConnectSuccess_.store(false, std::memory_order_relaxed);
            instancePtr->asyncConnectPending_.store(false, std::memory_order_release);
            if (!pClient_->disconnect()) {
                instancePtr->disconnectCallbackPending_.store(false, std::memory_order_release);
            }
            return;
        }

        instancePtr->activeConnectionHandle_.store(pClient_->getConnHandle(), std::memory_order_release);

        instancePtr->asyncConnectSuccess_ = true;
        instancePtr->asyncConnectPending_ = false;
        // The callback must not publish connected_=true directly: quiescence
        // can close acceptClientCallbacks_ immediately after the acquire above.
        // Main-loop processing rechecks that gate before publishing link state;
        // subscribe completion later confirms the fully usable session.
        instancePtr->pendingConnectStateGeneration_.store(callbackGeneration, std::memory_order_relaxed);
        instancePtr->pendingConnectStateUpdate_.store(true, std::memory_order_release);
    }
}

void V1BLEClient::ClientCallbacks::onConnectFail(NimBLEClient* pClient_, int reason) {
    (void)pClient_;
    (void)reason;
    if (!instancePtr) {
        return;
    }

    // NimBLE reports async cancellation/failure here. Publishing completion is
    // enough: the main-loop state machine owns retry and teardown decisions.
    instancePtr->asyncConnectSuccess_.store(false, std::memory_order_relaxed);
    instancePtr->asyncConnectPending_.store(false, std::memory_order_release);
}

void V1BLEClient::ClientCallbacks::onDisconnect(NimBLEClient* pClient_, int reason) {
    // NimBLE callback: defer teardown work to the main loop.
    if (instancePtr) {
        const uint16_t callbackHandle = pClient_->getConnHandle();
        const bool accepting = instancePtr->acceptClientCallbacks_.load(std::memory_order_acquire);
        const uint16_t expectedHandle = accepting
                                            ? instancePtr->activeConnectionHandle_.load(std::memory_order_acquire)
                                            : instancePtr->quiescingConnectionHandle_.load(std::memory_order_acquire);
        // NimBLE invokes onDisconnect before it clears m_connHandle. Rejecting
        // a mismatched handle prevents a delayed callback from an older link
        // from tearing down the current session.
        if (expectedHandle != 0xFFFF && callbackHandle != expectedHandle) {
            return;
        }
    }

    // If the disconnect was unexpected (e.g., V1 powered off), clear bonding info
    // to ensure a clean reconnect next time.
    // deleteBond() writes NVS, so defer it rather than blocking the host task.
    if (reason != 0 && reason != BLE_HS_ETIMEOUT) {
        if (instancePtr) {
            const NimBLEAddress peerAddr = pClient_->getPeerAddress();
            // Guard the non-atomic address against a concurrent main-loop read.
            portENTER_CRITICAL(&pendingAddrMux);
            instancePtr->pendingDeleteBondAddr_ = peerAddr;
            instancePtr->pendingDeleteBond_ = true;
            portEXIT_CRITICAL(&pendingAddrMux);
        }
    }

    if (instancePtr) {
        instancePtr->lastV1ConnectionEventMs_.store(static_cast<uint32_t>(millis()), std::memory_order_relaxed);
        instancePtr->verifyPushMatchEdgePending_.store(false, std::memory_order_relaxed);
        ProxyCallbackEvent event{};
        event.type = ProxyCallbackEventType::V1_DISCONNECTED;
        instancePtr->enqueueProxyCallbackEvent(event);

        // Remote attribute ownership stays on the main loop. The callback only
        // closes the publication gate and queues teardown; this prevents it
        // from nulling a pointer between a main-loop check and dereference.
        instancePtr->acceptClientCallbacks_.store(false, std::memory_order_release);
        instancePtr->sessionPublicationGate_.close();
        instancePtr->connected_.store(false, std::memory_order_release);
        instancePtr->asyncConnectSuccess_.store(false, std::memory_order_relaxed);
        instancePtr->asyncConnectPending_.store(false, std::memory_order_release);
        instancePtr->disconnectCallbackPending_.store(false, std::memory_order_release);
        instancePtr->pendingDisconnectReason_.store(reason, std::memory_order_relaxed);
        instancePtr->pendingDisconnectCleanup_.store(true, std::memory_order_release);
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

    // discoverAttributes() mutates/deletes the client's cached services. Never
    // let a new connection begin until the prior discovery task has published
    // its final completion and exited the client-mutating portion.
    if (bleState_ == BLEState::QUIESCING || discoveryTaskRunning_.load(std::memory_order_acquire)) {
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

    // Start a fresh ownership generation. Retries within this connection
    // sequence share it; teardown invalidates it before releasing the client.
    uint32_t nextGeneration = sessionGeneration_.load(std::memory_order_relaxed) + 1;
    if (nextGeneration == 0) {
        nextGeneration = 1;
    }
    sessionGeneration_.store(nextGeneration, std::memory_order_release);
    pendingConnectStateUpdate_.store(false, std::memory_order_relaxed);
    pendingConnectStateGeneration_.store(0, std::memory_order_relaxed);
    sessionPublicationGate_.open(nextGeneration);
    acceptClientCallbacks_.store(true, std::memory_order_release);

    // Set connection guard; CONNECTING state will initiate one async attempt
    // per loop() pass to avoid monopolizing a single iteration.
    connectInProgress_ = true;
    connectStartMs_ = static_cast<uint32_t>(millis());
    connectAttemptNumber_ = 0; // Reset for new connection sequence
    asyncConnectPending_ = false;
    asyncConnectSuccess_ = false;
    setBLEState(BLEState::CONNECTING, "connectToServer");
    return true;
}

bool V1BLEClient::startAsyncConnect() {
    if (bleState_ == BLEState::QUIESCING || discoveryTaskRunning_.load(std::memory_order_acquire)) {
        return false;
    }

    connectAttemptNumber_++;

    // Stop proxy advertising before using the radio for the client connection.
    if (proxyEnabled_ && NimBLEDevice::getAdvertising()->isAdvertising()) {
        NimBLEDevice::stopAdvertising();
    }

    // Ensure no scan remains active before connecting.
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
        pScan->stop();
    }

    // Reuse the client because runtime deletion corrupts NimBLE bookkeeping.
    if (!pClient_) {
        pClient_ = NimBLEDevice::createClient();
        if (!pClient_) {
            Serial.println("[BLE] ERROR: Failed to create client");
            connectInProgress_ = false;
            connectStartMs_ = 0;
            setBLEState(BLEState::DISCONNECTED, "client creation failed");
            return false;
        }
        // Callbacks share the client's lifetime.
        if (!pClientCallbacks_) {
            pClientCallbacks_.reset(new ClientCallbacks());
        }
        pClient_->setClientCallbacks(pClientCallbacks_.get());
    }

    // Connection parameters: 12-24 (15-30ms interval), balanced for stability
    pClient_->setConnectionParams(NIMBLE_CONN_INTERVAL_MIN, NIMBLE_CONN_INTERVAL_MAX, NIMBLE_CONN_LATENCY,
                                  NIMBLE_CONN_SUPERVISION_TIMEOUT);
    // Preserve current active-connect timeout behavior.
    pClient_->setConnectTimeout(NIMBLE_CONNECT_TIMEOUT_ACTIVE_MS);
    // NimBLE 2.5.0 added a built-in connect retry (default=2). Disable it:
    // this project manages its own MAX_CONNECT_ATTEMPTS retry loop, and
    // NimBLE's internal retry would stack on top of the 3s wall-clock timeout
    // in processConnectingWait(), causing premature disconnect mid-retry.
    pClient_->setConnectRetries(0);

    // A stale connection is quiesced asynchronously before another attempt.
    if (pClient_->isConnected()) {
        // Keep disconnect asynchronous. Drop back to scan flow and
        // let the next discovered V1 restart the connection sequence cleanly.
        Serial.println("[BLE] Client thinks it's connected_; clearing stale session");
        nextConnectAllowedMs_ = 0;
        connectInProgress_ = false;
        connectStartMs_ = 0;
        beginClientQuiesce("waiting stale disconnect");
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
        if (shouldLogBleConnectionEvent(connectInitiationFailedLog, static_cast<uint32_t>(millis()))) {
            Serial.printf("[BLE] Async connect initiation failed (error: %d)\n", err);
        }
        asyncConnectPending_ = false;

        // Check if we should retry
        if (connectAttemptNumber_ < MAX_CONNECT_ATTEMPTS) {
            // Retry next loop pass; don't spin multiple attempts in one process() call.
            return true; // Keep state machine going
        }

        // All attempts exhausted
        consecutiveConnectFailures_++;

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
    const uint32_t currentGeneration = sessionGeneration_.load(std::memory_order_acquire);
    if (!acceptClientCallbacks_.load(std::memory_order_acquire) ||
        !sessionPublicationGate_.accepts(currentGeneration) || discoveryTaskRunning_.load(std::memory_order_acquire)) {
        beginClientQuiesce("late connect completion");
        return false;
    }

    // Success!
    consecutiveConnectFailures_ = 0;
    nextConnectAllowedMs_ = 0;

    // Log the negotiated connection parameters (interval units = 1.25ms, timeout units = 10ms)
    logConnParams("post-connect");

    // Transition to discovery phase
    activeDiscoveryGeneration_ = currentGeneration;
    discoveryTaskDone_.store(false, std::memory_order_relaxed);
    discoveryTaskResult_.store(false, std::memory_order_relaxed);
    discoveryCompletedGeneration_.store(0, std::memory_order_relaxed);
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
            consecutiveConnectFailures_++;

            if (hitsV1BleHardResetThreshold(consecutiveConnectFailures_)) {
                hardResetBLEClient();
                return;
            }

            nextConnectAllowedMs_ = 0;
            connectInProgress_ = false;
            connectStartMs_ = 0;
            beginClientQuiesce("connect timeout");
        }
        return; // Still waiting
    }

    // Async connect failed (asyncConnectPending_ cleared without asyncConnectSuccess_)
    int err = pClient_ ? pClient_->getLastError() : -1;
    static BleLogRateLimitState connectAttemptFailedLog;
    if (shouldLogBleConnectionEvent(connectAttemptFailedLog, now)) {
        Serial.printf("[BLE] Async connect attempt %d failed (error: %d)\n", connectAttemptNumber_, err);
    }

    // Check if we should retry
    if (connectAttemptNumber_ < MAX_CONNECT_ATTEMPTS) {
        if (err == 13) { // EBUSY - defer via backoff instead of blocking main loop
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

    if (hitsV1BleHardResetThreshold(consecutiveConnectFailures_)) {
        hardResetBLEClient();
        return;
    }

    nextConnectAllowedMs_ = 0;
    connectInProgress_ = false;
    connectStartMs_ = 0;
    setBLEState(BLEState::DISCONNECTED, "all connect attempts failed");
}

void V1BLEClient::beginClientQuiesce(const char* reason, bool requestHardReset) {
    hardResetPending_ = hardResetPending_ || requestHardReset;

    if (bleState_ != BLEState::QUIESCING) {
        // Invalidate every task/result/pointer published for the outgoing
        // session before asking NimBLE to cancel work on that session.
        acceptClientCallbacks_.store(false, std::memory_order_release);
        sessionPublicationGate_.close();
        connected_.store(false, std::memory_order_release);
        uint32_t nextGeneration = sessionGeneration_.load(std::memory_order_relaxed) + 1;
        if (nextGeneration == 0) {
            nextGeneration = 1;
        }
        sessionGeneration_.store(nextGeneration, std::memory_order_release);
        if (sessionClosedCallback_) {
            sessionClosedCallback_(nextGeneration);
        }
        connectInProgress_ = false;
        connectStartMs_ = 0;
        connectedFollowupStep_ = ConnectedFollowupStep::NONE;
        quiesceStartedMs_ = static_cast<uint32_t>(millis());
        quiesceLastRetryMs_ = quiesceStartedMs_;
        uint16_t outgoingHandle = activeConnectionHandle_.load(std::memory_order_acquire);
        if (outgoingHandle == 0xFFFF && pClient_ && pClient_->isConnected()) {
            outgoingHandle = pClient_->getConnHandle();
        }
        quiescingConnectionHandle_.store(outgoingHandle, std::memory_order_release);
        setBLEState(BLEState::QUIESCING, reason);

        NimBLEScan* scan = NimBLEDevice::getScan();
        if (scan && scan->isScanning()) {
            scan->stop();
        }
    }

    if (pClient_ && asyncConnectPending_.load(std::memory_order_acquire)) {
        quiesceAwaitingConnectCancel_ = true;
        pClient_->cancelConnect();
    }

    if (pClient_ && pClient_->isConnected() && !disconnectCallbackPending_.load(std::memory_order_acquire)) {
        disconnectCallbackPending_.store(true, std::memory_order_release);
        if (!pClient_->disconnect()) {
            // Retry from processClientQuiesce(); never block the loop here.
            disconnectCallbackPending_.store(false, std::memory_order_release);
        }
    }
}

void V1BLEClient::processClientQuiesce() {
    if (quiesceAwaitingConnectCancel_) {
        if (!asyncConnectPending_.load(std::memory_order_acquire)) {
            quiesceAwaitingConnectCancel_ = false;
        }
    }

    const bool waitingForConnectCancel = quiesceAwaitingConnectCancel_;
    const bool waitingForDiscovery = discoveryTaskRunning_.load(std::memory_order_acquire);
    const bool waitingForDisconnect = disconnectCallbackPending_.load(std::memory_order_acquire);
    const bool clientStillConnected = pClient_ && pClient_->isConnected();
    const uint32_t now = static_cast<uint32_t>(millis());
    const ble_quiesce_policy::Decision decision = ble_quiesce_policy::evaluate({
        waitingForConnectCancel,
        waitingForDiscovery,
        waitingForDisconnect,
        clientStillConnected,
        now,
        quiesceStartedMs_,
        quiesceLastRetryMs_,
        QUIESCE_RETRY_MS,
        QUIESCE_FATAL_TIMEOUT_MS,
    });

    // Apply one deadline to every incomplete teardown path, including the
    // case where disconnect() repeatedly refuses the request while NimBLE
    // continues to report the client connected.
    if (decision.action == ble_quiesce_policy::Action::FATAL_RESTART) {
        quiesceTimeoutRecoveryCount_.fetch_add(1, std::memory_order_relaxed);
        Serial.printf(
            "[BLE] FATAL quiesce timeout: cancel=%u discovery=%u disconnect=%u connected=%u handle=%u; restarting\n",
            waitingForConnectCancel ? 1u : 0u, waitingForDiscovery ? 1u : 0u, waitingForDisconnect ? 1u : 0u,
            clientStillConnected ? 1u : 0u,
            static_cast<unsigned>(quiescingConnectionHandle_.load(std::memory_order_relaxed)));
        ESP.restart();
        return;
    }

    if (decision.action == ble_quiesce_policy::Action::WAIT ||
        decision.action == ble_quiesce_policy::Action::RETRY_PENDING_OPERATIONS) {
        // The pinned NimBLE 2.5.0 client invokes onDisconnect while the old
        // m_connHandle is still available, then clears the handle/status after
        // the callback returns. We therefore do not release this generation on
        // isConnected()==false alone: the matching callback is the ownership
        // fence. If NimBLE violates that contract or discovery never unwinds,
        // fail closed with a device restart; never admit a new session that a
        // late callback could tear down.
        if (decision.action == ble_quiesce_policy::Action::RETRY_PENDING_OPERATIONS) {
            quiesceLastRetryMs_ = now;
            if (decision.retryConnectCancel && pClient_) {
                pClient_->cancelConnect();
            }
            if (decision.retryDisconnect && pClient_) {
                pClient_->disconnect();
            }
        }
        return;
    }

    // If a disconnect request failed before it reached the controller, retry
    // one non-blocking request per loop pass and wait for its callback.
    if (decision.action == ble_quiesce_policy::Action::REQUEST_DISCONNECT) {
        disconnectCallbackPending_.store(true, std::memory_order_release);
        if (!pClient_->disconnect()) {
            disconnectCallbackPending_.store(false, std::memory_order_release);
        }
        return;
    }

    // No task can mutate NimBLE's attribute cache beyond this point. It is now
    // safe for the main-loop owner to retire all outgoing remote handles.
    cleanupConnection();
    discoveryTaskDone_.store(false, std::memory_order_relaxed);
    discoveryTaskResult_.store(false, std::memory_order_relaxed);
    discoveryCompletedGeneration_.store(0, std::memory_order_relaxed);
    activeDiscoveryGeneration_ = 0;
    activeConnectionHandle_.store(0xFFFF, std::memory_order_relaxed);
    quiescingConnectionHandle_.store(0xFFFF, std::memory_order_relaxed);
    quiesceStartedMs_ = 0;
    quiesceLastRetryMs_ = 0;
    asyncConnectPending_.store(false, std::memory_order_relaxed);
    asyncConnectSuccess_.store(false, std::memory_order_relaxed);
    pendingConnectStateUpdate_.store(false, std::memory_order_relaxed);
    pendingConnectStateGeneration_.store(0, std::memory_order_relaxed);

    if (hardResetPending_) {
        completeHardResetBLEClient();
        hardResetPending_ = false;
    }

    nextConnectAllowedMs_ = 0;
    setBLEState(BLEState::DISCONNECTED, "client quiesced");
}

// --- discovery ---

// Static trampoline for async discovery task
void V1BLEClient::discoveryTaskFunc(void* param) {
    const DiscoveryTaskContext context = *static_cast<DiscoveryTaskContext*>(param);
    V1BLEClient* self = context.owner;
    const bool result = context.client && context.client->discoverAttributes();

    self->discoveryTaskResult_.store(result, std::memory_order_relaxed);
    self->discoveryCompletedGeneration_.store(context.generation, std::memory_order_relaxed);
    self->discoveryTaskDone_.store(true, std::memory_order_release);
    // Release is the ownership handoff: after observing false with acquire,
    // the main loop may reuse the context and operate on the client again.
    self->discoveryTaskRunning_.store(false, std::memory_order_release);
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
        connectInProgress_ = false;
        connectStartMs_ = 0;
        beginClientQuiesce("discovery timeout");
        return;
    }

    // Spawn discovery task on first entry
    // Guard: wait for any prior discovery task to finish (e.g. after timeout/disconnect)
    if (discoveryTaskRunning_.load()) {
        return; // Previous task still winding down — yield
    }
    if (!discoveryTaskDone_.load()) {
        discoveryTaskContext_.owner = this;
        discoveryTaskContext_.client = pClient_;
        discoveryTaskContext_.generation = activeDiscoveryGeneration_;
        discoveryTaskRunning_.store(true, std::memory_order_release);
        BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(discoveryTaskFunc, "disc", 4096, &discoveryTaskContext_, 1,
                                                        nullptr, tskNO_AFFINITY, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (rc != pdPASS) {
            // Discovery remains off the main loop; back off and retry.
            static BleLogRateLimitState discoveryTaskCreateFailedLog;
            if (shouldLogBleConnectionEvent(discoveryTaskCreateFailedLog, now)) {
                Serial.println("[BLE] disc task create failed - backing off");
            }
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
            beginClientQuiesce("discovery task create failed");
        }
        return; // Yield to loop while task runs
    }


    const uint32_t completedGeneration = discoveryCompletedGeneration_.load(std::memory_order_acquire);
    if (completedGeneration != activeDiscoveryGeneration_ ||
        completedGeneration != sessionGeneration_.load(std::memory_order_acquire)) {
        beginClientQuiesce("stale discovery completion");
        return;
    }

    if (!discoveryTaskResult_.load(std::memory_order_relaxed)) {
        static BleLogRateLimitState discoveryFailedLog;
        if (shouldLogBleConnectionEvent(discoveryFailedLog, now)) {
            Serial.println("[BLE] FAIL discovery");
        }
        connectInProgress_ = false;
        connectStartMs_ = 0;
        beginClientQuiesce("discovery failed");
        return;
    }

    // Transition to subscribe phase (uses step machine to break up CCCD writes)
    subscribeStep_ = SubscribeStep::GET_SERVICE;
    setBLEState(BLEState::SUBSCRIBING, "discovery complete");
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
        Serial.printf("[BLE] Conn params (%s): interval=%.2f ms latency=%u\n", tag ? tag : "n/a", intervalMs,
                      info.getConnLatency());
    }
}

// --- notify callback ---

// Subscription setup now runs through the non-blocking executeSubscribeStep()
// state machine instead of a single monolithic helper.
void V1BLEClient::notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    if (!pData || !instancePtr || !pChar) {
        return;
    }
    const uint32_t callbackMillis = static_cast<uint32_t>(millis());
    const uint32_t callbackGeneration = instancePtr->sessionGeneration_.load(std::memory_order_acquire);
    const uint32_t proxyQueueEpoch = instancePtr->proxyQueueEpoch_.load(std::memory_order_acquire);
    BleProxyEpochObserver::CallbackLease callbackLease(instancePtr->proxyEpochObserver_,
                                                       BleProxyCallbackDirection::V1ToProxy, proxyQueueEpoch);
    if (!instancePtr->acceptClientCallbacks_.load(std::memory_order_acquire) ||
        !instancePtr->sessionPublicationGate_.accepts(callbackGeneration)) {
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
        // A mapping can disappear after the acceptance check when quiescence
        // begins. Drop that stale callback instead of dereferencing the retiring
        // remote object or mislabeling a late B4E0 packet as B2CE.
        return;
    }

    // Relay every notification on its observed source characteristic. Packet
    // IDs 0x41/0x42 are request IDs, while response 0x43 already arrives on
    // B4E0, so packet-ID-based rerouting would misrepresent the source.
    // Forward to proxy via the proxy queue only. Keep BLE callback path notify-free.
    instancePtr->forwardToProxyForEpoch(pData, length, charId, proxyQueueEpoch);

    if (instancePtr->connected_.load(std::memory_order_relaxed) &&
        instancePtr->firstRxAfterConnectMs_.load(std::memory_order_relaxed) == 0) {
        instancePtr->firstRxAfterConnectMs_.store(callbackMillis, std::memory_order_relaxed);
    }

    // Call user callback for display processing (queued to main loop for SPI safety)
    if (instancePtr->dataCallback_ && instancePtr->acceptClientCallbacks_.load(std::memory_order_acquire) &&
        instancePtr->sessionGeneration_.load(std::memory_order_acquire) == callbackGeneration &&
        instancePtr->sessionPublicationGate_.accepts(callbackGeneration)) {
        instancePtr->dataCallback_(pData, length, charId, callbackGeneration, callbackMillis);
    }
}
