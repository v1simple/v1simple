#pragma once

#include <ArduinoJson.h>

namespace StatusObservabilityPayload {

struct WifiStatusSnapshot {
    uint32_t apLastTransitionReasonCode = 0;
    const char* apLastTransitionReason = "unknown";
    uint32_t lowDmaCooldownRemainingMs = 0;
};

void appendStatusObservability(JsonObject root, const WifiStatusSnapshot& wifi);

} // namespace StatusObservabilityPayload
