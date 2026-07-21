#include "obd_runtime_module.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#ifndef UNIT_TEST
extern "C" {
#include "nimble/nimble/host/include/host/ble_att.h"
#include "nimble/nimble/host/include/host/ble_hs.h"
#include "nimble/nimble/include/nimble/ble.h"
}
#endif

#include "../../psram_freertos_alloc.h"
#include "obd_elm327_parser.h"
#include "obd_scan_policy.h"
#include "obd_string_utils.h"
#include "perf_metrics.h"

#ifndef UNIT_TEST
#include "ble_client.h"
#include "obd_ble_client.h"

// Forward declaration — defined in obd_runtime_transport.cpp (separate TU in production)
class ObdRuntimeModule;
class ObdBleClient;
bool ensureObdTransportRuntime(ObdBleClient* bleClient, ObdRuntimeModule* runtime);
#endif

// ======================================================================
// FILE-SCOPE HELPERS — string utilities and enum naming
// ======================================================================

using ObdStringUtils::commandDisplayLen;
using ObdStringUtils::copyString;
using ObdStringUtils::stringContainsCI;

namespace {
#ifndef OBD_RSSI_REFRESH_MS_DEFINED
#define OBD_RSSI_REFRESH_MS_DEFINED
constexpr uint32_t OBD_RSSI_REFRESH_MS = 2000;
#endif

} // namespace

// ======================================================================
// SINGLETON — file-scope module instance
// ======================================================================

ObdRuntimeModule obdRuntimeModule;

// ======================================================================
// LIFECYCLE — resetForBegin / begin constructor and reset sequence
// ======================================================================

void ObdRuntimeModule::resetForBegin() {
    state_ = ObdConnectionState::IDLE;
    stateEnteredMs_ = 0;
    bootReadyMs_ = 0;
    stateEntryPending_ = false;
    connectionCycleStateCode_ = 0;
    connectionCycleTimeInStateMs_ = 0;
    lastProxyAdvertising_ = false;
    lastProxyClientConnected_ = false;
    lastObdRetryAllowed_ = true;
    minRssi_ = obd::DEFAULT_MIN_RSSI;
    rssi_ = 0;
    pendingRssi_ = 0;
    pendingAddrType_ = 0;
    savedAddrType_ = 0;
    connectAddrType_ = 0;
    manualCandidateAddrType_ = 0;
    pendingDeviceFound_ = false;
    scanRequested_ = false;
    manualScanPending_ = false;
    manualScanPreemptProxy_ = false;
    manualCandidateValid_ = false;
    connectTargetFromManualCandidate_ = false;
    preferWarmReconnect_ = false;
    warmInitPreferred_ = false;
    coldInitFallbackUsed_ = false;
    preferWriteWithResponse_ = false;

    savedAddress_[0] = '\0';
    connectAddress_[0] = '\0';
    manualCandidateAddress_[0] = '\0';
    pendingAddress_[0] = '\0';

    connectAttempts_ = 0;
    connectSuccesses_ = 0;
    connectFailures_ = 0;
    pollCount_ = 0;
    pollErrors_ = 0;
    staleSpeedCount_ = 0;
    consecutiveErrors_ = 0;
    backoffCycles_ = 0;
    v1WasConnectedAtEcuIdle_ = false;
    totalBytesReceived_ = 0;
    lastRssiMs_ = 0;
    bufferOverflowCount_ = 0;
    initRetries_ = 0;
    consecutiveSpeedSamples_ = 0;
    securityRepairs_ = 0;
    lastConnectStartMs_ = 0;
    lastConnectSuccessMs_ = 0;
    lastFailureMs_ = 0;
    lastFailure_ = ObdFailureReason::NONE;
    bleDisconnectReason_ = 0;
    repairedBondAddress_[0] = '\0';

    clearBleEventQueue();
    clearBleResponseState();
    bleDisconnected_ = false;
    resetCommandState();
    nextTransportRequestId_ = 0;
    clearTransportRequest();
    readyTransportResult_ = {};
    transportDisconnectPending_ = false;
    transportDisconnectQueued_ = false;
    pendingDisconnectDeleteBond_ = false;
    pendingDisconnectFollowupDeleteBond_ = false;
    lastDisconnectSucceeded_ = false;
    pendingDisconnectRequestId_ = 0;
    pendingDisconnectAddress_[0] = '\0';
    pendingDisconnectAddrType_ = 0;
    clearSpeedState();
    resetPollingSchedule(0);
    initIndex_ = 0;

#ifdef UNIT_TEST
    testStartScanResult_ = true;
    testConnectResult_ = true;
    testBleConnected_ = false;
    testDiscoverResult_ = true;
    testSubscribeResult_ = true;
    testWriteResult_ = true;
    testBeginSecurityResult_ = true;
    testSecurityReady_ = true;
    testSecurityEncrypted_ = true;
    testSecurityBonded_ = true;
    testSecurityAuthenticated_ = true;
    testRssi_ = 0;
    testLastBleError_ = 0;
    testLastSecurityError_ = 0;
    testStartScanCalls_ = 0;
    testConnectCalls_ = 0;
    testDiscoverCalls_ = 0;
    testDisconnectCalls_ = 0;
    testWriteCalls_ = 0;
    testBeginSecurityCalls_ = 0;
    testDeleteBondCalls_ = 0;
    testRefreshBondBackupCalls_ = 0;
    testLastCommand_[0] = '\0';
    testLastWriteWithResponse_ = true;
#endif
}

