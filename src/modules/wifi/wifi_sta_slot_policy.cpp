#include "wifi_sta_slot_policy.h"

namespace WifiStaSlotPolicy {

namespace {

bool slotComesBefore(const WifiStaSlot& lhs, size_t lhsIndex, const WifiStaSlot& rhs, size_t rhsIndex) {
    if (lhs.priority != rhs.priority) {
        return lhs.priority < rhs.priority;
    }
    if (lhs.lastConnectedAtSec != rhs.lastConnectedAtSec) {
        return lhs.lastConnectedAtSec > rhs.lastConnectedAtSec;
    }
    return lhsIndex < rhsIndex;
}

} // namespace

size_t orderConfiguredSlots(const V1Settings& settings, size_t* indicesOut, size_t maxIndices) {
    if (!indicesOut || maxIndices == 0) {
        return 0;
    }

    size_t count = 0;
    for (size_t i = 0; i < kWifiStaSlotCount && count < maxIndices; ++i) {
        if (!settings.wifiStaSlots[i].isConfigured()) {
            continue;
        }

        size_t insertAt = count;
        while (insertAt > 0) {
            const size_t previousIndex = indicesOut[insertAt - 1];
            if (!slotComesBefore(settings.wifiStaSlots[i], i, settings.wifiStaSlots[previousIndex], previousIndex)) {
                break;
            }
            indicesOut[insertAt] = indicesOut[insertAt - 1];
            --insertAt;
        }
        indicesOut[insertAt] = i;
        ++count;
    }
    return count;
}

} // namespace WifiStaSlotPolicy
