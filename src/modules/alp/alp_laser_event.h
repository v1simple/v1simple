/**
 * ALP Laser Event — atomic event snapshot for display propagation.
 *
 * Consolidates the event-owned fields that the display pipeline
 * reads from AlpRuntimeModule and writes to V1Display in a single,
 * typed struct. This ensures atomic propagation and eliminates the
 * three-flag dance.
 */

#pragma once

#include <cstdint>

enum class AlpGunType : uint8_t;
enum class AlpLaserDirection : uint8_t;

struct AlpLaserEvent {
    bool active = false;
    AlpGunType gun = static_cast<AlpGunType>(0);
    AlpLaserDirection direction = static_cast<AlpLaserDirection>(0);
    bool lidActive = false;
    uint32_t openedAtMs = 0;
    uint32_t closedAtMs = 0;
    // Changes only when a fresh engagement opens; unlike millis(), this also
    // distinguishes two sessions opened during the same clock tick.
    uint32_t sessionGeneration = 0;
};
