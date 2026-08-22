#pragma once

#include <stdint.h>

#ifdef UNIT_TEST
#include <Arduino.h>
#endif

// One monotonic clock domain and one reboot-ephemeral segment identify every
// qualification timestamp emitted by a firmware boot. The segment is not a
// persistent device identifier and must never be used as one.
namespace QualificationClock {

#ifdef UNIT_TEST
inline void initialize() {}

inline uint64_t nowMicros() {
    return 1;
}

inline uint64_t segment() {
    return 1;
}

#else

void initialize();
uint64_t nowMicros();
uint64_t segment();

#endif

} // namespace QualificationClock
