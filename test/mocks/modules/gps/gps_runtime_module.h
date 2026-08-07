#pragma once
#ifndef GPS_RUNTIME_MODULE_H
#define GPS_RUNTIME_MODULE_H

// Use the canonical struct definition — never redefine it here.
// This ensures the mock and real module always agree on the shape of GpsRuntimeStatus.
#include "../../../../src/modules/gps/gps_runtime_status.h"

/**
 * Mock GpsRuntimeModule for native unit testing.
 * Only the class behavior is mocked; GpsRuntimeStatus is the real struct.
 */
class GpsRuntimeModule {
public:
    int snapshotCalls = 0;
    GpsRuntimeStatus nextSnapshot{};

    bool freshSpeedResult = false;
    float freshSpeedMph = 0.0f;
    uint32_t freshSpeedTsMs = 0;

    GpsRuntimeStatus snapshot(uint32_t /*nowMs*/) {
        snapshotCalls++;
        return nextSnapshot;
    }

    bool getFreshSpeed(uint32_t /*nowMs*/, float& speedMphOut, uint32_t& tsMsOut) const {
        if (!freshSpeedResult) return false;
        speedMphOut = freshSpeedMph;
        tsMsOut = freshSpeedTsMs;
        return true;
    }
};

#endif // GPS_RUNTIME_MODULE_H
