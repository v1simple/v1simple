/**
 * OBD Runtime — ELM327 Command Lifecycle & Response Parsing
 *
 * Owns the command slice extracted from obd_runtime_module.cpp.
 *
 * Owns:
 *   - ELM327 command lifecycle: startCommand, retry, complete, reset.
 *   - Response validation: AT responses, simple responses.
 *   - Response handlers: speed (PID 0x0D), AT init sequence.
 *   - Speed polling scheduler: freshness, due-time, next-poll dispatch.
 *
 * Reads bleBuf_ populated by obd_runtime_transport.cpp::applyBleEvent and
 * drains it during parse. Writes speed fields consumed by the public
 * snapshot() / getFreshSpeed() API which remain in obd_runtime_module.cpp.
 */

#include "obd_runtime_module.h"

#include <cctype>
#include <cstring>

#ifndef UNIT_TEST
#include <Arduino.h>
#endif

#include "obd_elm327_parser.h"
#include "obd_scan_policy.h"
#include "obd_string_utils.h"
#include "../../perf_metrics.h"

// ======================================================================
// FILE-SCOPE HELPERS — string utilities for command parsing
// ======================================================================

using ObdStringUtils::copyString;
using ObdStringUtils::stringContainsCI;

// ======================================================================
// SPEED STATE RESET — clear speed polling state on transition
// ======================================================================

void ObdRuntimeModule::clearSpeedState() {
    speedMph_ = 0.0f;
    speedSampleTsMs_ = 0;
    speedValid_ = false;
    consecutiveSpeedSamples_ = 0;
}

void ObdRuntimeModule::resetPollingSchedule(uint32_t nowMs) {
    nextSpeedDueMs_ = nowMs;
}

void ObdRuntimeModule::resetCommandState() {
    activeCommand_ = ActiveObdCommand{};
}

// ======================================================================
// RESPONSE VALIDATION — parse + interpret AT/speed responses
// ======================================================================

bool ObdRuntimeModule::validateAtResponse(const char* command, const char* response, size_t len) const {
    if (!command || !response || len == 0) {
        return false;
    }

    if (strncmp(command, "0100", 4) == 0) {
        return validateSimpleResponse(0x41, 0x00, response, len);
    }
    if (strncmp(command, "ATZ", 3) == 0 || strncmp(command, "ATI", 3) == 0) {
        return stringContainsCI(response, "OBDLINK") || stringContainsCI(response, "STN") ||
               stringContainsCI(response, "ELM327");
    }
    return stringContainsCI(response, "OK");
}

bool ObdRuntimeModule::validateSimpleResponse(uint8_t expectedService, uint8_t expectedPid, const char* response,
                                              size_t len) const {
    Elm327ParseResult result = parseElm327Response(response, len);
    return result.valid && result.service == expectedService && result.pid == expectedPid;
}

// ======================================================================
// COMMAND LIFECYCLE — start/retry/complete command sequence
// ======================================================================

bool ObdRuntimeModule::startCommand(ObdCommandKind kind, ParserKind parser, const char* tx, uint8_t expectedService,
                                    uint8_t expectedPid, uint16_t expectedDid, uint32_t timeoutMs, uint8_t retries,
                                    uint32_t nowMs) {
    if (!tx || tx[0] == '\0') {
        return false;
    }

    clearBleResponseState();
    activeCommand_ = ActiveObdCommand{};
    activeCommand_.active = true;
    activeCommand_.kind = kind;
    activeCommand_.parser = parser;
    activeCommand_.expectedService = expectedService;
    activeCommand_.expectedPid = expectedPid;
    activeCommand_.expectedDid = expectedDid;
    activeCommand_.timeoutMs = timeoutMs;
    activeCommand_.retriesRemaining = retries;
    activeCommand_.writeWithResponse = preferWriteWithResponse_;
    activeCommand_.alternateWriteModeTried = false;
    copyString(activeCommand_.tx, sizeof(activeCommand_.tx), tx);

    activeCommand_.sentMs = 0;
    if (!beginTransportRequest(ObdTransportOp::WRITE, nowMs, 0, activeCommand_.tx, activeCommand_.writeWithResponse)) {
        resetCommandState();
        return false;
    }
    return true;
}

