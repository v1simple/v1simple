/**
 * OBD Runtime — Connection State Machine
 *
 * Owns the state-machine slice extracted from obd_runtime_module.cpp.
 *
 * Owns:
 *   - Connection-state transitions (transitionTo + state-name helper).
 *   - Per-state handlers: updateSecuring, updateAtInit, updatePolling.
 *   - Target-address bookkeeping (saved/connect/manual scan candidates).
 *
 * Called from obd_runtime_module.cpp::update() which dispatches into the
 * appropriate handler based on current state_.
 */

#include "obd_runtime_module.h"

#include <cstring>

#ifndef UNIT_TEST
#include <Arduino.h>
#endif

#include "obd_scan_policy.h"
#include "obd_string_utils.h"

#ifndef OBD_RSSI_REFRESH_MS_DEFINED
#define OBD_RSSI_REFRESH_MS_DEFINED
static constexpr uint32_t OBD_RSSI_REFRESH_MS = 2000;
#endif

using ObdStringUtils::commandDisplayLen;
using ObdStringUtils::copyString;

namespace {

// ======================================================================
// STATE MACHINE — transitions, per-state handler dispatch, state name enum
// ======================================================================

#ifndef OBD_STATENAME_DEFINED
#define OBD_STATENAME_DEFINED
const char* obdStateName(ObdConnectionState s) {
    switch (s) {
        case ObdConnectionState::IDLE:          return "IDLE";
        case ObdConnectionState::WAIT_BOOT:     return "WAIT_BOOT";
        case ObdConnectionState::SCANNING:      return "SCANNING";
        case ObdConnectionState::CONNECTING:    return "CONNECTING";
        case ObdConnectionState::SECURING:      return "SECURING";
        case ObdConnectionState::DISCOVERING:   return "DISCOVERING";
        case ObdConnectionState::AT_INIT:       return "AT_INIT";
        case ObdConnectionState::POLLING:       return "POLLING";
        case ObdConnectionState::ERROR_BACKOFF: return "ERROR_BACKOFF";
        case ObdConnectionState::DISCONNECTED:  return "DISCONNECTED";
        case ObdConnectionState::ECU_IDLE:      return "ECU_IDLE";
        default:                                return "?";
    }
}
#endif  // OBD_STATENAME_DEFINED

}  // namespace

void ObdRuntimeModule::transitionTo(ObdConnectionState newState, uint32_t nowMs) {
    if (newState == ObdConnectionState::POLLING) {
        commitManualScanCandidate();
    }
    state_ = newState;
    stateEnteredMs_ = nowMs;
    stateEntryPending_ = true;
}

// ======================================================================
// ADDRESS / TARGET MANAGEMENT — saved/connect/manual scan addresses
// ======================================================================

void ObdRuntimeModule::setSavedAddressFromBuffer(const char* address) {
    copyString(savedAddress_, sizeof(savedAddress_), address);
}

void ObdRuntimeModule::setConnectTarget(const char* address,
                                        uint8_t addrType,
                                        bool fromManualCandidate) {
    copyString(connectAddress_, sizeof(connectAddress_), address);
    connectAddrType_ = addrType;
    connectTargetFromManualCandidate_ = fromManualCandidate && connectAddress_[0] != '\0';
}

void ObdRuntimeModule::setConnectTargetFromSaved() {
    setConnectTarget(savedAddress_, savedAddrType_, false);
}

void ObdRuntimeModule::clearConnectTarget() {
    connectAddress_[0] = '\0';
    connectAddrType_ = 0;
    connectTargetFromManualCandidate_ = false;
}

void ObdRuntimeModule::clearManualScanState() {
    manualScanPending_ = false;
    manualScanPreemptProxy_ = false;
    manualCandidateValid_ = false;
    manualCandidateAddress_[0] = '\0';
    manualCandidateAddrType_ = 0;
    clearConnectTarget();
}

void ObdRuntimeModule::commitManualScanCandidate() {
    if (!manualScanPending_ || !manualCandidateValid_) {
        return;
    }

    const bool addressChanged =
        strcmp(savedAddress_, manualCandidateAddress_) != 0 || savedAddrType_ != manualCandidateAddrType_;

    setSavedAddressFromBuffer(manualCandidateAddress_);
    savedAddrType_ = manualCandidateAddrType_;
    if (addressChanged) {
        preferWarmReconnect_ = false;
    }
    clearManualScanState();
}

