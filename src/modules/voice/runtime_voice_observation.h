#pragma once

#include "render_frame.h"
#include "voice_playback_coordinator.h"

// Runtime boundary only: use the already-filtered presentation frame, never
// rebuild its V1 rows independently from parser state (notably during ALP).
static_assert(RenderFrame::MAX_CARDS + 1 == 16, "voice observation matches the bounded presentation rows");

inline bool isRenderableVoiceAlert(const AlertData& alert) {
    return alert.isValid && alert.band != BAND_NONE && (alert.band == BAND_LASER || alert.frequency != 0);
}

inline V1VoiceObservation makeV1VoiceObservation(const RenderFrame& frame, uint32_t episode, bool proxyConnected) {
    V1VoiceObservation result;
    result.episode = episode;
    result.muted = frame.context.muted;
    result.softMuted = frame.context.softMuted;
    result.mainVolume = frame.context.mainVolume;
    result.proxyConnected = proxyConnected;
    const bool frameCarriesV1Primary = frame.primaryKind == RenderFramePrimaryKind::V1_LIVE;
    bool foundFlaggedRenderablePriority = false;

    if (frameCarriesV1Primary) {
        result.alerts[result.count++] = frame.v1Priority;
        result.hasPriority = true;
        result.priority = frame.v1Priority;
    }

    for (int index = 0; index < frame.cardCount; ++index) {
        const RenderFrameCard& card = frame.cards[index];
        if (result.count >= static_cast<int>(result.alerts.size())) {
            break;
        }
        result.alerts[result.count++] = card.v1Alert;
        if (!result.hasPriority && isRenderableVoiceAlert(card.v1Alert)) {
            result.hasPriority = true;
            result.priority = card.v1Alert;
        }
        // ALP owns primary presentation, but V1's flagged row still owns its
        // speaker decision. Keep card order; use the first renderable fallback
        // only when no flagged renderable row exists.
        if (!frameCarriesV1Primary && !foundFlaggedRenderablePriority && card.v1Alert.isPriority &&
            isRenderableVoiceAlert(card.v1Alert)) {
            result.hasPriority = true;
            result.priority = card.v1Alert;
            foundFlaggedRenderablePriority = true;
        }
    }

    return result;
}