void ObdRuntimeModule::begin(ObdBleClient* bleClient, bool enabled, const char* savedAddress, uint8_t savedAddrType,
                             int8_t minRssi) {
    bleClient_ = bleClient;
    enabled_ = enabled;
    resetForBegin();
    setMinRssi(minRssi);

    setSavedAddressFromBuffer(savedAddress);
    savedAddrType_ = savedAddrType;

#ifndef UNIT_TEST
    if (bleClient_) {
        bleClient_->init(this);
    }
    (void)ensureObdTransportRuntime(bleClient_, this);
#endif

    if (!enabled_) {
        return;
    }

    if (savedAddress_[0] != '\0') {
        state_ = ObdConnectionState::WAIT_BOOT;
    }
}

void ObdRuntimeModule::clearBleEventQueue() {
    taskENTER_CRITICAL(&bleEventQueueMux_);
    bleEventQueueHead_ = 0;
    bleEventQueueCount_ = 0;
    taskEXIT_CRITICAL(&bleEventQueueMux_);
}

// ======================================================================
// RESPONSE + SPEED STATE RESET — clear BLE buffer and speed on transition
// ======================================================================

void ObdRuntimeModule::resetInitState(bool preferWarmInit) {
    initIndex_ = 0;
    warmInitPreferred_ = preferWarmInit;
    coldInitFallbackUsed_ = false;
    initRetries_ = 0;
    resetCommandState();
    clearBleResponseState();
}

// ======================================================================
// RUNTIME TOGGLES — setEnabled / setMinRssi runtime configuration
// ======================================================================

void ObdRuntimeModule::setEnabled(bool enabled) {
    if (enabled_ == enabled)
        return;
    enabled_ = enabled;

    if (!enabled_) {
        stopBleScan();
        disconnectBle();
        clearBleEventQueue();
        scanRequested_ = false;
        clearManualScanState();
        pendingDeviceFound_ = false;
        pendingAddress_[0] = '\0';
        clearBleResponseState();
        resetCommandState();
        clearTransportRequest();
        readyTransportResult_ = {};
        bleDisconnected_ = false;
        clearSpeedState();
        state_ = ObdConnectionState::IDLE;
        stateEnteredMs_ = 0;
        stateEntryPending_ = false;
        return;
    }

    clearSpeedState();
    connectAttempts_ = 0;
    resetPollingSchedule(0);
    clearBleEventQueue();
    clearBleResponseState();
    resetCommandState();
    bleDisconnected_ = false;
    clearManualScanState();
    // Re-enabling restores the runtime to its boot admission state only.
    // Coordinator-owned auto OBD admission still waits for the next V1 cycle
    // or an explicit manual scan rather than forcing a mid-cycle reopen here.
    state_ = (savedAddress_[0] != '\0') ? ObdConnectionState::WAIT_BOOT : ObdConnectionState::IDLE;
    stateEnteredMs_ = 0;
    stateEntryPending_ = false;
}

