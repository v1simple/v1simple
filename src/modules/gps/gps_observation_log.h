#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#ifdef UNIT_TEST
#include <atomic>
#else
#include <freertos/FreeRTOS.h>
#endif

struct GpsObservation {
    uint32_t tsMs = 0;
    bool hasFix = false;
    bool speedValid = false;
    float speedMph = 0.0f;
    uint8_t satellites = 0;
    float hdop = NAN;
    bool locationValid = false;
    float latitudeDeg = NAN;
    float longitudeDeg = NAN;
};

struct GpsObservationLogStats {
    uint32_t published = 0;
    uint32_t drops = 0;
    size_t size = 0;
};

class GpsObservationLog {
public:
    static constexpr size_t kCapacity = 64;

    void reset();
    bool publish(const GpsObservation& observation);
    size_t copyRecent(GpsObservation* out, size_t maxCount) const;
    GpsObservationLogStats stats() const;

private:
    void lock() const;
    void unlock() const;

    struct LockGuard {
        explicit LockGuard(const GpsObservationLog& ownerRef) : owner(ownerRef) {
            owner.lock();
        }
        ~LockGuard() {
            owner.unlock();
        }
        const GpsObservationLog& owner;
    };

    static uint8_t nextIndex(uint8_t idx) {
        return static_cast<uint8_t>((idx + 1u) % kCapacity);
    }
    static uint8_t prevIndex(uint8_t idx) {
        return static_cast<uint8_t>((idx + kCapacity - 1u) % kCapacity);
    }

    GpsObservation ring_[kCapacity] = {};
    uint8_t head_ = 0;
    uint8_t tail_ = 0;
    uint8_t count_ = 0;
    uint32_t published_ = 0;
    uint32_t drops_ = 0;
#ifdef UNIT_TEST
    mutable std::atomic_flag lockFlag_ = ATOMIC_FLAG_INIT;
#else
    mutable portMUX_TYPE lockMux_ = portMUX_INITIALIZER_UNLOCKED;
#endif
};
