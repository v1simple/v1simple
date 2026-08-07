#pragma once

#include <Arduino.h>

class ObdRuntimeModule;
class GpsRuntimeModule;

enum class SpeedSource : uint8_t { NONE = 0, GPS = 1, OBD = 3 };

struct SpeedSelection {
    SpeedSource source = SpeedSource::NONE;
    float speedMph = 0.0f;
    uint32_t timestampMs = 0;
    uint32_t ageMs = UINT32_MAX;
    bool valid = false;
};

struct SpeedSelectorStatus {
    bool obdEnabled = false;
    bool gpsEnabled = false;
    SpeedSource selectedSource = SpeedSource::NONE;
    float selectedSpeedMph = 0.0f;
    uint32_t selectedAgeMs = UINT32_MAX;

    bool obdFresh = false;
    float obdSpeedMph = 0.0f;
    uint32_t obdAgeMs = UINT32_MAX;

    bool gpsFresh = false;      // Fresh sample with valid speed
    bool gpsGoodSignal = false; // Fresh + stable fix + sats/HDOP thresholds
    float gpsSpeedMph = 0.0f;
    uint32_t gpsAgeMs = UINT32_MAX;
    uint8_t gpsSatellites = 0;
    float gpsHdop = 0.0f;

    uint32_t sourceSwitches = 0;
    uint32_t obdSelections = 0;
    uint32_t gpsSelections = 0;
    uint32_t noSourceSelections = 0;
};

class SpeedSourceSelector {
  public:
    static constexpr float MAX_VALID_SPEED_MPH = 250.0f;
    // GPS "good signal" thresholds — secondary source quality gate.
    static constexpr uint8_t GPS_MIN_SATELLITES = 4;
    static constexpr float GPS_MAX_HDOP = 5.0f;

    void begin(ObdRuntimeModule* obd, bool obdEnabled, GpsRuntimeModule* gps = nullptr, bool gpsEnabled = false);
    void syncEnabledInputs(bool obdEnabled, bool gpsEnabled = false);
    void update(uint32_t nowMs);

    // Producers call update(nowMs) once per loop to commit state.
    // Consumers read snapshot() for the committed state or snapshotAt(nowMs)
    // for a pure point-in-time view that does not mutate counters/state.
    SpeedSelectorStatus snapshot() const;
    SpeedSelectorStatus snapshotAt(uint32_t nowMs) const;
    SpeedSelection selectedSpeed() const { return selectedSpeed_; }

    static const char* sourceName(SpeedSource source);

  private:
    SpeedSelectorStatus buildStatus(uint32_t nowMs) const;

    bool obdEnabled_ = false;
    bool gpsEnabled_ = false;

    ObdRuntimeModule* obd_ = nullptr;
    GpsRuntimeModule* gps_ = nullptr;

    SpeedSource lastSource_ = SpeedSource::NONE;
    uint32_t sourceSwitches_ = 0;
    uint32_t obdSelections_ = 0;
    uint32_t gpsSelections_ = 0;
    uint32_t noSourceSelections_ = 0;
    SpeedSelectorStatus cachedStatus_ = {};
    SpeedSelection selectedSpeed_ = {};
};