void ObdRuntimeModule::setMinRssi(int8_t minRssi) {
    if (minRssi < -90)
        minRssi = -90;
    if (minRssi > -40)
        minRssi = -40;
    minRssi_ = minRssi;
}

void ObdRuntimeModule::handlePollingResponse(uint32_t nowMs) {
    if (!activeCommand_.active) {
        return;
    }

    const ObdCommandKind kind = activeCommand_.kind;
    bool handled = false;

    if (bleOverflowed_) {
        bufferOverflowCount_++;
        handled = false;
    } else {
        switch (kind) {
        case ObdCommandKind::SPEED:
            handled = handleSpeedResponse(nowMs);
            break;
        default:
            handled = false;
            break;
        }
    }

    totalBytesReceived_ += bleBufLen_;
    clearBleResponseState();
    const bool completedWriteWithResponse = activeCommand_.writeWithResponse;
    completeActiveCommand();

    if (handled) {
        preferWriteWithResponse_ = completedWriteWithResponse;
        return;
    }

    if (bufferOverflowCount_ >= obd::BUFFER_OVERFLOWS_BEFORE_DISCONNECT) {
        handlePollingError(nowMs, false, ObdFailureReason::BUFFER_OVERFLOW);
    } else {
        handlePollingError(nowMs, false,
                           bleOverflowed_ ? ObdFailureReason::BUFFER_OVERFLOW : ObdFailureReason::COMMAND_RESPONSE);
    }
}

// ======================================================================
// MAIN UPDATE DISPATCHER — FSM pump delegating to per-state handlers
// ======================================================================