bool ObdRuntimeModule::retryActiveCommand(uint32_t nowMs) {
    if (!activeCommand_.active || activeCommand_.retriesRemaining == 0) {
        return false;
    }

    activeCommand_.retriesRemaining--;
    initRetries_++;
    clearBleResponseState();
    activeCommand_.sentMs = 0;
    if (!beginTransportRequest(ObdTransportOp::WRITE, nowMs, 0, activeCommand_.tx, activeCommand_.writeWithResponse)) {
        return false;
    }
    return true;
}

bool ObdRuntimeModule::retryActiveCommandWithAlternateWriteMode(uint32_t nowMs) {
    if (!activeCommand_.active || activeCommand_.retriesRemaining == 0 || activeCommand_.alternateWriteModeTried) {
        return false;
    }

    activeCommand_.retriesRemaining--;
    activeCommand_.alternateWriteModeTried = true;
    activeCommand_.writeWithResponse = !activeCommand_.writeWithResponse;
    initRetries_++;
    clearBleResponseState();
    activeCommand_.sentMs = 0;
    if (!beginTransportRequest(ObdTransportOp::WRITE, nowMs, 0, activeCommand_.tx, activeCommand_.writeWithResponse)) {
        return false;
    }
    return true;
}

void ObdRuntimeModule::completeActiveCommand() {
    resetCommandState();
}

// ======================================================================
// RESPONSE HANDLERS — handleAtInitResponse + handleSpeedResponse
// ======================================================================