// ======================================================================
// PER-STATE HANDLERS — Securing / AtInit / Polling state machines
// ======================================================================

void ObdRuntimeModule::updateSecuring(uint32_t nowMs) {
    ObdTransportResult transportResult{};

    if (bleDisconnected_) {
#ifndef UNIT_TEST
        Serial.printf("[OBD] lost connection during securing (ble reason=%d %s)\n",
                      bleDisconnectReason_,
                      bleReasonName(bleDisconnectReason_));
#endif
        bleDisconnected_ = false;
        if (autoHealBondIfAllowed(nowMs, "securing_disconnect")) {
            return;
        }
        handleConnectFailure(nowMs, ObdFailureReason::SECURITY_TIMEOUT);
        return;
    }

    if ((nowMs - stateEnteredMs_) < obd::POST_CONNECT_SETTLE_MS) {
        return;
    }

    if (isBleSecurityReady() || isBleEncrypted()) {
        transitionTo(ObdConnectionState::DISCOVERING, nowMs);
        return;
    }

    if (takeTransportResult(ObdTransportOp::SECURITY_START, transportResult) &&
        (!transportResult.success || transportResult.timedOut)) {
#ifndef UNIT_TEST
        Serial.printf("[OBD] secureConnection start failed rc=%d (%s)\n",
                      transportResult.securityError,
                      bleReasonName(transportResult.securityError));
#endif
        if (autoHealBondIfAllowed(nowMs, "securing_start")) {
            return;
        }
        disconnectBle();
        handleConnectFailure(nowMs, ObdFailureReason::SECURITY_START);
        return;
    }

    if (!transportRequestActive_ &&
        !readyTransportResult_.ready &&
        !beginTransportRequest(ObdTransportOp::SECURITY_START, nowMs, obd::SECURITY_TIMEOUT_MS)) {
#ifndef UNIT_TEST
        Serial.printf("[OBD] secureConnection request queue failed rc=%d (%s)\n",
                      getBleSecurityFailure(),
                      bleReasonName(getBleSecurityFailure()));
#endif
        if (autoHealBondIfAllowed(nowMs, "securing_start")) {
            return;
        }
        disconnectBle();
        handleConnectFailure(nowMs, ObdFailureReason::SECURITY_START);
        return;
    }

    if ((nowMs - stateEnteredMs_) >= (obd::POST_CONNECT_SETTLE_MS + obd::SECURITY_TIMEOUT_MS)) {
#ifndef UNIT_TEST
        Serial.printf("[OBD] securing timed out bleError=%d (%s) securityError=%d (%s)\n",
                      getBleLastError(),
                      bleReasonName(getBleLastError()),
                      getBleSecurityFailure(),
                      bleReasonName(getBleSecurityFailure()));
#endif
        if (autoHealBondIfAllowed(nowMs, "securing_timeout")) {
            return;
        }
        disconnectBle();
        handleConnectFailure(nowMs, ObdFailureReason::SECURITY_TIMEOUT);
    }
}

