#include "alp_event_latch.h"

void AlpEventLatch::setEvent(const AlpLaserEvent& ev) {
    if (!ev.active) {
        return;
    }

    latched_ = ev;
    latched_.active = true;
    persistStartMs_ = 0;
    persisting_ = false;
}

void AlpEventLatch::startPersistence(uint32_t nowMs) {
    if (!latched_.active || persisting_) {
        return;
    }

    persistStartMs_ = nowMs;
    persisting_ = true;
}

bool AlpEventLatch::shouldShowPersisted(uint32_t nowMs, uint32_t windowMs) const {
    return latched_.active && persisting_ && windowMs > 0 && (nowMs - persistStartMs_) < windowMs;
}

void AlpEventLatch::clearLatch() {
    latched_ = AlpLaserEvent{};
    persistStartMs_ = 0;
    persisting_ = false;
}