void ObdRuntimeModule::handleAtInitResponse(uint32_t nowMs) {
    if (!activeCommand_.active) {
        return;
    }
    const bool valid = !bleOverflowed_ && validateAtResponse(activeCommand_.tx, bleBuf_, bleBufLen_);
    clearBleResponseState();

    if (valid) {
        preferWriteWithResponse_ = activeCommand_.writeWithResponse;
        completeActiveCommand();
        initIndex_++;
        return;
    }

    // 0100 (supported PIDs) fails when the vehicle isn't running. Main
    // explicitly treats this as non-fatal: "vehicle might just be off".
    // Skip and proceed to polling so AT commands still work for voltage, etc.
    if (activeCommand_.kind == ObdCommandKind::SANITY) {
#ifndef UNIT_TEST
        Serial.println("[OBD] sanity 0100 failed (vehicle may be off) - skipping");
#endif
        completeActiveCommand();
        initIndex_++;
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
    handleConnectFailure(nowMs, ObdFailureReason::INIT_RESPONSE);
}

bool ObdRuntimeModule::handleSpeedResponse(uint32_t nowMs) {
    Elm327ParseResult result = parseElm327Response(bleBuf_, bleBufLen_);
    if (!result.valid || result.service != 0x41 || result.pid != 0x0D) {
        return false;
    }

    const float kmh = decodeSpeedKmh(result);
    if (kmh < 0.0f) {
        return false;
    }

    speedMph_ = kmhToMph(kmh);
    speedSampleTsMs_ = nowMs;
    speedValid_ = true;
    consecutiveSpeedSamples_++;
    consecutiveErrors_ = 0;
    backoffCycles_ = 0;
    pollCount_++;
    nextSpeedDueMs_ = nowMs + obd::POLL_INTERVAL_MS;
    return true;
}

// ======================================================================
// SPEED POLLING — scheduling and freshness checks
// ======================================================================

bool ObdRuntimeModule::isSpeedFresh(uint32_t nowMs) const {
    return speedValid_ && speedSampleTsMs_ != 0 && (nowMs - speedSampleTsMs_) <= obd::SPEED_MAX_AGE_MS;
}

bool ObdRuntimeModule::speedDue(uint32_t nowMs) const {
    return nextSpeedDueMs_ == 0 || static_cast<int32_t>(nowMs - nextSpeedDueMs_) >= 0;
}

bool ObdRuntimeModule::startSpeedCommand(uint32_t nowMs) {
    if (!startCommand(ObdCommandKind::SPEED, ParserKind::SIMPLE, obd::SPEED_POLL_CMD, 0x41, 0x0D, 0x0000,
                      obd::POLL_TIMEOUT_MS, obd::POLL_COMMAND_RETRIES, nowMs)) {
        handlePollingError(nowMs, false, ObdFailureReason::WRITE);
        return false;
    }
    nextSpeedDueMs_ = nowMs + obd::POLL_INTERVAL_MS;
    return true;
}

bool ObdRuntimeModule::sendNextPollingCommand(uint32_t nowMs) {
    return speedDue(nowMs) ? startSpeedCommand(nowMs) : false;
}

// ============================================================================
// Failure Classification & Recovery
// ============================================================================

namespace {

#ifndef OBD_STATENAME_DEFINED
#define OBD_STATENAME_DEFINED
const char* obdStateName(ObdConnectionState s) {
    switch (s) {
    case ObdConnectionState::IDLE:
        return "IDLE";
    case ObdConnectionState::WAIT_BOOT:
        return "WAIT_BOOT";
    case ObdConnectionState::SCANNING:
        return "SCANNING";
    case ObdConnectionState::CONNECTING:
        return "CONNECTING";
    case ObdConnectionState::SECURING:
        return "SECURING";
    case ObdConnectionState::DISCOVERING:
        return "DISCOVERING";
    case ObdConnectionState::AT_INIT:
        return "AT_INIT";
    case ObdConnectionState::POLLING:
        return "POLLING";
    case ObdConnectionState::ERROR_BACKOFF:
        return "ERROR_BACKOFF";
    case ObdConnectionState::DISCONNECTED:
        return "DISCONNECTED";
    case ObdConnectionState::ECU_IDLE:
        return "ECU_IDLE";
    default:
        return "?";
    }
}
#endif // OBD_STATENAME_DEFINED

#ifndef OBD_FAILURE_REASON_NAME_DEFINED
#define OBD_FAILURE_REASON_NAME_DEFINED
const char* obdFailureReasonName(ObdFailureReason reason) {
    switch (reason) {
    case ObdFailureReason::NONE:
        return "NONE";
    case ObdFailureReason::CONNECT_START:
        return "CONNECT_START";
    case ObdFailureReason::CONNECT_TIMEOUT:
        return "CONNECT_TIMEOUT";
    case ObdFailureReason::DISCOVERY:
        return "DISCOVERY";
    case ObdFailureReason::SUBSCRIBE:
        return "SUBSCRIBE";
    case ObdFailureReason::INIT_TIMEOUT:
        return "INIT_TIMEOUT";
    case ObdFailureReason::INIT_RESPONSE:
        return "INIT_RESPONSE";
    case ObdFailureReason::COMMAND_TIMEOUT:
        return "COMMAND_TIMEOUT";
    case ObdFailureReason::COMMAND_RESPONSE:
        return "COMMAND_RESPONSE";
    case ObdFailureReason::WRITE:
        return "WRITE";
    case ObdFailureReason::BUFFER_OVERFLOW:
        return "BUFFER_OVERFLOW";
    case ObdFailureReason::SECURITY_START:
        return "SECURITY_START";
    case ObdFailureReason::SECURITY_TIMEOUT:
        return "SECURITY_TIMEOUT";
    default:
        return "UNKNOWN";
    }
}
#endif // OBD_FAILURE_REASON_NAME_DEFINED

} // namespace

// ======================================================================
// ERROR RECOVERY — failure marking, backoff, connection failure handlers
// ======================================================================

void ObdRuntimeModule::markFailure(ObdFailureReason reason, uint32_t nowMs) {
    lastFailure_ = reason;
    lastFailureMs_ = nowMs;
}

void ObdRuntimeModule::handleConnectFailure(uint32_t nowMs, ObdFailureReason reason, int bleReason) {
    markFailure(reason, nowMs);
    connectFailures_++;
    connectAttempts_++;
    clearBleResponseState();
    resetCommandState();
    bleDisconnected_ = false;
    const uint32_t obdStateMs = stateEnteredMs_ == 0 ? 0 : (nowMs - stateEnteredMs_);
    const bool directAttemptsExhausted = !manualScanPending_ && connectAttempts_ >= obd::MAX_DIRECT_CONNECT_FAILURES;
#ifndef UNIT_TEST
    Serial.printf("[OBD] connect failure reason=%s cycle=%s cycleMs=%lu obdState=%s obdStateMs=%lu attempt=%u/%u "
                  "proxyAdv=%d proxyClient=%d retryAllowed=%d bleReason=%d (%s) next=%s\n",
                  obdFailureReasonName(reason), perfConnectionCycleStateName(connectionCycleStateCode_),
                  static_cast<unsigned long>(connectionCycleTimeInStateMs_), obdStateName(state_),
                  static_cast<unsigned long>(obdStateMs), static_cast<unsigned int>(connectAttempts_),
                  static_cast<unsigned int>(obd::MAX_DIRECT_CONNECT_FAILURES), lastProxyAdvertising_ ? 1 : 0,
                  lastProxyClientConnected_ ? 1 : 0, lastObdRetryAllowed_ ? 1 : 0, bleReason, bleReasonName(bleReason),
                  manualScanPending_ ? "IDLE" : (directAttemptsExhausted ? "IDLE" : "DISCONNECTED"));
#endif
    if (manualScanPending_) {
        // Auto-heal only for post-connection failures where a stale bond
        // could be the cause. Connect failures are not bond-related.
        if ((reason == ObdFailureReason::DISCOVERY || reason == ObdFailureReason::SUBSCRIBE ||
             reason == ObdFailureReason::WRITE || reason == ObdFailureReason::INIT_TIMEOUT) &&
            autoHealBondIfAllowed(nowMs, "manual_pair_failure")) {
            return;
        }
        connectAttempts_ = 0;
        clearManualScanState();
        transitionTo(ObdConnectionState::IDLE, nowMs);
        return;
    }
    if (connectAttempts_ >= obd::MAX_DIRECT_CONNECT_FAILURES) {
        connectAttempts_ = 0;
        transitionTo(ObdConnectionState::IDLE, nowMs);
        return;
    }
    transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
}

void ObdRuntimeModule::handlePollingError(uint32_t nowMs, bool disconnectBleNow, ObdFailureReason reason) {
    markFailure(reason, nowMs);
    pollErrors_++;
    consecutiveErrors_++;
    consecutiveSpeedSamples_ = 0;
    clearBleResponseState();
    resetCommandState();
    if (disconnectBleNow) {
        disconnectBle();
    }
    if (consecutiveErrors_ >= obd::ERRORS_BEFORE_DISCONNECT && shouldDisconnectAfterPollingError(reason)) {
        transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
        return;
    }
    if (consecutiveErrors_ >= obd::MAX_CONSECUTIVE_ERRORS) {
        transitionTo(ObdConnectionState::ERROR_BACKOFF, nowMs);
    }
}

void ObdRuntimeModule::handleCommandFailure(uint32_t nowMs, ObdFailureReason reason, bool disconnectBleNow) {
    handlePollingError(nowMs, disconnectBleNow, reason);
}

bool ObdRuntimeModule::shouldDisconnectAfterPollingError(ObdFailureReason reason) {
    switch (reason) {
    case ObdFailureReason::WRITE:
    case ObdFailureReason::BUFFER_OVERFLOW:
        return true;
    case ObdFailureReason::NONE:
    case ObdFailureReason::CONNECT_START:
    case ObdFailureReason::CONNECT_TIMEOUT:
    case ObdFailureReason::DISCOVERY:
    case ObdFailureReason::SUBSCRIBE:
    case ObdFailureReason::INIT_TIMEOUT:
    case ObdFailureReason::INIT_RESPONSE:
    case ObdFailureReason::COMMAND_TIMEOUT:
    case ObdFailureReason::COMMAND_RESPONSE:
    case ObdFailureReason::SECURITY_START:
    case ObdFailureReason::SECURITY_TIMEOUT:
    default:
        return false;
    }
}

// ======================================================================
// BOND AUTO-HEAL — failure recovery, auto-heal decision + retry
// ======================================================================

bool ObdRuntimeModule::canAutoHealBond() const {
    const char* const addr = connectAddress_[0] != '\0' ? connectAddress_ : savedAddress_;
    return addr[0] != '\0' && strcmp(repairedBondAddress_, addr) != 0;
}

bool ObdRuntimeModule::autoHealBondIfAllowed(uint32_t nowMs, const char* context) {
    if (!canAutoHealBond()) {
        return false;
    }

    if (!disconnectBle(true)) {
        return false;
    }

#ifndef UNIT_TEST
    Serial.printf("[OBD] auto-heal bond during %s addr=%s lastBleError=%d (%s) lastSecurityError=%d (%s)\n",
                  context ? context : "unknown", savedAddress_, getBleLastError(), bleReasonName(getBleLastError()),
                  getBleSecurityFailure(), bleReasonName(getBleSecurityFailure()));
#endif

    clearBleResponseState();
    resetCommandState();
    bleDisconnected_ = false;
    preferWarmReconnect_ = false;
    warmInitPreferred_ = false;
    const char* const healAddr = connectAddress_[0] != '\0' ? connectAddress_ : savedAddress_;
    copyString(repairedBondAddress_, sizeof(repairedBondAddress_), healAddr);
    securityRepairs_++;
#ifdef UNIT_TEST
    // Production refreshes after the transport-owned delete is acknowledged.
    refreshBleBondBackup();
#endif
    transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
    return true;
}