void ObdRuntimeModule::updateAtInit(uint32_t nowMs) {
    ObdTransportResult transportResult{};

    if (bleDisconnected_) {
#ifndef UNIT_TEST
        Serial.printf("[OBD] lost connection during AT init (ble reason=%d %s)\n",
                      bleDisconnectReason_,
                      bleReasonName(bleDisconnectReason_));
#endif
        bleDisconnected_ = false;
        if (autoHealBondIfAllowed(nowMs, "at_init_disconnect")) {
            return;
        }
        if (manualScanPending_) {
            clearManualScanState();
            transitionTo(ObdConnectionState::IDLE, nowMs);
            return;
        }
        transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
        return;
    }

    if (activeCommand_.active) {
        if (activeCommand_.sentMs == 0) {
            if (!takeTransportResult(ObdTransportOp::WRITE, transportResult)) {
                return;
            }
            if (!transportResult.success || transportResult.timedOut) {
                const int cmdLen = static_cast<int>(commandDisplayLen(activeCommand_.tx));
#ifndef UNIT_TEST
                Serial.printf("[OBD] AT init write failed cmd=%.*s writeMode=%s rc=%d (%s) timedOut=%d\n",
                              cmdLen,
                              activeCommand_.tx,
                              activeCommand_.writeWithResponse ? "with_response" : "no_response",
                              transportResult.bleError,
                              bleReasonName(transportResult.bleError),
                              transportResult.timedOut ? 1 : 0);
#endif
                if (initIndex_ == 0 && autoHealBondIfAllowed(nowMs, "at_init_write")) {
                    return;
                }
                disconnectBle();
                handleConnectFailure(nowMs,
                                     transportResult.timedOut
                                         ? ObdFailureReason::INIT_TIMEOUT
                                         : ObdFailureReason::WRITE);
                return;
            }
            activeCommand_.sentMs = transportResult.issuedMs;
            return;
        }
        if (bleDataReady_) {
            handleAtInitResponse(nowMs);
            return;
        }
        if (nowMs - activeCommand_.sentMs >= activeCommand_.timeoutMs) {
            const int cmdLen = static_cast<int>(commandDisplayLen(activeCommand_.tx));
#ifndef UNIT_TEST
            Serial.printf("[OBD] AT init response timed out cmd=%.*s writeMode=%s rxBytes=%u raw=[%.*s] securityReady=%d enc=%d bond=%d auth=%d lastBleError=%d (%s) disconnectReason=%d (%s)\n",
                          cmdLen,
                          activeCommand_.tx,
                          activeCommand_.writeWithResponse ? "with_response" : "no_response",
                          static_cast<unsigned>(bleBufLen_),
                          static_cast<int>(bleBufLen_),
                          bleBuf_,
                          isBleSecurityReady(),
                          isBleEncrypted(),
                          isBleBonded(),
                          isBleAuthenticated(),
                          getBleLastError(),
                          bleReasonName(getBleLastError()),
                          bleDisconnectReason_,
                          bleReasonName(bleDisconnectReason_));
#endif
            // 0100 (sanity) times out when vehicle isn't running - non-fatal.
            if (activeCommand_.kind == ObdCommandKind::SANITY) {
#ifndef UNIT_TEST
                Serial.printf("[OBD] sanity 0100 timed out rxBytes=%u (vehicle may be off) - skipping\n",
                              static_cast<unsigned>(bleBufLen_));
#endif
                clearBleResponseState();
                completeActiveCommand();
                initIndex_++;
                return;
            }
            if (bleBufLen_ == 0 && retryActiveCommandWithAlternateWriteMode(nowMs)) {
#ifndef UNIT_TEST
                Serial.printf("[OBD] AT init retrying cmd=%.*s with alternate write mode=%s after empty timeout\n",
                              cmdLen,
                              activeCommand_.tx,
                              activeCommand_.writeWithResponse ? "with_response" : "no_response");
#endif
                return;
            }
            if (retryActiveCommand(nowMs)) {
                return;
            }
            if (warmInitPreferred_ && !coldInitFallbackUsed_) {
                coldInitFallbackUsed_ = true;
                resetInitState(false);
                return;
            }
            disconnectBle();
            handleConnectFailure(nowMs, ObdFailureReason::INIT_TIMEOUT);
        }
        return;
    }

    if ((nowMs - stateEnteredMs_) < obd::POST_SUBSCRIBE_SETTLE_MS) {
        return;
    }

    const char* const* commands = warmInitPreferred_ ? obd::WARM_INIT_COMMANDS : obd::COLD_INIT_COMMANDS;
    const size_t commandCount = warmInitPreferred_ ? obd::WARM_INIT_COMMAND_COUNT : obd::COLD_INIT_COMMAND_COUNT;
    if (initIndex_ >= commandCount) {
        consecutiveErrors_ = 0;
        resetPollingSchedule(nowMs);
        clearBleResponseState();
        transitionTo(ObdConnectionState::POLLING, nowMs);
        return;
    }

    const char* command = commands[initIndex_];
    const bool isSanity = strncmp(command, "0100", 4) == 0;
    if (!startCommand(isSanity ? ObdCommandKind::SANITY : ObdCommandKind::AT_INIT,
                      isSanity ? ParserKind::SIMPLE : ParserKind::AT_TEXT,
                      command,
                      isSanity ? 0x41 : 0x00,
                      0x00,
                      0x0000,
                      obd::AT_INIT_RESPONSE_TIMEOUT_MS,
                      obd::AT_INIT_RETRIES,
                      nowMs)) {
#ifndef UNIT_TEST
        const int cmdLen = static_cast<int>(commandDisplayLen(command));
        Serial.printf("[OBD] AT init write failed cmd=%.*s writeMode=%s rc=%d (%s) securityReady=%d enc=%d bond=%d auth=%d bleReason=%d (%s)\n",
                      cmdLen,
                      command,
                      preferWriteWithResponse_ ? "with_response" : "no_response",
                      getBleLastError(),
                      bleReasonName(getBleLastError()),
                      isBleSecurityReady(),
                      isBleEncrypted(),
                      isBleBonded(),
                      isBleAuthenticated(),
                      bleDisconnectReason_,
                      bleReasonName(bleDisconnectReason_));
#endif
        if (initIndex_ == 0 && autoHealBondIfAllowed(nowMs, "at_init_write")) {
            return;
        }
        disconnectBle();
        handleConnectFailure(nowMs, ObdFailureReason::WRITE);
    }
}

