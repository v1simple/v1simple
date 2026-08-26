#pragma once

#include <cstdint>

namespace waveshare_349 {

enum class Revision : uint8_t {
    Unknown = 0,
    V1,
    V2,
};

enum class ResetRoute : uint8_t {
    None = 0,
    Gpio21,
    Exio5,
};

struct RoutingPlan {
    int8_t backlightGpio = -1;
    ResetRoute resetRoute = ResetRoute::None;
};

inline RoutingPlan routingFor(Revision revision) {
    switch (revision) {
    case Revision::V1:
        return {8, ResetRoute::Gpio21};
    case Revision::V2:
        return {42, ResetRoute::Exio5};
    case Revision::Unknown:
        break;
    }
    return {};
}

inline uint8_t withPinLevel(uint8_t output, uint8_t pin, bool high) {
    const uint8_t mask = static_cast<uint8_t>(1u << pin);
    return high ? static_cast<uint8_t>(output | mask) : static_cast<uint8_t>(output & static_cast<uint8_t>(~mask));
}

inline uint8_t withPinDirection(uint8_t config, uint8_t pin, bool output) {
    const uint8_t mask = static_cast<uint8_t>(1u << pin);
    return output ? static_cast<uint8_t>(config & static_cast<uint8_t>(~mask))
                  : static_cast<uint8_t>(config | mask);
}

struct LevelSamples {
    uint16_t low = 0;
    uint16_t high = 0;
    uint16_t readFailures = 0;

    uint16_t total() const { return static_cast<uint16_t>(low + high); }
    bool stableLow(uint16_t minimumSamples) const {
        return readFailures == 0 && total() >= minimumSamples && high == 0;
    }
    bool stableHigh(uint16_t minimumSamples) const {
        return readFailures == 0 && total() >= minimumSamples && low == 0;
    }
    bool lowDominant(uint16_t minimumSamples) const {
        return readFailures == 0 && total() >= minimumSamples &&
               static_cast<uint32_t>(low) * 4u >= static_cast<uint32_t>(total()) * 3u;
    }
};

struct RevisionEvidence {
    LevelSamples gpio21;
    LevelSamples exio5;
};

inline Revision classifyRevision(const RevisionEvidence& evidence, uint16_t minimumSamples) {
    // The reset line is pulled continuously HIGH. The TE line is LOW at idle,
    // but a software/upload reset can leave the panel producing short HIGH TE
    // pulses. Require a stable HIGH reset candidate and a strongly LOW-dominant
    // counterpart; sparse noise and 50/50 instability remain Unknown.
    if (evidence.gpio21.stableHigh(minimumSamples) && evidence.exio5.lowDominant(minimumSamples)) {
        return Revision::V1;
    }
    if (evidence.gpio21.lowDominant(minimumSamples) && evidence.exio5.stableHigh(minimumSamples)) {
        return Revision::V2;
    }
    return Revision::Unknown;
}

inline const char* revisionName(Revision revision) {
    switch (revision) {
    case Revision::V1:
        return "V1";
    case Revision::V2:
        return "V2";
    case Revision::Unknown:
        break;
    }
    return "Unknown";
}

} // namespace waveshare_349
