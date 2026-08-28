#pragma once

#include <cstdint>

namespace WifiMaintenanceLinkPolicy {

enum class Decision : uint8_t {
    None = 0,
    RejectSuppressedPhysicalConnection,
    ReconcilePhysicalConnection,
    StartCandidateScan,
};

struct Input {
    bool physicalConnected = false;
    bool appConnected = false;
    bool appConnecting = false;
    bool autoConnectActive = false;
    bool clientEnabled = false;
    bool hasSavedCandidates = false;
    bool autoJoinSuppressed = false;
    uint32_t nowMs = 0;
    uint32_t retryAtMs = 0;
};

inline bool deadlineReached(const uint32_t nowMs, const uint32_t deadlineMs) {
    return deadlineMs != 0 && static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

inline bool shouldDisconnectAddressCollision(const bool physicalConnected, const bool addressCollision) {
    return physicalConnected && addressCollision;
}

inline Decision evaluate(const Input& input) {
    if (input.autoJoinSuppressed) {
        return input.physicalConnected ? Decision::RejectSuppressedPhysicalConnection : Decision::None;
    }
    if (input.physicalConnected && !input.appConnected) {
        return Decision::ReconcilePhysicalConnection;
    }
    if (!input.physicalConnected && !input.appConnected && !input.appConnecting && !input.autoConnectActive &&
        input.clientEnabled && input.hasSavedCandidates && deadlineReached(input.nowMs, input.retryAtMs)) {
        return Decision::StartCandidateScan;
    }
    return Decision::None;
}

} // namespace WifiMaintenanceLinkPolicy