void ObdRuntimeModule::update(uint32_t nowMs, const ObdBleContext& bootReadyContext) {
    // Disconnect acknowledgements must drain even while OBD is disabled;
    // otherwise re-enabling could inherit a permanently closed transport gate.
    pumpTransportResults();
    if (!enabled_) {
        // Qualification/proxy teardown can disable the runtime immediately
        // after cancellation. Reject a connection that surfaces afterward;
        // disabled IDLE/DISCONNECTED states cannot own a physical link.
        if (!transportDisconnectPending_ &&
            (state_ == ObdConnectionState::IDLE || state_ == ObdConnectionState::DISCONNECTED) && isBleConnected()) {
            disconnectBle();
        }
        return;
    }

    drainBleEventQueue();

    if (pendingTransportTimedOut(nowMs)) {
        pendingTransportTimedOut_ = true;
    }
    const bool bootReady = bootReadyContext.bootReady;
    const bool v1Connected = bootReadyContext.v1Connected;
    const bool bleScanIdle = bootReadyContext.bleScanIdle;
    const bool v1ConnectBurstSettling = bootReadyContext.v1ConnectBurstSettling;
    const bool proxyAdvertising = bootReadyContext.proxyAdvertising;
    const bool proxyClientConnected = bootReadyContext.proxyClientConnected;
    const bool v1ConnectInProgress = bootReadyContext.v1ConnectInProgress;
    const bool obdScanAllowed = bootReadyContext.obdScanAllowed;
    const bool obdConnectAllowed = bootReadyContext.obdConnectAllowed;
    const bool obdRetryAllowed = bootReadyContext.obdRetryAllowed;
    connectionCycleStateCode_ = bootReadyContext.connectionCycleStateCode;
    connectionCycleTimeInStateMs_ = bootReadyContext.connectionCycleTimeInStateMs;
    lastProxyAdvertising_ = proxyAdvertising;
    lastProxyClientConnected_ = proxyClientConnected;
    lastObdRetryAllowed_ = obdRetryAllowed;

    if (bootReady && bootReadyMs_ == 0) {
        bootReadyMs_ = nowMs == 0 ? 1 : nowMs;
    }

    // The priority disconnect is a transport fence. Do not start a scan,
    // reconnect, or another GATT operation until its acknowledgement drains.
    if (transportDisconnectPending_) {
        return;
    }

    const bool justEntered = stateEntryPending_;
    stateEntryPending_ = false;

    if (proxyClientConnected) {
        // Proxy/app mode is the authority: the phone app owns low-speed
        // muting policy, and the BLE radio must not carry V1 + proxy +
        // OBD simultaneously. Drop any OBD scan/connect/poll work immediately
        // and stay idle until proxy disconnects.
        stopBleScan();
        scanRequested_ = false;
        pendingDeviceFound_ = false;
        pendingAddress_[0] = '\0';
        if (manualScanPending_) {
            clearManualScanState();
        }
        if (isBleConnected() || state_ != ObdConnectionState::IDLE) {
            disconnectBle();
            clearBleEventQueue();
            clearBleResponseState();
            resetCommandState();
            clearTransportRequest();
            readyTransportResult_ = {};
            bleDisconnected_ = false;
            clearSpeedState();
            transitionTo(ObdConnectionState::IDLE, nowMs);
        }
        return;
    }

    switch (state_) {
    case ObdConnectionState::IDLE:
        // Cancellation can win the state-machine race while an in-flight
        // transport connect still becomes established. IDLE has no session
        // that can own such a link, so reconcile it before admitting scans.
        if (isBleConnected()) {
            disconnectBle();
            break;
        }
        if (manualScanPreemptProxy_ && (proxyAdvertising || proxyClientConnected)) {
            break;
        }
        if (scanRequested_ && (manualScanPending_ || obdScanAllowed) && bleScanIdle && !v1ConnectInProgress) {
            if (startBleScan()) {
                scanRequested_ = false;
                transitionTo(ObdConnectionState::SCANNING, nowMs);
            }
        }
        break;

    case ObdConnectionState::WAIT_BOOT: {
        if (!bootReady) {
            break;
        }
        if (!obdConnectAllowed) {
            break;
        }

        setConnectTargetFromSaved();
        transitionTo(ObdConnectionState::CONNECTING, nowMs);
        break;
    }

    case ObdConnectionState::SCANNING: {
        if (pendingDeviceFound_) {
            pendingDeviceFound_ = false;
            rssi_ = pendingRssi_;
            connectAttempts_ = 0;
            preferWarmReconnect_ = false;
            if (manualScanPending_) {
                copyString(manualCandidateAddress_, sizeof(manualCandidateAddress_), pendingAddress_);
                manualCandidateAddrType_ = pendingAddrType_;
                manualCandidateValid_ = true;
                setConnectTarget(manualCandidateAddress_, manualCandidateAddrType_, true);
                manualScanPreemptProxy_ = false;
            } else {
                setSavedAddressFromBuffer(pendingAddress_);
                savedAddrType_ = pendingAddrType_;
                setConnectTargetFromSaved();
            }
            transitionTo(ObdConnectionState::CONNECTING, nowMs);
            break;
        }
        if ((nowMs - stateEnteredMs_) >= obd::SCAN_DURATION_MS) {
            if (manualScanPending_) {
                clearManualScanState();
            }
            transitionTo(ObdConnectionState::IDLE, nowMs);
        }
        break;
    }

    case ObdConnectionState::CONNECTING:
        if (bleDisconnected_) {
            handleConnectFailure(nowMs, ObdFailureReason::CONNECT_START, bleDisconnectReason_);
            break;
        }
        {
            ObdTransportResult transportResult{};
            if (takeTransportResult(ObdTransportOp::CONNECT, transportResult)) {
                if (!transportResult.success || transportResult.timedOut) {
                    handleConnectFailure(nowMs, transportResult.timedOut ? ObdFailureReason::CONNECT_TIMEOUT
                                                                         : ObdFailureReason::CONNECT_START);
                    break;
                }
            }
        }
        if (justEntered) {
            bleDisconnectReason_ = 0;
            lastConnectStartMs_ = nowMs;
            const bool preferCachedAttributes = preferWarmReconnect_ && savedAddress_[0] != '\0';
            if (!beginTransportRequest(ObdTransportOp::CONNECT, nowMs, obd::CONNECT_TIMEOUT_MS, nullptr, false,
                                       preferCachedAttributes)) {
                handleConnectFailure(nowMs, ObdFailureReason::CONNECT_START);
                break;
            }
        }
        if (isBleConnected()) {
            connectAttempts_ = 0;
            connectSuccesses_++;
            lastConnectSuccessMs_ = nowMs;
            if (pendingTransportOp_ == ObdTransportOp::CONNECT || readyTransportResult_.op == ObdTransportOp::CONNECT) {
                clearTransportRequest();
                readyTransportResult_ = {};
            }
            transitionTo(ObdConnectionState::DISCOVERING, nowMs);
            break;
        }
        if ((nowMs - stateEnteredMs_) >= obd::CONNECT_TIMEOUT_MS) {
            clearTransportRequest();
            readyTransportResult_ = {};
            disconnectBle();
            handleConnectFailure(nowMs, ObdFailureReason::CONNECT_TIMEOUT);
            break;
        }
        break;

    case ObdConnectionState::SECURING:
        updateSecuring(nowMs);
        break;

    case ObdConnectionState::DISCOVERING:
        if (bleDisconnected_) {
#ifndef UNIT_TEST
            Serial.printf("[OBD] lost connection during discovery (ble reason=%d %s)\n", bleDisconnectReason_,
                          bleReasonName(bleDisconnectReason_));
#endif
            bleDisconnected_ = false;
            if (autoHealBondIfAllowed(nowMs, "discovering_disconnect")) {
                break;
            }
            if (manualScanPending_) {
                clearManualScanState();
                transitionTo(ObdConnectionState::IDLE, nowMs);
                break;
            }
            transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
            break;
        }
        // DA14531 BLE 4.2 needs time after connect before GATT ops
        if ((nowMs - stateEnteredMs_) < obd::POST_CONNECT_SETTLE_MS) {
            break;
        }
        {
            ObdTransportResult transportResult{};
            if (takeTransportResult(ObdTransportOp::DISCOVER, transportResult)) {
                if (!transportResult.success || transportResult.timedOut) {
                    disconnectBle();
                    handleConnectFailure(nowMs, ObdFailureReason::DISCOVERY);
                    break;
                }
                if (bleDisconnected_) {
#ifndef UNIT_TEST
                    Serial.printf("[OBD] lost connection after discovery (ble reason=%d %s)\n", bleDisconnectReason_,
                                  bleReasonName(bleDisconnectReason_));
#endif
                    bleDisconnected_ = false;
                    if (autoHealBondIfAllowed(nowMs, "post_discover_disconnect")) {
                        break;
                    }
                    if (manualScanPending_) {
                        clearManualScanState();
                        transitionTo(ObdConnectionState::IDLE, nowMs);
                        break;
                    }
                    transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
                    break;
                }
                if (!beginTransportRequest(ObdTransportOp::SUBSCRIBE, nowMs, obd::CONNECT_TIMEOUT_MS)) {
                    disconnectBle();
                    handleConnectFailure(nowMs, ObdFailureReason::SUBSCRIBE);
                    break;
                }
                break;
            }
            if (takeTransportResult(ObdTransportOp::SUBSCRIBE, transportResult)) {
                if (!transportResult.success || transportResult.timedOut) {
                    disconnectBle();
                    handleConnectFailure(nowMs, ObdFailureReason::SUBSCRIBE);
                    break;
                }
                resetInitState(preferWarmReconnect_);
                preferWarmReconnect_ = true;
                transitionTo(ObdConnectionState::AT_INIT, nowMs);
                break;
            }
            if (!transportRequestActive_ && !readyTransportResult_.ready &&
                !beginTransportRequest(ObdTransportOp::DISCOVER, nowMs, obd::CONNECT_TIMEOUT_MS)) {
                disconnectBle();
                handleConnectFailure(nowMs, ObdFailureReason::DISCOVERY);
                break;
            }
        }
        break;

    case ObdConnectionState::AT_INIT:
        updateAtInit(nowMs);
        break;

    case ObdConnectionState::POLLING:
        updatePolling(nowMs);
        break;

    case ObdConnectionState::ERROR_BACKOFF:
        if ((nowMs - stateEnteredMs_) >= obd::ERROR_PAUSE_MS) {
            if (shouldDisconnectAfterPollingError(lastFailure_) &&
                consecutiveErrors_ >= obd::ERRORS_BEFORE_DISCONNECT) {
                disconnectBle();
                transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
            } else {
                backoffCycles_++;
                if (!shouldDisconnectAfterPollingError(lastFailure_)) {
                    consecutiveErrors_ = 0;
                }
                if (backoffCycles_ >= obd::ECU_IDLE_BACKOFF_THRESHOLD) {
                    v1WasConnectedAtEcuIdle_ = v1Connected;
                    disconnectBle();
                    clearSpeedState();
                    transitionTo(ObdConnectionState::ECU_IDLE, nowMs);
                } else {
                    transitionTo(ObdConnectionState::POLLING, nowMs);
                }
            }
        }
        break;

    case ObdConnectionState::DISCONNECTED: {
        // A late connect may arrive after the one-shot entry cleanup was
        // consumed. Re-check physical ownership on every settled pass before
        // retrying or scanning, while preserving the normal entry cleanup.
        const bool unownedLinkConnected = isBleConnected();
        if (justEntered) {
            clearBleResponseState();
            resetCommandState();
            disconnectBle();
        } else if (unownedLinkConnected) {
            disconnectBle();
        }
        if (unownedLinkConnected) {
            break;
        }
        if (scanRequested_ && (manualScanPending_ || obdScanAllowed) && bleScanIdle && !v1ConnectInProgress) {
            if (startBleScan()) {
                scanRequested_ = false;
                transitionTo(ObdConnectionState::SCANNING, nowMs);
                break;
            }
        }
        if (obdRetryAllowed || connectTargetFromManualCandidate_) {
            if (savedAddress_[0] != '\0') {
                setConnectTargetFromSaved();
                transitionTo(ObdConnectionState::CONNECTING, nowMs);
            } else if (connectAddress_[0] != '\0') {
                // Auto-heal recovery: connectAddress_ still set from prior attempt
                transitionTo(ObdConnectionState::CONNECTING, nowMs);
            } else {
                transitionTo(ObdConnectionState::IDLE, nowMs);
            }
        }
        break;
    }

    case ObdConnectionState::ECU_IDLE:
        if (justEntered) {
            clearBleResponseState();
            resetCommandState();
        }

        // Resume path 1: V1 reconnected (was disconnected when we entered ECU_IDLE)
        if (!v1WasConnectedAtEcuIdle_ && v1Connected) {
            backoffCycles_ = 0;
            transitionTo(ObdConnectionState::WAIT_BOOT, nowMs);
            break;
        }

        // Retry cadence is now coordinator-owned. Re-enter connect only when allowed.
        if (obdRetryAllowed) {
            if (savedAddress_[0] != '\0') {
                setConnectTargetFromSaved();
                preferWarmReconnect_ = true;
                transitionTo(ObdConnectionState::CONNECTING, nowMs);
            }
        }
        break;
    }
}

