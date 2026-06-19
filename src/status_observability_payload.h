#pragma once

#include <ArduinoJson.h>

#include "modules/wifi/wifi_auto_start_module.h"

namespace StatusObservabilityPayload {

struct WifiStatusSnapshot {
    uint32_t apLastTransitionReasonCode = 0;
    const char* apLastTransitionReason = "unknown";
    uint32_t lowDmaCooldownRemainingMs = 0;
    WifiAutoStartDecisionSnapshot autoStart;
};

void appendStatusObservability(JsonObject root,
                               const WifiStatusSnapshot& wifi);

}  // namespace StatusObservabilityPayload
