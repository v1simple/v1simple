#include "gps_observation_log.h"

#include <algorithm>

GpsObservationLog gpsObservationLog;

void GpsObservationLog::lock() const {
#ifdef UNIT_TEST
    while (lockFlag_.test_and_set(std::memory_order_acquire)) {
    }
#else
    portENTER_CRITICAL(&lockMux_);
#endif
}

void GpsObservationLog::unlock() const {
#ifdef UNIT_TEST
    lockFlag_.clear(std::memory_order_release);
#else
    portEXIT_CRITICAL(&lockMux_);
#endif
}

void GpsObservationLog::reset() {
    LockGuard guard(*this);
    head_ = 0;
    tail_ = 0;
    count_ = 0;
    published_ = 0;
    drops_ = 0;
}

bool GpsObservationLog::publish(const GpsObservation& observation) {
    LockGuard guard(*this);
    bool dropped = false;

    if (count_ == kCapacity) {
        tail_ = nextIndex(tail_);
        count_--;
        drops_++;
        dropped = true;
    }

    ring_[head_] = observation;
    head_ = nextIndex(head_);
    count_++;
    published_++;
    return !dropped;
}

size_t GpsObservationLog::copyRecent(GpsObservation* out, size_t maxCount) const {
    LockGuard guard(*this);
    if (!out || maxCount == 0 || count_ == 0) {
        return 0;
    }

    const size_t copyCount = std::min<size_t>(maxCount, count_);
    uint8_t idx = head_;
    for (size_t i = 0; i < copyCount; ++i) {
        idx = prevIndex(idx);
        out[i] = ring_[idx];
    }
    return copyCount;
}

GpsObservationLogStats GpsObservationLog::stats() const {
    LockGuard guard(*this);
    GpsObservationLogStats out;
    out.published = published_;
    out.drops = drops_;
    out.size = count_;
    return out;
}
