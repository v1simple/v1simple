#pragma once

#include <stdint.h>

namespace battery_latch_shutdown_policy {

enum class Verification : uint8_t {
    WRITE_FAILED = 0,
    READBACK_FAILED,
    LATCH_LOW,
    LATCH_HIGH,
};

struct Input {
    bool writeSucceeded = false;
    bool readbackSucceeded = false;
    bool latchLow = false;
};

struct Decision {
    Verification verification = Verification::WRITE_FAILED;
    bool waitForRailCollapse = false;
};

constexpr Decision evaluate(const Input& input) {
    if (!input.writeSucceeded) {
        return {Verification::WRITE_FAILED, false};
    }
    if (!input.readbackSucceeded) {
        return {Verification::READBACK_FAILED, true};
    }
    return {
        input.latchLow ? Verification::LATCH_LOW : Verification::LATCH_HIGH,
        true,
    };
}

constexpr const char* externalOutcomeName(Verification verification) {
    switch (verification) {
    case Verification::WRITE_FAILED:
        return "WRITE_FAILED";
    case Verification::READBACK_FAILED:
        return "READBACK_FAILED";
    case Verification::LATCH_LOW:
        return "LOW";
    case Verification::LATCH_HIGH:
        return "HIGH_STUCK";
    }
    return "READBACK_FAILED";
}

constexpr const char* batteryFallbackReason(Verification verification) {
    switch (verification) {
    case Verification::WRITE_FAILED:
        return "latch_write_failed";
    case Verification::READBACK_FAILED:
        return "latch_readback_failed";
    case Verification::LATCH_LOW:
        return "rail_alive_after_latch";
    case Verification::LATCH_HIGH:
        return "latch_readback_high";
    }
    return "latch_readback_failed";
}

} // namespace battery_latch_shutdown_policy
