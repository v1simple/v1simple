#include "wifi_sta_slot_policy.h"

namespace WifiStaSlotPolicy {

namespace {

bool slotComesBefore(const WifiStaSlot& lhs,
                     size_t lhsIndex,
                     const WifiStaSlot& rhs,
                     size_t rhsIndex) {
    if (lhs.priority != rhs.priority) {
        return lhs.priority < rhs.priority;
    }
    if (lhs.lastConnectedAtSec != rhs.lastConnectedAtSec) {
        return lhs.lastConnectedAtSec > rhs.lastConnectedAtSec;
    }
    return lhsIndex < rhsIndex;
}

}  // namespace

size_t orderConfiguredSlots(const V1Settings& settings,
                            size_t* indicesOut,
                            size_t maxIndices) {
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
            if (!slotComesBefore(settings.wifiStaSlots[i],
                                 i,
                                 settings.wifiStaSlots[previousIndex],
                                 previousIndex)) {
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

bool scanContainsSsid(const String* scannedSsids,
                      size_t scannedCount,
                      const String& ssid) {
    if (!scannedSsids || ssid.length() == 0) {
        return false;
    }
    for (size_t i = 0; i < scannedCount; ++i) {
        if (scannedSsids[i] == ssid) {
            return true;
        }
    }
    return false;
}

size_t selectInRangeSlots(const V1Settings& settings,
                          const String* scannedSsids,
                          size_t scannedCount,
                          size_t* indicesOut,
                          size_t maxIndices) {
    if (!indicesOut || maxIndices == 0) {
        return 0;
    }

    size_t ordered[kWifiStaSlotCount] = {};
    const size_t orderedCount = orderConfiguredSlots(settings, ordered, kWifiStaSlotCount);
    size_t count = 0;
    for (size_t i = 0; i < orderedCount && count < maxIndices; ++i) {
        const size_t slotIndex = ordered[i];
        if (scanContainsSsid(scannedSsids,
                             scannedCount,
                             settings.wifiStaSlots[slotIndex].ssid)) {
            indicesOut[count++] = slotIndex;
        }
    }
    return count;
}

}  // namespace WifiStaSlotPolicy
