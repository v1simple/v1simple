#include "render_frame_composer.h"

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
    state.signalBars = 8; // Laser = full local 8-slot meter
    state.flashBits = 0;
    state.bandFlashBits = 0;
    // ALP is the authoritative source for laser alerts — it owns both the
    // detection and its own speaker. The V1's `state.muted` bit reflects V1
    // audio state (SAVVY/OBD may have driven V1 main volume to 0 to quiet
    // false-prone K/X chatter), which has no bearing on whether the driver
    // is actually hearing this laser alert: the ALP unit's own speaker is
    // firing regardless. Surfacing "muted" on an ALP-owned primary is
    // misinformation, and flapping the bit as the V1's display-packet
    // hysteresis ticks produces the visible flash the driver sees as the
    // mute icon blinking during a sustained ALP engagement.
    //
    // Valentine philosophy enforcement:
    //   - "Render laser alerts with the same urgency tier as Ka —
    //      immediately, prominently."
    //   - "The only thing worse than detecting a false signal is failing
    //      to detect real radar … every design decision — display priority,
    //      muting behavior, alert persistence — passes through this filter
    //      first."
    // A muted render on a live laser event silently downgrades urgency at
    // the exact moment Valentine's Law says we must not. We enforce the
    // unmute at the composer so every downstream consumer (status strip,
    // frequency, arrows, bands) renders the same unmuted frame and a
    // future refactor can't accidentally reintroduce mute on the ALP path.
    // Do NOT "fix" this to honor state.muted — the misread fix is to trace
    // why state.muted leaked into this branch upstream, not to respect it
    // here.
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
    const bool suppressV1Laser = alp.ownsLaserDisplay || hasAlpPersisted;
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
        // The simple signal bars are the parsed InfDisplayData LED bitmap
        // expanded onto this display's local 8-slot meter. Do not recompute
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
