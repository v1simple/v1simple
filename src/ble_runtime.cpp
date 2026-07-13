/**
 * BLE runtime state machine and scan/priority control.
 * Extracted from ble_client.cpp for modularity.
 */

#include "ble_client.h"

#include <cstring>
#include <string>
#include <WiFi.h>
#include "settings.h"
#include "perf_metrics.h"
#include "ble_log_rate_limit.h"
#include "config.h"
#include "ble_internals.h"

void V1BLEClient::process() {
    // Handle deferred BLE callback updates without blocking in callbacks
    if (pendingConnectStateUpdate_) {
        SemaphoreGuard lock(bleMutex_, 0);
        if (lock.locked()) {
            const uint32_t edgeGeneration =
                pendingConnectStateGeneration_.load(std::memory_order_relaxed);
            pendingConnectStateUpdate_.store(false, std::memory_order_relaxed);
            pendingConnectStateGeneration_.store(0, std::memory_order_relaxed);
            const bool edgeStillAccepted =
                edgeGeneration == sessionGeneration_.load(std::memory_order_acquire) &&
                acceptClientCallbacks_.load(std::memory_order_acquire) &&
                sessionPublicationGate_.accepts(edgeGeneration);
            connected_.store(edgeStillAccepted, std::memory_order_release);
            // Don't set CONNECTED state here - async state machine handles transitions
            // Just set the connected_ flag; state machine will transition via asyncConnectSuccess_
        }
    }
    if (pendingDisconnectCleanup_) {
        SemaphoreGuard lock(bleMutex_, 0);
        if (lock.locked()) {
            pendingDisconnectCleanup_ = false;
            const bool disconnectWasExpected = bleState_ == BLEState::QUIESCING;
            connected_.store(false, std::memory_order_relaxed);
            connectInProgress_ = false;
            connectStartMs_ = 0;
            connectedFollowupStep_ = ConnectedFollowupStep::NONE;
            connectCompletedAtMs_.store(0, std::memory_order_relaxed);
            firstRxAfterConnectMs_.store(0, std::memory_order_relaxed);
            lastBleProcessDurationUs_.store(0, std::memory_order_relaxed);
            lastDisplayPipelineDurationUs_.store(0, std::memory_order_relaxed);
            connectBurstStableLoopCount_ = 0;
            proxyClientConnected_ = false;
            proxyDisconnectRequestedByCoordinator_ = false;
            verifyPending_ = false;
            verifyComplete_ = false;
            verifyMatch_ = false;
            verifyPushMatchEdgePending_.store(false, std::memory_order_relaxed);
            // A spontaneous V1 power-off starts a fresh reconnect cycle. A
            // callback caused by our own failure teardown must retain the
            // failure count so the hard-reset threshold still works.
            if (!disconnectWasExpected) {
                consecutiveConnectFailures_ = 0;
            }
            nextConnectAllowedMs_ = 0;
            shouldConnect_ = false;
            hasTargetDevice_ = false;
            beginClientQuiesce("deferred onDisconnect");
        }
    }
    // Deferred bond deletion (NVS write moved out of BLE callback)
    if (pendingDeleteBond_) {
        // Copy the address under the writer's guard so a second disconnect on
        // the NimBLE host task can't rewrite it mid-read.
        portENTER_CRITICAL(&pendingAddrMux);
        pendingDeleteBond_ = false;
        const NimBLEAddress addrToDelete = pendingDeleteBondAddr_;
        portEXIT_CRITICAL(&pendingAddrMux);
        if (NimBLEDevice::isBonded(addrToDelete)) {
            NimBLEDevice::deleteBond(addrToDelete);
        }
    }
    if (pendingScanEndUpdate_) {
        SemaphoreGuard lock(bleMutex_, 0);
        if (lock.locked()) {
            pendingScanEndUpdate_ = false;
            if (bleState_ == BLEState::SCANNING &&
                !pendingScanTargetUpdate_.load(std::memory_order_acquire)) {
                setBLEState(BLEState::DISCONNECTED, "scan ended without finding V1 (deferred)");
            }
        }
    }

    drainProxyCallbackEvents();

    if (pendingScanTargetUpdate_) {
        SemaphoreGuard lock(bleMutex_, 0);
        if (lock.locked()) {
            char addrCopy[sizeof(pendingScanTargetAddress_)] = {0};
            uint8_t addrTypeCopy = BLE_ADDR_PUBLIC;
            bool havePending = false;
            portENTER_CRITICAL(&pendingAddrMux);
            if (pendingScanTargetUpdate_) {
                pendingScanTargetUpdate_ = false;
                memcpy(addrCopy, pendingScanTargetAddress_, sizeof(pendingScanTargetAddress_));
                addrCopy[sizeof(addrCopy) - 1] = '\0';
                addrTypeCopy = pendingScanTargetAddressType_;
                havePending = true;
            }
            portEXIT_CRITICAL(&pendingAddrMux);
            if (havePending && bleState_ == BLEState::SCANNING) {
                targetAddress_ = NimBLEAddress(std::string(addrCopy), addrTypeCopy);
                targetAddressType_ = addrTypeCopy;
                hasTargetDevice_ = true;
                shouldConnect_ = true;
                scanStopRequestedMs_ = static_cast<uint32_t>(millis());
                setBLEState(BLEState::SCAN_STOPPING, "V1 found (deferred)");
            }
        }
    }
    if (pendingLastV1AddressValid_) {
        char addrCopy[sizeof(pendingLastV1Address_)] = {0};
        char nameCopy[sizeof(pendingLastV1Name_)] = {0};
        bool shouldWrite = false;
        bool shouldAdoptName = false;
        portENTER_CRITICAL(&pendingAddrMux);
        if (pendingLastV1AddressValid_) {
            pendingLastV1AddressValid_ = false;
            memcpy(addrCopy, pendingLastV1Address_, sizeof(pendingLastV1Address_));
            addrCopy[sizeof(addrCopy) - 1] = '\0';
            shouldWrite = true;
        }
        if (pendingLastV1NameValid_) {
            pendingLastV1NameValid_ = false;
            memcpy(nameCopy, pendingLastV1Name_, sizeof(pendingLastV1Name_));
            nameCopy[sizeof(nameCopy) - 1] = '\0';
            shouldAdoptName = true;
        }
        portEXIT_CRITICAL(&pendingAddrMux);
        if (shouldWrite) {
            settingsManager.setLastV1Address(addrCopy);
        }
        if (shouldAdoptName) {
            adoptV1AdvertisedNameForProxy(nameCopy);
        }
    }
    // Process phone->V1 commands (up to queue size per loop to drain any backlog)
    // Each call processes one command to minimize mutex hold time during BLE writes
    for (int i = 0; i < MAX_PHONE_CMDS_PER_LOOP; i++) {
        if (processPhoneCommandQueue() == 0) {
            break;
        }
    }
    if (connectedFollowupStep_ != ConnectedFollowupStep::NONE && isConnected()) {
        processConnectedFollowup();
    }

    const uint32_t now = static_cast<uint32_t>(millis());
    const bool holdProxyForAutoObd =
        obdBleArbitrationRequest_ == ObdBleArbitrationRequest::HOLD_PROXY_FOR_AUTO_OBD;
    const bool preemptProxyForManualScan =
        obdBleArbitrationRequest_ == ObdBleArbitrationRequest::PREEMPT_PROXY_FOR_MANUAL_SCAN;
    const bool suppressPassiveProxy = holdProxyForAutoObd || preemptProxyForManualScan;
    const bool proxyConnected =
        proxyClientConnected_.load(std::memory_order_relaxed) ||
        (pServer_ && pServer_->getConnectedCount() > 0);
    NimBLEAdvertising* pProxyAdvertising =
        (proxyEnabled_ && proxyServerInitialized_) ? NimBLEDevice::getAdvertising() : nullptr;
    const bool proxyAdvertisingActive = pProxyAdvertising && pProxyAdvertising->isAdvertising();
    const bool proxyAdvertisingAllowed =
        proxyAdvertisingAllowed_ &&
        isConnected() &&
        proxyEnabled_ &&
        proxyServerInitialized_ &&
        !wifiPriorityMode_ &&
        !suppressPassiveProxy;
    const bool proxyKeepConnectionAllowed =
        proxyKeepConnectionAllowed_ && !preemptProxyForManualScan;

    if (!proxyKeepConnectionAllowed &&
        proxyConnected &&
        !proxyDisconnectRequestedByCoordinator_ &&
        pServer_ &&
        pServer_->getConnectedCount() > 0) {
        proxyDisconnectRequestedByCoordinator_ = true;
        proxySuppressedForObdHold_ = true;
        if (proxySuppressedResumeReasonCode_ ==
            static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown)) {
            proxySuppressedResumeReasonCode_ =
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartRetryWindow);
        }
        for (uint16_t h : pServer_->getPeerDevices()) {
            pServer_->disconnect(h);
        }
    } else if (proxyKeepConnectionAllowed) {
        proxyDisconnectRequestedByCoordinator_ = false;
    }

    if (!proxyAdvertisingAllowed) {
        clearProxyAdvertisingSchedule();
        if (!proxyConnected && proxyAdvertisingActive) {
            proxySuppressedForObdHold_ = true;
            if (proxySuppressedResumeReasonCode_ ==
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown)) {
                proxySuppressedResumeReasonCode_ =
                    static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartRetryWindow);
            }
            stopProxyAdvertisingFromMainLoop(
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopOther));
        }
    } else if (!proxyConnected && !proxyAdvertisingActive && proxyAdvertisingStartMs_ == 0) {
        proxyAdvertisingStartMs_ = now + PROXY_STABILIZE_MS;
        proxyAdvertisingStartReasonCode_ =
            proxySuppressedResumeReasonCode_ == 0
                ? static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartRetryWindow)
                : proxySuppressedResumeReasonCode_;
        proxySuppressedForObdHold_ = false;
        proxySuppressedResumeReasonCode_ =
            static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown);
        Serial.printf("[BLE] Proxy advertising scheduled (%lums, cycle=%s cycleMs=%lu)\n",
                      static_cast<unsigned long>(PROXY_STABILIZE_MS),
                      perfConnectionCycleStateName(connectionCycleStateCode_),
                      static_cast<unsigned long>(connectionCycleTimeInStateMs_));
    }

    if (proxyAdvertisingAllowed && !proxyConnected && proxyAdvertisingActive) {
        refreshProxyAdvertisingCadence(
            now,
            static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartRetryWindow));
    }

    // Handle deferred proxy advertising start (non-blocking replacement for delay(1500))
    if (proxyAdvertisingStartMs_ != 0) {
        if (static_cast<int32_t>(now - proxyAdvertisingStartMs_) >= 0) {
            if (!proxyAdvertisingAllowed) {
                proxySuppressedForObdHold_ = true;
                if (proxySuppressedResumeReasonCode_ ==
                    static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown)) {
                    proxySuppressedResumeReasonCode_ = proxyAdvertisingStartReasonCode_;
                }
            } else if (isConnected() && proxyEnabled_ && proxyServerInitialized_) {
                const uint8_t startReason = proxyAdvertisingStartReasonCode_;
                proxyAdvertisingStartMs_ = 0;  // Clear pending flag
                proxyAdvertisingStartReasonCode_ =
                    static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown);
                // Advertising data already configured in initProxyServer() with proper flags
                startProxyAdvertising(startReason);
            } else {
                proxyAdvertisingStartMs_ = 0;
                proxyAdvertisingStartReasonCode_ =
                    static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown);
            }
        }
    }

    NimBLEScan* pScan = NimBLEDevice::getScan();

    // Boot readiness gate: keep state machine idle until setup opens the gate.
    if (!bootReadyFlag_) {
        return;
    }

    // ============================================================================
    // BLE STATE MACHINE
    // ============================================================================
    switch (bleState_) {
        case BLEState::DISCONNECTED: {
            if (discoveryTaskRunning_.load(std::memory_order_acquire)) {
                beginClientQuiesce("discovery owner still active");
                break;
            }

            // Skip scanning if WiFi priority mode is active
            if (wifiPriorityMode_) {
                return;
            }

            // Not connected_ - keep actively scanning for the V1.
            if (!pScan->isScanning() && (now - lastScanStart_ >= RECONNECT_DELAY)) {
                lastScanStart_ = now;
                pScan->clearResults();
                // Re-assert scan-callback ownership: another subsystem (e.g. the
                // OBD pair scan) may have swapped the shared scan object's
                // callbacks; without this the V1 could never be rediscovered.
                if (pScanCallbacks_) {
                    pScan->setScanCallbacks(pScanCallbacks_.get());
                }
                bool started = pScan->start(SCAN_DURATION, false, false);
                if (started) {
                    setBLEState(BLEState::SCANNING, "scan started");
                }
            }
            break;
        }

        case BLEState::SCANNING: {
            // Check if scan found a device (shouldConnect_ flag set by callback)
            bool wantConnect = false;
            {
                // HOT PATH: try-lock only, skip if busy
                SemaphoreGuard lock(bleMutex_, 0);
                if (lock.locked()) {
                    wantConnect = shouldConnect_;
                }
            }

            if (wantConnect) {
                // V1 found - stop scan and transition to SCAN_STOPPING
                if (pScan->isScanning()) {
                    pScan->stop();
                    scanStopRequestedMs_ = now;
                    setBLEState(BLEState::SCAN_STOPPING, "V1 found during scan");
                } else {
                    // Scan already stopped, proceed directly
                    scanStopRequestedMs_ = now;
                    setBLEState(BLEState::SCAN_STOPPING, "scan already stopped");
                }
            }
            // Note: Scan ending without finding device will restart via scan callbacks
            // Watchdog: if the scan ended but our onScanEnd never advanced the
            // state machine (missed or foreign scan-callback edge), force
            // recovery instead of wedging in SCANNING forever.
            if (!wantConnect && !pScan->isScanning() &&
                (now - lastScanStart_) > (SCAN_DURATION + 5000u)) {  // scan length + settle margin
                setBLEState(BLEState::DISCONNECTED, "scan watchdog");
            }
            break;
        }

        case BLEState::SCAN_STOPPING: {
            // Wait for scan to fully stop and radio to settle
            const uint32_t elapsed = now - scanStopRequestedMs_;

            // Ensure scan is actually stopped
            if (pScan->isScanning()) {
                if (elapsed > 1000) {  // Force stop if taking too long
                    pScan->stop();
                }
                return;  // Wait more
            }

            // Clear scan results once scan has stopped
            if (!scanStopResultsCleared_ && elapsed > 100) {  // Clear after brief delay
                pScan->clearResults();
                scanStopResultsCleared_ = true;
            }

            // Check if settle time has elapsed
            // Use longer settle on first scan after boot (radio is "cold")
            const uint32_t settleTime =
                firstScanAfterBoot_ ? SCAN_STOP_SETTLE_FRESH_MS : SCAN_STOP_SETTLE_MS;
            if (elapsed >= settleTime) {
                if (firstScanAfterBoot_) {
                    Serial.println("[BLE] First scan settle complete (extended)");
                    firstScanAfterBoot_ = false;
                }
                // Ready to connect
                bool wantConnect = false;
                {
                    // HOT PATH: try-lock only, skip if busy
                    SemaphoreGuard lock(bleMutex_, 0);
                    if (lock.locked()) {
                        wantConnect = shouldConnect_;
                        shouldConnect_ = false;  // Clear flag
                    }
                }

                if (wantConnect) {
                    connectToServer();  // This will set state to CONNECTING
                } else {
                    setBLEState(BLEState::DISCONNECTED, "no connect pending");
                }
            }
            break;
        }

        case BLEState::CONNECTING: {
            if (nextConnectAllowedMs_ != 0 && static_cast<int32_t>(now - nextConnectAllowedMs_) < 0) {
                break;
            }

            // Initiate at most one async connect attempt per loop iteration.
            // startAsyncConnect() transitions to CONNECTING_WAIT on successful initiation.
            if (!asyncConnectPending_ && !asyncConnectSuccess_) {
                startAsyncConnect();
                if (bleState_ != BLEState::CONNECTING) {
                    break;
                }
            }

            // If we're stuck here for too long, something is wrong.
            if (connectStartMs_ > 0 && (now - connectStartMs_) > 5000) {
                Serial.println("[BLE] Connect initiation stuck for 5s - resetting");
                connectInProgress_ = false;
                connectStartMs_ = 0;
                beginClientQuiesce("connect initiation timeout");
            }
            break;
        }

        case BLEState::CONNECTING_WAIT: {
            // Waiting for async connect callback
            processConnectingWait();
            break;
        }

        case BLEState::DISCOVERING: {
            // Performing service discovery
            processDiscovering();
            break;
        }

        case BLEState::SUBSCRIBING: {
            // Subscribing to characteristics (step machine)
            processSubscribing();
            break;
        }

        case BLEState::SUBSCRIBE_YIELD: {
            // Brief yield between subscribe steps to let loop() run
            processSubscribeYield();
            break;
        }

        case BLEState::CONNECTED: {
            // All good - nothing to do in state machine
            // Verify we're actually still connected_
            if (!pClient_ || !pClient_->isConnected()) {
                connected_.store(false, std::memory_order_relaxed);
                connectInProgress_ = false;
                beginClientQuiesce("connection lost");
            }
            break;
        }

        case BLEState::BACKOFF: {
            // Legacy state: immediately resume the normal disconnected scan flow.
            setBLEState(BLEState::DISCONNECTED, "backoff retired");
            break;
        }

        case BLEState::QUIESCING: {
            processClientQuiesce();
            break;
        }
    }
}