#ifdef UNIT_TEST
ObdBleArbitrationRequest ObdRuntimeModule::getBleArbitrationRequest() const {
    if (manualScanPreemptProxy_) {
        return ObdBleArbitrationRequest::PREEMPT_PROXY_FOR_MANUAL_SCAN;
    }

    switch (state_) {
    case ObdConnectionState::WAIT_BOOT:
    case ObdConnectionState::CONNECTING:
    case ObdConnectionState::SECURING:
    case ObdConnectionState::DISCOVERING:
    case ObdConnectionState::AT_INIT:
        return (savedAddress_[0] != '\0' || connectAddress_[0] != '\0')
                   ? ObdBleArbitrationRequest::HOLD_PROXY_FOR_AUTO_OBD
                   : ObdBleArbitrationRequest::NONE;
    default:
        return ObdBleArbitrationRequest::NONE;
    }
}
#endif

// ======================================================================
// SNAPSHOT + SPEED QUERY — read-only queries into module state
// ======================================================================

ObdRuntimeStatus ObdRuntimeModule::snapshot(uint32_t nowMs) const {
#ifndef UNIT_TEST
    // Guard against calls before begin() — bleClient_ is still nullptr and
    // the helper methods (isBleConnected, etc.) would dereference it.
    if (!bleClient_)
        return ObdRuntimeStatus{};
#endif

    ObdRuntimeStatus status;
    status.enabled = enabled_;
    status.state = state_;
    status.connected = isBleConnected() || state_ == ObdConnectionState::SECURING ||
                       state_ == ObdConnectionState::DISCOVERING || state_ == ObdConnectionState::AT_INIT ||
                       state_ == ObdConnectionState::POLLING || state_ == ObdConnectionState::ERROR_BACKOFF;
    status.securityReady = isBleSecurityReady();
    status.encrypted = isBleEncrypted();
    status.bonded = isBleBonded();
    status.speedValid = isSpeedFresh(nowMs);
    status.speedMph = speedMph_;
    status.speedSampleTsMs = speedSampleTsMs_;
    status.speedAgeMs = status.speedValid ? (nowMs - speedSampleTsMs_) : UINT32_MAX;
    status.rssi = rssi_;
    status.connectAttempts = connectAttempts_;
    status.connectSuccesses = connectSuccesses_;
    status.connectFailures = connectFailures_;
    status.securityRepairs = securityRepairs_;
    status.scanInProgress = (state_ == ObdConnectionState::SCANNING);
    status.manualScanPending = manualScanPending_;
    status.savedAddressValid = savedAddress_[0] != '\0';
    status.initRetries = initRetries_;
    status.pollCount = pollCount_;
    status.pollErrors = pollErrors_;
    status.staleSpeedCount = staleSpeedCount_;
    status.consecutiveErrors = consecutiveErrors_;
    status.totalBytesReceived = totalBytesReceived_;
    status.bufferOverflows = bufferOverflowCount_;
    status.lastConnectStartMs = lastConnectStartMs_;
    status.lastConnectSuccessMs = lastConnectSuccessMs_;
    status.lastFailureMs = lastFailureMs_;
    status.lastBleError = getBleLastError();
    status.lastSecurityError = getBleSecurityFailure();
    status.lastFailure = lastFailure_;
    status.commandInFlight = activeCommand_.active ? activeCommand_.kind : ObdCommandKind::NONE;
    return status;
}

