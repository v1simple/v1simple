#pragma once

// Fixture inputs for coordinator/worker tests with no ALP or rendering owner.
// Runtime composition/filtering is exercised separately through the real frame
// adapter; these tests supply the real parser's bounded V1 observations only.
#include "../../src/modules/voice/voice_playback_coordinator.h"

inline V1VoiceObservation fixtureVoiceObservation(PacketParser& parser, const V1BLEClient& ble) {
    V1VoiceObservation observation;
    const DisplayState& state = parser.getDisplayState();
    observation.episode = parser.alertLifetime();
    observation.muted = state.muted;
    observation.softMuted = state.softMuted;
    observation.mainVolume = state.mainVolume;
    observation.proxyConnected = ble.isProxyClientConnected();
    observation.hasPriority = parser.hasAlerts() && parser.getRenderablePriorityAlert(observation.priority);
    if (observation.hasPriority) observation.alerts[observation.count++] = observation.priority;
    bool skippedPriority = false;
    for (int index = 0; index < parser.getAlertCount(); ++index) {
        const AlertData& alert = parser.getAllAlerts()[index];
        if (!alert.isValid || alert.band == BAND_NONE) continue;
        const auto& priority = observation.priority;
        if (observation.hasPriority && !skippedPriority && alert.isValid == priority.isValid &&
            alert.band == priority.band && alert.direction == priority.direction &&
            alert.frontStrength == priority.frontStrength && alert.rearStrength == priority.rearStrength &&
            alert.frequency == priority.frequency && alert.isPriority == priority.isPriority) {
            skippedPriority = true;
            continue;
        }
        if (observation.count == observation.alerts.size()) break;
        observation.alerts[observation.count++] = alert;
    }
    return observation;
}

inline VoiceSettings fixtureVoiceSettings(const V1Settings& settings) {
    return {settings.voiceAlertMode, settings.voiceDirectionEnabled, settings.announceBogeyCount};
}
