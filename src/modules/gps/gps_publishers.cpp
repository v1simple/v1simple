#include "gps_publishers.h"

// ─────────────────────────────────────────────────────────────────────────────
// Global instances
// ─────────────────────────────────────────────────────────────────────────────

GpsTimePublisher gpsTimePublisher;
GpsGeoPublisher gpsGeoPublisher;

// ─────────────────────────────────────────────────────────────────────────────
// GpsTimePublisher locking
// ─────────────────────────────────────────────────────────────────────────────

void GpsTimePublisher::lock() const {
#ifdef UNIT_TEST
    while (lockFlag_.test_and_set(std::memory_order_acquire)) {
    }
#else
    portENTER_CRITICAL(&lockMux_);
#endif
}

void GpsTimePublisher::unlock() const {
#ifdef UNIT_TEST
    lockFlag_.clear(std::memory_order_release);
#else
    portEXIT_CRITICAL(&lockMux_);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// GpsTimePublisher implementation
// ─────────────────────────────────────────────────────────────────────────────

void GpsTimePublisher::publish(const GpsTimeSnapshot& s) {
    lock();
    snapshot_ = s;
    unlock();
}

GpsTimeSnapshot GpsTimePublisher::read(uint32_t nowMs) const {
    lock();
    const GpsTimeSnapshot s = snapshot_;
    unlock();

    if (!s.valid || s.capturedMs == 0) {
        return GpsTimeSnapshot{};
    }
    const uint32_t age = static_cast<uint32_t>(nowMs - s.capturedMs);
    if (age > kStaleMs) {
        return GpsTimeSnapshot{};
    }
    return s;
}

bool GpsTimePublisher::readUtc(uint32_t nowMs, uint64_t& utcEpochMsOut) const {
    const GpsTimeSnapshot s = read(nowMs);
    if (!s.valid) {
        return false;
    }
    const uint32_t elapsed = static_cast<uint32_t>(nowMs - s.capturedMs);
    utcEpochMsOut = s.utcEpochMs + static_cast<uint64_t>(elapsed);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// GpsGeoPublisher locking
// ─────────────────────────────────────────────────────────────────────────────

void GpsGeoPublisher::lock() const {
#ifdef UNIT_TEST
    while (lockFlag_.test_and_set(std::memory_order_acquire)) {
    }
#else
    portENTER_CRITICAL(&lockMux_);
#endif
}

void GpsGeoPublisher::unlock() const {
#ifdef UNIT_TEST
    lockFlag_.clear(std::memory_order_release);
#else
    portEXIT_CRITICAL(&lockMux_);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// GpsGeoPublisher implementation
// ─────────────────────────────────────────────────────────────────────────────

void GpsGeoPublisher::publish(const GpsGeoSnapshot& s) {
    lock();
    snapshot_ = s;
    unlock();
}

GpsGeoSnapshot GpsGeoPublisher::read(uint32_t nowMs) const {
    lock();
    const GpsGeoSnapshot s = snapshot_;
    unlock();

    if (!s.valid || s.capturedMs == 0) {
        return GpsGeoSnapshot{};
    }
    const uint32_t age = static_cast<uint32_t>(nowMs - s.capturedMs);
    if (age > kStaleMs) {
        return GpsGeoSnapshot{};
    }
    return s;
}

bool GpsGeoPublisher::fresh(uint32_t nowMs) const {
    const GpsGeoSnapshot s = read(nowMs);
    return s.valid;
}