bool ObdRuntimeModule::getFreshSpeed(uint32_t nowMs, float& speedMphOut, uint32_t& tsMsOut) const {
    if (!isSpeedFresh(nowMs))
        return false;
    speedMphOut = speedMph_;
    tsMsOut = speedSampleTsMs_;
    return true;
}

// ======================================================================
// PUBLIC CONTROL API — startScan, stopActiveScan, requestManualPairScan, etc.
// ======================================================================

bool ObdRuntimeModule::startScan() {
    if (!enabled_ || state_ == ObdConnectionState::SCANNING || scanRequested_)
        return false;
    scanRequested_ = true;
    return true;
}

void ObdRuntimeModule::stopActiveScan() {
    stopBleScan();
    scanRequested_ = false;
    pendingDeviceFound_ = false;
    pendingAddress_[0] = '\0';
    if (manualScanPending_) {
        clearManualScanState();
    }

    if (state_ == ObdConnectionState::SCANNING) {
        clearTransportRequest();
        readyTransportResult_ = {};
        bleDisconnected_ = false;
        transitionTo(ObdConnectionState::IDLE, static_cast<uint32_t>(millis()));
    }
}

bool ObdRuntimeModule::requestManualPairScan(uint32_t nowMs) {
    if (!enabled_ || isBleConnected() || manualScanPending_ || scanRequested_ ||
        state_ == ObdConnectionState::SCANNING) {
        return false;
    }

    stopBleScan();
    disconnectBle();
    clearBleEventQueue();
    clearBleResponseState();
    resetCommandState();
    clearTransportRequest();
    readyTransportResult_ = {};
    bleDisconnected_ = false;
    pendingDeviceFound_ = false;
    pendingAddress_[0] = '\0';
    connectAttempts_ = 0;
    clearManualScanState();
    manualScanPending_ = true;
    manualScanPreemptProxy_ = true;
    scanRequested_ = true;
    state_ = ObdConnectionState::IDLE;
    stateEnteredMs_ = nowMs;
    stateEntryPending_ = false;
    return true;
}

