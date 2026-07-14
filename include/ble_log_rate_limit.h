#pragma once

#include <cstdint>

constexpr uint32_t kBleConnectionLogMinIntervalMs = 10000;
constexpr uint32_t kBleResyncLogMinIntervalMs = 1000;

struct BleLogRateLimitState {
    uint32_t lastLogMs = 0;
    bool hasLogged = false;
};

inline bool shouldLogBleConnectionEvent(BleLogRateLimitState& state, uint32_t nowMs,
                                        uint32_t minIntervalMs = kBleConnectionLogMinIntervalMs) {
    if (!state.hasLogged || static_cast<uint32_t>(nowMs - state.lastLogMs) >= minIntervalMs) {
        state.lastLogMs = nowMs;
        state.hasLogged = true;
        return true;
    }
    return false;
}
