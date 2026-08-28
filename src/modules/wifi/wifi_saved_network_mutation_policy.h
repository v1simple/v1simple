#pragma once

#include <cstdint>

namespace WifiSavedNetworkMutationPolicy {

struct Input {
    bool persistenceSucceeded = false;
    bool allSlots = false;
    int mutationSlotIndex = -1;
    int currentSlotIndex = -1;
    int pendingSlotIndex = -1;
    bool maintenanceScanActive = false;
    bool maintenanceConnectActive = false;
};

struct Decision {
    bool cancelMaintenanceAutoActivity = false;
    bool disconnectTrackedActivity = false;
    bool scheduleReplacementScan = false;
};

inline Decision evaluate(const Input& input) {
    if (!input.persistenceSucceeded) {
        return {};
    }
    const bool tracked = input.allSlots ||
                         (input.mutationSlotIndex >= 0 &&
                          (input.currentSlotIndex == input.mutationSlotIndex ||
                           input.pendingSlotIndex == input.mutationSlotIndex));
    const bool cancelScan = input.maintenanceScanActive;
    const bool cancelConnect = input.maintenanceConnectActive && tracked;
    const bool cancelActivity = cancelScan || cancelConnect;
    return {cancelActivity, tracked, cancelActivity};
}

} // namespace WifiSavedNetworkMutationPolicy