void ObdRuntimeModule::updatePolling(uint32_t nowMs) {
    ObdTransportResult transportResult{};

    if (bleDisconnected_) {
#ifndef UNIT_TEST
        Serial.printf("[OBD] lost connection during polling (ble reason=%d %s)\n",
                      bleDisconnectReason_,
                      bleReasonName(bleDisconnectReason_));
#endif
        bleDisconnected_ = false;
        clearSpeedState();
        clearBleResponseState();
        resetCommandState();
        transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
        return;
    }

    if (activeCommand_.active) {
        if (activeCommand_.sentMs == 0) {
            if (!takeTransportResult(ObdTransportOp::WRITE, transportResult)) {
                return;
            }
            if (!transportResult.success || transportResult.timedOut) {
                clearBleResponseState();
                completeActiveCommand();
                handlePollingError(nowMs,
                                   false,
                                   transportResult.timedOut
                                       ? ObdFailureReason::COMMAND_TIMEOUT
                                       : ObdFailureReason::WRITE);
                return;
            }
            activeCommand_.sentMs = transportResult.issuedMs;
            return;
        }
        if (bleDataReady_) {
            handlePollingResponse(nowMs);
            if (state_ != ObdConnectionState::POLLING) {
                return;
            }
        } else if (nowMs - activeCommand_.sentMs >= activeCommand_.timeoutMs) {
            // CX sends "SEARCHING..." while probing OBD protocols (up to ~10s).
            // Extend timeout so we don't retry mid-search and collide.
            if (activeCommand_.kind == ObdCommandKind::SPEED &&
                bleBufLen_ >= 9 &&
                strstr(bleBuf_, "SEARCHING") != nullptr &&
                (nowMs - activeCommand_.sentMs) < obd::SEARCH_EXTENDED_TIMEOUT_MS) {
                return;
            }
            if (activeCommand_.kind == ObdCommandKind::SPEED &&
                bleBufLen_ == 0 &&
                retryActiveCommandWithAlternateWriteMode(nowMs)) {
#ifndef UNIT_TEST
                Serial.printf("[OBD] speed timeout retrying with alternate write mode=%s\n",
                              activeCommand_.writeWithResponse ? "with_response" : "no_response");
#endif
                return;
            }
#ifndef UNIT_TEST
            if (activeCommand_.kind == ObdCommandKind::SPEED && bleBufLen_ > 0) {
                Serial.printf("[OBD] speed timeout rxBytes=%u raw=[%.*s]\n",
                              static_cast<unsigned>(bleBufLen_),
                              static_cast<int>(bleBufLen_),
                              bleBuf_);
            }
#endif
            clearBleResponseState();
            completeActiveCommand();
            handlePollingError(nowMs, false, ObdFailureReason::COMMAND_TIMEOUT);
            return;
        }
    }

    if (!activeCommand_.active) {
        sendNextPollingCommand(nowMs);
    }

    if (takeTransportResult(ObdTransportOp::RSSI_READ, transportResult)) {
        rssi_ = transportResult.rssi;
        lastRssiMs_ = nowMs;
    } else if (!transportRequestActive_ &&
               !readyTransportResult_.ready &&
               !(activeCommand_.active && activeCommand_.sentMs == 0) &&
               static_cast<int32_t>(nowMs - lastRssiMs_) >= static_cast<int32_t>(OBD_RSSI_REFRESH_MS)) {
        if (beginTransportRequest(ObdTransportOp::RSSI_READ, nowMs, 0)) {
            lastRssiMs_ = nowMs;
        }
    }

    if (speedValid_ && !isSpeedFresh(nowMs)) {
        speedValid_ = false;
        staleSpeedCount_++;
    }
}
