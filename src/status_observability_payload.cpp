#include "status_observability_payload.h"

namespace StatusObservabilityPayload {

namespace {

JsonObject ensureObject(JsonObject root, const char* key) {
    JsonObject obj = root[key].as<JsonObject>();
    if (obj.isNull()) {
        obj = root[key].to<JsonObject>();
    }
    return obj;
}

} // namespace

void appendStatusObservability(JsonObject root, const WifiStatusSnapshot& wifiSnapshot) {
    JsonObject wifi = ensureObject(root, "wifi");
    wifi["ap_last_transition_reason_code"] = wifiSnapshot.apLastTransitionReasonCode;
    wifi["ap_last_transition_reason"] = wifiSnapshot.apLastTransitionReason;
    wifi["low_dma_cooldown_ms"] = wifiSnapshot.lowDmaCooldownRemainingMs;
}

} // namespace StatusObservabilityPayload