void V1BLEClient::startScanning() {
    if (!isConnected() && bleState_ == BLEState::DISCONNECTED &&
        !discoveryTaskRunning_.load(std::memory_order_acquire)) {
        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (!pScan->isScanning()) {
            lastScanStart_ = static_cast<uint32_t>(millis());
            pScan->clearResults();
            // Re-assert scan-callback ownership (see state-machine scan start).
            if (pScanCallbacks_) {
                pScan->setScanCallbacks(pScanCallbacks_.get());
            }
            bool started = pScan->start(SCAN_DURATION, false, false);
            if (started) {
                setBLEState(BLEState::SCANNING, "manual scan start");
            }
        }
    }
}

bool V1BLEClient::isScanning() {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    return pScan && pScan->isScanning();
}

NimBLEAddress V1BLEClient::getConnectedAddress() const {
    if (pClient_ && pClient_->isConnected()) {
        return pClient_->getPeerAddress();
    }
    return NimBLEAddress();  // Default constructor for empty address
}

void V1BLEClient::disconnect() {
    beginClientQuiesce("disconnect requested");
}

// ============================================================================
// WiFi Priority Mode
// ============================================================================
// Deprioritize BLE when web UI is active to maximize responsiveness

void V1BLEClient::setBootReady(bool ready) {
    bootReadyFlag_ = ready;
}

