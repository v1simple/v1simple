/**
 * GPS Publishers — single-writer, many-reader atomic snapshots for GPS data.
 *
 * GpsTimePublisher: UTC wall-clock time from GPS RMC sentences.
 *   - Written by GpsRuntimeModule once every GPS_TIME_UPDATE_INTERVAL_MS (~5 min).
 *   - Read by perf CSV logger and ALP CSV logger.
 *
 * GpsGeoPublisher: position/course/speed from GPS fixes.
 *   - Written by GpsRuntimeModule on each valid NMEA fix.
 *   - Phase 1: no consumers. Phase 2: heading-aware features.
 */

#pragma once

#include <cstdint>
#include <cmath>

#ifdef UNIT_TEST
#include <atomic>
#else
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Snapshot structs
// ─────────────────────────────────────────────────────────────────────────────

struct GpsTimeSnapshot {
    bool     valid         = false;
    uint32_t capturedMs    = 0;      // millis() at publish
    uint64_t utcEpochMs    = 0;      // ms since Unix epoch (UTC)
    uint8_t  source        = 0;      // 1 = RMC
};

struct GpsGeoSnapshot {
    bool     valid         = false;
    uint32_t capturedMs    = 0;
    float    latitudeDeg   = NAN;
    float    longitudeDeg  = NAN;
    bool     courseValid   = false;
    float    courseDeg     = NAN;    // 0..360, true north
    bool     speedValid    = false;
    float    speedMph      = 0.0f;
    uint8_t  satellites    = 0;
    float    hdop          = NAN;
    bool     hasFix        = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// GpsTimePublisher
// ─────────────────────────────────────────────────────────────────────────────

class GpsTimePublisher {
public:
    // Staleness threshold: UTC stale after this many ms with no re-publish.
    static constexpr uint32_t kStaleMs = 30000;  // 30 s

    // Called by GpsRuntimeModule to publish a new UTC time.
    void publish(const GpsTimeSnapshot& s);

    // Read latest snapshot, extrapolating UTC from capturedMs if still fresh.
    // Returns snapshot with valid=false if stale or never set.
    GpsTimeSnapshot read(uint32_t nowMs) const;

    // Convenience: populate utcEpochMsOut with estimated current UTC.
    // Returns true if the publisher has a valid, fresh snapshot.
    bool readUtc(uint32_t nowMs, uint64_t& utcEpochMsOut) const;

private:
    void lock() const;
    void unlock() const;

    GpsTimeSnapshot snapshot_{};
#ifdef UNIT_TEST
    mutable std::atomic_flag lockFlag_ = ATOMIC_FLAG_INIT;
#else
    mutable portMUX_TYPE lockMux_ = portMUX_INITIALIZER_UNLOCKED;
#endif
};

// ─────────────────────────────────────────────────────────────────────────────
// GpsGeoPublisher
// ─────────────────────────────────────────────────────────────────────────────

class GpsGeoPublisher {
public:
    // Staleness threshold: geo stale after this many ms with no re-publish.
    static constexpr uint32_t kStaleMs = 5000;  // 5 s

    // Called by GpsRuntimeModule to publish a new geo snapshot.
    void publish(const GpsGeoSnapshot& s);

    // Read latest snapshot; returns empty snapshot (valid=false) if stale.
    GpsGeoSnapshot read(uint32_t nowMs) const;

    // Returns true if the most recent snapshot is fresh.
    bool fresh(uint32_t nowMs) const;

private:
    void lock() const;
    void unlock() const;

    GpsGeoSnapshot snapshot_{};
#ifdef UNIT_TEST
    mutable std::atomic_flag lockFlag_ = ATOMIC_FLAG_INIT;
#else
    mutable portMUX_TYPE lockMux_ = portMUX_INITIALIZER_UNLOCKED;
#endif
};

// ─────────────────────────────────────────────────────────────────────────────
// Global instances (defined in gps_publishers.cpp)
// ─────────────────────────────────────────────────────────────────────────────

extern GpsTimePublisher gpsTimePublisher;
extern GpsGeoPublisher  gpsGeoPublisher;
