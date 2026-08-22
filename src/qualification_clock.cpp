#include "qualification_clock.h"

#ifndef UNIT_TEST

#include <esp_random.h>
#include <esp_timer.h>

namespace {

uint64_t clockSegment = 0;

} // namespace

namespace QualificationClock {

uint64_t nowMicros() {
    return static_cast<uint64_t>(esp_timer_get_time());
}

void initialize() {
    if (clockSegment != 0) {
        return;
    }

    const uint64_t randomBits = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
    clockSegment = randomBits ^ nowMicros();
    if (clockSegment == 0) {
        clockSegment = 1;
    }
}

uint64_t segment() {
    return clockSegment;
}

} // namespace QualificationClock

#endif