void V1BLEClient::setWifiPriority(bool enabled) {
    if (wifiPriorityMode_ == enabled) return;  // No change

    wifiPriorityMode_ = enabled;

    // Rate-limit transition logs to avoid serial spam if caller oscillates.
    static BleLogRateLimitState wifiPriorityLog;
    const uint32_t nowMs = static_cast<uint32_t>(millis());
    const bool shouldLog = shouldLogBleConnectionEvent(wifiPriorityLog, nowMs);

    if (enabled) {
        if (shouldLog) Serial.println("[BLE] WiFi priority ENABLED - suppressing scans/reconnects/proxy");

        // Stop any active scan
        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (pScan && pScan->isScanning()) {
            if (shouldLog) Serial.println("[BLE] Stopping scan for WiFi priority mode");
            pScan->stop();
            pScan->clearResults();
        }

        // Stop proxy advertising if running
        if (proxyEnabled_ && NimBLEDevice::getAdvertising()->isAdvertising()) {
            if (shouldLog) Serial.println("[BLE] Stopping proxy advertising for WiFi priority mode");
            NimBLEDevice::stopAdvertising();
            perfRecordProxyAdvertisingTransition(
                false,
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopWifiPriority),
                nowMs);
        }

        // Cancel any pending deferred advertising start
        proxyAdvertisingStartMs_ = 0;
        proxyAdvertisingStartReasonCode_ =
            static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown);

        // Note: We keep existing V1 connection if already connected_
        // to avoid disrupting active radar detection

    } else {
        if (shouldLog) Serial.println("[BLE] WiFi priority DISABLED - resuming normal BLE operation");

        // Resume scanning if disconnected
        if (!isConnected() && bleState_ == BLEState::DISCONNECTED) {
            if (shouldLog) Serial.println("[BLE] Resuming scan after WiFi priority mode");
            startScanning();
        }
    }
}