void ObdRuntimeModule::cancelPendingConnect() {
    const bool connectStateActive = state_ == ObdConnectionState::CONNECTING ||
                                    state_ == ObdConnectionState::SECURING ||
                                    state_ == ObdConnectionState::DISCOVERING || state_ == ObdConnectionState::AT_INIT;
    const bool connectTransportActive =
        transportRequestActive_ &&
        (pendingTransportOp_ == ObdTransportOp::CONNECT || pendingTransportOp_ == ObdTransportOp::SECURITY_START ||
         pendingTransportOp_ == ObdTransportOp::DISCOVER || pendingTransportOp_ == ObdTransportOp::SUBSCRIBE);
    if (!connectStateActive && !connectTransportActive && !isBleConnected()) {
        return;
    }

    disconnectBle();
    clearBleEventQueue();
    clearBleResponseState();
    resetCommandState();
    clearTransportRequest();
    readyTransportResult_ = {};
    bleDisconnected_ = false;
    if (manualScanPending_) {
        clearManualScanState();
    }
    transitionTo(ObdConnectionState::IDLE, static_cast<uint32_t>(millis()));
}

bool ObdRuntimeModule::isScanStopped() const {
    return state_ != ObdConnectionState::SCANNING && !scanRequested_ && !pendingDeviceFound_;
}

