#pragma once

#include "quiet_coordinator_module.h"

template <typename SpeedMuteLike>
void QuietCoordinatorModule::applyVoicePresentation(VoiceContext& voiceCtx, const SpeedMuteLike* speedMute,
                                                    const bool hasRenderablePriority, const uint8_t priorityBand) {
    syncCommittedState();

    presentation_.voiceSuppressed = false;
    presentation_.voiceAllowVolZeroBypass = false;
    voiceCtx.isSuppressed = false;

    if (!voiceCtx.isSuppressed && speedMute) {
        const auto& smSettings = speedMute->getSettings();
        const auto& smState = speedMute->getState();
        if (smSettings.voice && smState.muteActive && hasRenderablePriority) {
            if (!speedMute->isBandOverridden(priorityBand)) {
                voiceCtx.isSuppressed = true;
                presentation_.voiceSuppressed = true;
            }
        }
    }

    if (speedMute && hasRenderablePriority) {
        if (speedVolActive_ && speedMute->isBandOverridden(priorityBand)) {
            voiceCtx.isMuted = false;
            voiceCtx.isSoftMuted = false; // also bypass audio-mute gate (audit B2)
            if (voiceCtx.mainVolume == 0) {
                voiceCtx.mainVolume = 1;
            }
            presentation_.voiceAllowVolZeroBypass = true;
        }
    }
}
