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
    // Valentine's Law: ALP laser renders at full urgency, independent of V1
    // mute. Normalize here so all downstream indicators agree.
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
    // Valentine's Law: ALP_LIVE > V1_LIVE > ALP_PERSISTED > V1_PERSISTED > IDLE.
    // Suppress V1 laser only when ALP actually renders it; connection alone
    // cannot hide a live V1 detection. Live V1 also outranks an ALP tail.
    const bool suppressV1Laser = hasAlpLive;
    const AlertData filteredPriority =
        (v1.hasRenderablePriority && (!suppressV1Laser || v1.priority.band != BAND_LASER))
            ? v1.priority
            : firstRenderableFilteredAlert(v1, suppressV1Laser);
    const bool hasFilteredPriority = isRenderableAlert(filteredPriority);

    if (hasAlpLive) {
        frame.primaryKind = RenderFramePrimaryKind::ALP_LIVE;
        frame.alpPrimary = alp.event;
        frame.primaryState = synthesizeAlpPrimaryState(v1.state, frame.alpPrimary);
        appendFilteredV1Alerts(frame, v1, /*suppressLaser=*/true, nullptr);
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
        if (filteredPriority.band == BAND_LASER) {
            frame.primaryState.priorityArrow = filteredPriority.direction;
        }
        appendFilteredV1Alerts(frame, v1, /*suppressLaser=*/false, &filteredPriority);
        return frame;
    }

    if (hasAlpPersisted) {
        frame.primaryKind = RenderFramePrimaryKind::ALP_PERSISTED;
        frame.alpPrimary = alp.latchedEvent;
        frame.primaryState = synthesizeAlpPrimaryState(v1.state, frame.alpPrimary);
        appendFilteredV1Alerts(frame, v1, /*suppressLaser=*/true, nullptr);
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
