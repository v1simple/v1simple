#include "render_frame_composer.h"

#include "display_layout.h"
#include "modules/alp/alp_runtime_module.h"

namespace {

bool isRenderableAlert(const AlertData& alert) {
    return alert.isValid && alert.band != BAND_NONE && (alert.band == BAND_LASER || alert.frequency != 0);
}

bool alertsEquivalent(const AlertData& lhs, const AlertData& rhs) {
    return lhs.isValid == rhs.isValid && lhs.band == rhs.band && lhs.direction == rhs.direction &&
           lhs.frontStrength == rhs.frontStrength && lhs.rearStrength == rhs.rearStrength &&
           lhs.frequency == rhs.frequency && lhs.isPriority == rhs.isPriority;
}

Direction alpDirectionToV1Direction(AlpLaserDirection direction) {
    switch (direction) {
    case AlpLaserDirection::FRONT:
        return DIR_FRONT;

    case AlpLaserDirection::REAR:
        return DIR_REAR;

    case AlpLaserDirection::UNKNOWN:
    default:
        return DIR_NONE;
    }
}

DisplayState synthesizeAlpPrimaryState(const DisplayState& base, const AlpLaserEvent& event) {
    DisplayState state = base;
    const Direction v1Direction = alpDirectionToV1Direction(event.direction);
    state.activeBands = BAND_LASER;
    state.arrows = v1Direction;
    state.priorityArrow = v1Direction;
    state.signalBars = DisplayLayout::MAIN_SIGNAL_BAR_COUNT; // Laser = full physical meter
    state.flashBits = 0;
    state.bandFlashBits = 0;
    // Valentine's Law (docs/VALENTINE_PHILOSOPHY.md, Part II corollary): a live
    // laser event renders at full urgency, always.
    //
    // ALP owns both laser detection and its speaker, so the V1 mute state does
    // not apply to an ALP-owned frame. Normalizing muted here — at the composer,
    // not per-renderer — is deliberate: it makes the full-urgency frame the only
    // frame downstream consumers can see, so the status strip, frequency, arrows,
    // and bands cannot disagree about it and a later refactor cannot reintroduce
    // mute on the laser path by touching one renderer.
    state.muted = false;
    return state;
}

void appendV1Card(RenderFrame& frame, const AlertData& alert) {
    if (frame.cardCount >= static_cast<int>(RenderFrame::MAX_CARDS)) {
        return;
    }

    RenderFrameCard& card = frame.cards[frame.cardCount++];
    card = RenderFrameCard{};
    card.kind = RenderFrameCard::Kind::V1;
    card.v1Alert = alert;
}

AlertData firstRenderableFilteredAlert(const V1Snapshot& v1, bool suppressLaser) {
    if (!v1.alerts || v1.alertCount <= 0) {
        return AlertData{};
    }

    for (int index = 0; index < v1.alertCount; ++index) {
        const AlertData& alert = v1.alerts[index];
        if (suppressLaser && alert.band == BAND_LASER) {
            continue;
        }
        if (isRenderableAlert(alert)) {
            return alert;
        }
    }

    return AlertData{};
}

void appendFilteredV1Alerts(RenderFrame& frame, const V1Snapshot& v1, bool suppressLaser,
                            const AlertData* skipPriority) {
    if (!v1.alerts || v1.alertCount <= 0) {
        return;
    }

    bool skippedPriority = false;
    for (int index = 0; index < v1.alertCount; ++index) {
        const AlertData& alert = v1.alerts[index];
        if (!alert.isValid || alert.band == BAND_NONE) {
            continue;
        }
        if (suppressLaser && alert.band == BAND_LASER) {
            continue;
        }
        if (skipPriority && !skippedPriority && alertsEquivalent(alert, *skipPriority)) {
            skippedPriority = true;
            continue;
        }
        appendV1Card(frame, alert);
    }
}

} // namespace

RenderFrame RenderFrameComposer::compose(const V1Snapshot& v1, const AlpSnapshot& alp, const V1Settings& /*settings*/,
                                         uint32_t /*nowMs*/) const {
    RenderFrame frame;
    frame.context = v1.state;

    const bool hasAlpLive = alp.ownsLaserDisplay && alp.event.active;
    const bool hasAlpPersisted = alp.isPersistedLatch && alp.latchedEvent.active;
    // Valentine's Law (docs/VALENTINE_PHILOSOPHY.md, Part II corollary): the V1's
    // laser channel is suppressed only when an ALP branch below actually renders
    // one. ALP is the better laser instrument and outranks the V1 whenever it has
    // something to show, but a connected ALP with nothing on screen is not a
    // reason to drop the V1's own detection -- that would leave a live laser
    // rendered by neither source. Keying this to the render decision rather than
    // to connection state makes that state unreachable by construction.
    // Pinned by test_render_frame_composer_v1_laser_survives_silent_alp.
    const bool suppressV1Laser = hasAlpLive || hasAlpPersisted;
    const AlertData filteredPriority =
        (v1.hasRenderablePriority && (!suppressV1Laser || v1.priority.band != BAND_LASER))
            ? v1.priority
            : firstRenderableFilteredAlert(v1, suppressV1Laser);
    const bool hasFilteredPriority = isRenderableAlert(filteredPriority);

    if (hasAlpLive || hasAlpPersisted) {
        frame.primaryKind = hasAlpLive ? RenderFramePrimaryKind::ALP_LIVE : RenderFramePrimaryKind::ALP_PERSISTED;
        frame.alpPrimary = hasAlpLive ? alp.event : alp.latchedEvent;
        frame.primaryState = synthesizeAlpPrimaryState(v1.state, frame.alpPrimary);
        appendFilteredV1Alerts(frame, v1, suppressV1Laser, nullptr);
        return frame;
    }

    if (hasFilteredPriority) {
        frame.primaryKind = RenderFramePrimaryKind::V1_LIVE;
        frame.v1Priority = filteredPriority;
        // The simple signal bars are the parsed InfDisplayData LED bitmap on
        // V1Simple's six-slot meter. Do not recompute
        // them from alert-row RSSI; those per-alert strengths drive secondary
        // alert context only.
        frame.primaryState = v1.state;
        appendFilteredV1Alerts(frame, v1, suppressV1Laser, &filteredPriority);
        return frame;
    }

    if (v1.hasPersistedAlert) {
        frame.primaryKind = RenderFramePrimaryKind::V1_PERSISTED;
        frame.v1Priority = v1.persistedAlert;
        frame.primaryState = v1.state;
        return frame;
    }

    frame.primaryKind = RenderFramePrimaryKind::IDLE;
    frame.primaryState = v1.state;
    return frame;
}
