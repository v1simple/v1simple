#pragma once

#include <stdint.h>

namespace ble_quiesce_policy {

enum class Action : uint8_t {
    WAIT = 0,
    RETRY_PENDING_OPERATIONS = 1,
    REQUEST_DISCONNECT = 2,
    FINALIZE = 3,
    FATAL_RESTART = 4,
};

struct Input {
    bool waitingForConnectCancel = false;
    bool waitingForDiscovery = false;
    bool waitingForDisconnect = false;
    bool clientStillConnected = false;
    uint32_t nowMs = 0;
    uint32_t startedMs = 0;
    uint32_t lastRetryMs = 0;
    uint32_t retryIntervalMs = 0;
    uint32_t fatalTimeoutMs = 0;
};

struct Decision {
    Action action = Action::WAIT;
    bool retryConnectCancel = false;
    bool retryDisconnect = false;
};

constexpr bool elapsedAtLeast(uint32_t nowMs, uint32_t startedMs, uint32_t intervalMs) {
    return static_cast<uint32_t>(nowMs - startedMs) >= intervalMs;
}

constexpr Decision evaluate(const Input& input) {
    const bool waitingForCallback =
        input.waitingForConnectCancel || input.waitingForDiscovery || input.waitingForDisconnect;
    const bool teardownIncomplete = waitingForCallback || input.clientStillConnected;

    if (teardownIncomplete && elapsedAtLeast(input.nowMs, input.startedMs, input.fatalTimeoutMs)) {
        return {Action::FATAL_RESTART, false, false};
    }

    if (waitingForCallback) {
        if (!elapsedAtLeast(input.nowMs, input.lastRetryMs, input.retryIntervalMs)) {
            return {Action::WAIT, false, false};
        }
        return {
            Action::RETRY_PENDING_OPERATIONS,
            input.waitingForConnectCancel,
            input.waitingForDisconnect,
        };
    }

    if (input.clientStillConnected) {
        return {Action::REQUEST_DISCONNECT, false, false};
    }

    return {Action::FINALIZE, false, false};
}

} // namespace ble_quiesce_policy
