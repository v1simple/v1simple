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
            voiceCtx.isSuppressed = true;
            presentation_.voiceSuppressed = true;
        }
    }

    (void)priorityBand;
}
