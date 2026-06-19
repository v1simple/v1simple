#include "speed_source_selector.h"

#include "../gps/gps_runtime_status.h"

#ifndef UNIT_TEST
#include "../obd/obd_runtime_module.h"
#include "../gps/gps_runtime_module.h"
#endif
// In UNIT_TEST builds the test fixture provides minimal stubs for
// ObdRuntimeModule and GpsRuntimeModule before including this translation
// unit (see test/test_obd_speed_source/test_obd_speed_source.cpp).

void SpeedSourceSelector::begin(ObdRuntimeModule* obd, bool obdEnabled,
                                GpsRuntimeModule* gps, bool gpsEnabled) {
    obd_ = obd;
    gps_ = gps;
    syncEnabledInputs(obdEnabled, gpsEnabled);
    lastSource_ = SpeedSource::NONE;
    sourceSwitches_ = 0;
    obdSelections_ = 0;
    gpsSelections_ = 0;
    noSourceSelections_ = 0;
    cachedStatus_ = SpeedSelectorStatus{};
    cachedStatus_.obdEnabled = obdEnabled_;
    cachedStatus_.gpsEnabled = gpsEnabled_;
    selectedSpeed_ = SpeedSelection{};
}

void SpeedSourceSelector::syncEnabledInputs(bool obdEnabled, bool gpsEnabled) {
    obdEnabled_ = obdEnabled;
    gpsEnabled_ = gpsEnabled;
    cachedStatus_.obdEnabled = obdEnabled_;
    cachedStatus_.gpsEnabled = gpsEnabled_;
}

SpeedSelectorStatus SpeedSourceSelector::buildStatus(uint32_t nowMs) const {
    SpeedSelectorStatus status;
    status.obdEnabled = obdEnabled_;
    status.gpsEnabled = gpsEnabled_;

    // --- OBD (primary) ---
    float obdSpeed = 0.0f;
    uint32_t obdTs = 0;
    if (obdEnabled_ && obd_ && obd_->getFreshSpeed(nowMs, obdSpeed, obdTs) &&
        obdSpeed <= MAX_VALID_SPEED_MPH) {
        status.obdFresh = true;
        status.obdSpeedMph = obdSpeed;
        status.obdAgeMs = nowMs - obdTs;
    }

    // --- GPS (secondary) ---
    if (gpsEnabled_ && gps_) {
        float gpsSpeed = 0.0f;
        uint32_t gpsTs = 0;
        const bool freshSpeed = gps_->getFreshSpeed(nowMs, gpsSpeed, gpsTs);
        if (freshSpeed && gpsSpeed <= MAX_VALID_SPEED_MPH) {
            status.gpsFresh = true;
            status.gpsSpeedMph = gpsSpeed;
            status.gpsAgeMs = nowMs - gpsTs;
        }
        const GpsRuntimeStatus gpsStatus = gps_->snapshot(nowMs);
        status.gpsSatellites = gpsStatus.satellites;
        status.gpsHdop = gpsStatus.hdop;
        // "Good signal" gate: fresh speed sample, stable fix, enough satellites,
        // and HDOP within a reasonable bound. NaN HDOP fails the comparison.
        status.gpsGoodSignal = status.gpsFresh &&
                               gpsStatus.stableHasFix &&
                               gpsStatus.satellites >= GPS_MIN_SATELLITES &&
                               gpsStatus.hdop > 0.0f &&
                               gpsStatus.hdop <= GPS_MAX_HDOP;
    }

    // --- Priority: OBD beats GPS. GPS only used when OBD is not present. ---
    if (status.obdFresh) {
        status.selectedSource = SpeedSource::OBD;
        status.selectedSpeedMph = status.obdSpeedMph;
        status.selectedAgeMs = status.obdAgeMs;
    } else if (status.gpsGoodSignal) {
        status.selectedSource = SpeedSource::GPS;
        status.selectedSpeedMph = status.gpsSpeedMph;
        status.selectedAgeMs = status.gpsAgeMs;
    }

    status.sourceSwitches = sourceSwitches_;
    status.obdSelections = obdSelections_;
    status.gpsSelections = gpsSelections_;
    status.noSourceSelections = noSourceSelections_;
    return status;
}

void SpeedSourceSelector::update(uint32_t nowMs) {
    SpeedSelectorStatus next = buildStatus(nowMs);
    const SpeedSource picked = next.selectedSource;

    if (picked == SpeedSource::OBD) {
        obdSelections_++;
        selectedSpeed_.source = SpeedSource::OBD;
        selectedSpeed_.speedMph = next.selectedSpeedMph;
        selectedSpeed_.timestampMs = nowMs - next.selectedAgeMs;
        selectedSpeed_.ageMs = next.selectedAgeMs;
        selectedSpeed_.valid = true;
    } else if (picked == SpeedSource::GPS) {
        gpsSelections_++;
        selectedSpeed_.source = SpeedSource::GPS;
        selectedSpeed_.speedMph = next.selectedSpeedMph;
        selectedSpeed_.timestampMs = nowMs - next.selectedAgeMs;
        selectedSpeed_.ageMs = next.selectedAgeMs;
        selectedSpeed_.valid = true;
    } else {
        noSourceSelections_++;
        selectedSpeed_ = SpeedSelection{};
    }

    if (picked != lastSource_ && lastSource_ != SpeedSource::NONE) {
        sourceSwitches_++;
    }
    lastSource_ = picked;

    next.sourceSwitches = sourceSwitches_;
    next.obdSelections = obdSelections_;
    next.gpsSelections = gpsSelections_;
    next.noSourceSelections = noSourceSelections_;
    cachedStatus_ = next;
}

SpeedSelectorStatus SpeedSourceSelector::snapshot() const {
    return cachedStatus_;
}

SpeedSelectorStatus SpeedSourceSelector::snapshotAt(uint32_t nowMs) const {
    return buildStatus(nowMs);
}

const char* SpeedSourceSelector::sourceName(SpeedSource source) {
    switch (source) {
        case SpeedSource::OBD: return "obd";
        case SpeedSource::GPS: return "gps";
        case SpeedSource::NONE:
        default:
            return "none";
    }
}
