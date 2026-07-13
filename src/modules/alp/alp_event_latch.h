/**
 * AlpEventLatch - ALP laser event persistence for the display pipeline.
 *
 * Mirrors the V1 alert persistence behavior, but keeps the ALP event in its
 * live-shaped form so the display can continue rendering the same laser frame
 * through brief raw-event gaps and for the shared post-close persist window.
 */

#pragma once

#include <cstdint>

#include "alp_laser_event.h"

class AlpEventLatch {
public:
    void setEvent(const AlpLaserEvent& ev);
    void startPersistence(uint32_t nowMs);
    bool shouldShowPersisted(uint32_t nowMs, uint32_t windowMs) const;
    void clearLatch();

    const AlpLaserEvent& latchedEvent() const { return latched_; }
    bool isLatched() const { return latched_.active; }

private:
    AlpLaserEvent latched_{};
    uint32_t persistStartMs_ = 0;
    bool persisting_ = false;
};