bool ObdRuntimeModule::isConnectIdle() const {
    const bool connectStateActive = state_ == ObdConnectionState::CONNECTING ||
                                    state_ == ObdConnectionState::SECURING ||
                                    state_ == ObdConnectionState::DISCOVERING || state_ == ObdConnectionState::AT_INIT;
    const bool connectTransportActive =
        transportRequestActive_ &&
        (pendingTransportOp_ == ObdTransportOp::CONNECT || pendingTransportOp_ == ObdTransportOp::SECURITY_START ||
         pendingTransportOp_ == ObdTransportOp::DISCOVER || pendingTransportOp_ == ObdTransportOp::SUBSCRIBE);
    return !connectStateActive && !connectTransportActive && !transportDisconnectPending_ && !isBleConnected();
}

void ObdRuntimeModule::forgetDevice() {
    stopBleScan();
    disconnectBle(true);
    clearBleEventQueue();
    setSavedAddressFromBuffer("");
    pendingAddress_[0] = '\0';
    pendingDeviceFound_ = false;
    scanRequested_ = false;
    clearManualScanState();
    connectAttempts_ = 0;
    clearSpeedState();
    clearBleResponseState();
    resetCommandState();
    bleDisconnected_ = false;
    repairedBondAddress_[0] = '\0';
    state_ = ObdConnectionState::IDLE;
    stateEnteredMs_ = 0;
    stateEntryPending_ = false;
}

// ======================================================================
// TEST-ONLY HOOKS
// ======================================================================

#ifdef UNIT_TEST
void ObdRuntimeModule::injectSpeedForTest(float speedMph, uint32_t timestampMs) {
    speedMph_ = speedMph;
    speedSampleTsMs_ = timestampMs;
    speedValid_ = true;
    consecutiveErrors_ = 0;
    backoffCycles_ = 0;
}

void ObdRuntimeModule::forceStateForTest(ObdConnectionState state, uint32_t enteredMs) {
    state_ = state;
    stateEnteredMs_ = enteredMs;
    stateEntryPending_ = false;
    clearBleEventQueue();
    clearBleResponseState();
    resetCommandState();
    bleDisconnected_ = false;
}

void ObdRuntimeModule::transitionToPollingForTest(uint32_t nowMs) {
    transitionTo(ObdConnectionState::POLLING, nowMs);
}

ObdCommandKind ObdRuntimeModule::getActiveCommandKindForTest() const {
    return activeCommand_.active ? activeCommand_.kind : ObdCommandKind::NONE;
}
#endif
