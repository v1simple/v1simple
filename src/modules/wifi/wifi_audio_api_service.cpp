#include "wifi_audio_api_service.h"

#include <algorithm>

#include <ArduinoJson.h>

#include "wifi_api_response.h"

namespace WifiAudioApiService {

void handleApiGet(WebServer& server, const Runtime& runtime) {
    if (!runtime.getSettings) {
        server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
        return;
    }

    const V1Settings& settings = runtime.getSettings(runtime.ctx);

    JsonDocument doc;
    doc["voiceAlertMode"] = static_cast<int>(settings.voiceAlertMode);
    doc["voiceDirectionEnabled"] = settings.voiceDirectionEnabled;
    doc["announceBogeyCount"] = settings.announceBogeyCount;
    doc["muteVoiceIfVolZero"] = settings.muteVoiceIfVolZero;
    doc["voiceVolume"] = settings.voiceVolume;
    doc["announceSecondaryAlerts"] = settings.announceSecondaryAlerts;
    doc["secondaryLaser"] = settings.secondaryLaser;
    doc["secondaryKa"] = settings.secondaryKa;
    doc["secondaryK"] = settings.secondaryK;
    doc["secondaryX"] = settings.secondaryX;
    doc["alertVolumeFadeEnabled"] = settings.alertVolumeFadeEnabled;
    doc["alertVolumeFadeDelaySec"] = settings.alertVolumeFadeDelaySec;
    doc["alertVolumeFadeVolume"] = settings.alertVolumeFadeVolume;
    doc["speedMuteEnabled"] = settings.speedMuteEnabled;
    doc["speedMuteThresholdMph"] = settings.speedMuteThresholdMph;
    doc["speedMuteHysteresisMph"] = settings.speedMuteHysteresisMph;
    doc["speedMuteVolume"] = settings.speedMuteVolume;
    doc["speedMuteVoice"] = settings.speedMuteVoice;
    doc["stealthEnabled"] = settings.stealthEnabled;

    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiSave(WebServer& server, const Runtime& runtime) {
    if (runtime.checkRateLimit && !runtime.checkRateLimit(runtime.ctx))
        return;

    if (!runtime.getSettings || !runtime.applySettingsUpdate) {
        server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
        return;
    }

    Serial.println("[HTTP] POST /api/audio/settings");

    auto argBool = [&server](const char* key, bool fallback) -> bool {
        if (!server.hasArg(key))
            return fallback;
        return server.arg(key) == "true" || server.arg(key) == "1";
    };

    const V1Settings& settings = runtime.getSettings(runtime.ctx);
    AudioSettingsUpdate update;
    bool hasVoiceVolume = false;
    uint8_t nextVoiceVolume = settings.voiceVolume;

    if (server.hasArg("voiceAlertMode")) {
        int mode = server.arg("voiceAlertMode").toInt();
        mode = std::max(0, std::min(mode, 3));
        update.hasVoiceAlertMode = true;
        update.voiceAlertMode = static_cast<VoiceAlertMode>(mode);
    }
    if (server.hasArg("voiceDirectionEnabled")) {
        update.hasVoiceDirectionEnabled = true;
        update.voiceDirectionEnabled = argBool("voiceDirectionEnabled", settings.voiceDirectionEnabled);
    }
    if (server.hasArg("announceBogeyCount")) {
        update.hasAnnounceBogeyCount = true;
        update.announceBogeyCount = argBool("announceBogeyCount", settings.announceBogeyCount);
    }
    if (server.hasArg("muteVoiceIfVolZero")) {
        update.hasMuteVoiceIfVolZero = true;
        update.muteVoiceIfVolZero = argBool("muteVoiceIfVolZero", settings.muteVoiceIfVolZero);
    }
    if (server.hasArg("voiceVolume")) {
        int volume = server.arg("voiceVolume").toInt();
        volume = std::max(0, std::min(volume, 100));
        update.hasVoiceVolume = true;
        update.voiceVolume = static_cast<uint8_t>(volume);
        hasVoiceVolume = true;
        nextVoiceVolume = static_cast<uint8_t>(volume);
    }
    if (server.hasArg("announceSecondaryAlerts")) {
        update.hasAnnounceSecondaryAlerts = true;
        update.announceSecondaryAlerts = argBool("announceSecondaryAlerts", settings.announceSecondaryAlerts);
    }
    if (server.hasArg("secondaryLaser")) {
        update.hasSecondaryLaser = true;
        update.secondaryLaser = argBool("secondaryLaser", settings.secondaryLaser);
    }
    if (server.hasArg("secondaryKa")) {
        update.hasSecondaryKa = true;
        update.secondaryKa = argBool("secondaryKa", settings.secondaryKa);
    }
    if (server.hasArg("secondaryK")) {
        update.hasSecondaryK = true;
        update.secondaryK = argBool("secondaryK", settings.secondaryK);
    }
    if (server.hasArg("secondaryX")) {
        update.hasSecondaryX = true;
        update.secondaryX = argBool("secondaryX", settings.secondaryX);
    }
    if (server.hasArg("alertVolumeFadeEnabled")) {
        update.hasAlertVolumeFadeEnabled = true;
        update.alertVolumeFadeEnabled = argBool("alertVolumeFadeEnabled", settings.alertVolumeFadeEnabled);
    }
    if (server.hasArg("alertVolumeFadeDelaySec")) {
        int delaySec = server.arg("alertVolumeFadeDelaySec").toInt();
        update.hasAlertVolumeFadeDelaySec = true;
        update.alertVolumeFadeDelaySec = static_cast<uint8_t>(std::max(1, std::min(delaySec, 10)));
    }
    if (server.hasArg("alertVolumeFadeVolume")) {
        int fadeVolume = server.arg("alertVolumeFadeVolume").toInt();
        update.hasAlertVolumeFadeVolume = true;
        update.alertVolumeFadeVolume = static_cast<uint8_t>(std::max(1, std::min(fadeVolume, 9)));
    }
    if (server.hasArg("speedMuteEnabled")) {
        update.hasSpeedMuteEnabled = true;
        update.speedMuteEnabled = argBool("speedMuteEnabled", settings.speedMuteEnabled);
    }
    if (server.hasArg("speedMuteThresholdMph")) {
        int threshold = server.arg("speedMuteThresholdMph").toInt();
        update.hasSpeedMuteThresholdMph = true;
        update.speedMuteThresholdMph = static_cast<uint8_t>(std::max(5, std::min(threshold, 60)));
    }
    if (server.hasArg("speedMuteHysteresisMph")) {
        int hysteresis = server.arg("speedMuteHysteresisMph").toInt();
        update.hasSpeedMuteHysteresisMph = true;
        update.speedMuteHysteresisMph = static_cast<uint8_t>(std::max(1, std::min(hysteresis, 10)));
    }
    if (server.hasArg("speedMuteVolume")) {
        int vol = server.arg("speedMuteVolume").toInt();
        update.hasSpeedMuteVolume = true;
        update.speedMuteVolume = (vol >= 0 && vol <= 9) ? static_cast<uint8_t>(vol) : 0;
    }
    if (server.hasArg("speedMuteVoice")) {
        update.hasSpeedMuteVoice = true;
        update.speedMuteVoice = argBool("speedMuteVoice", settings.speedMuteVoice);
    }
    if (server.hasArg("stealthEnabled")) {
        update.hasStealthEnabled = true;
        update.stealthEnabled = argBool("stealthEnabled", settings.stealthEnabled);
    }

    runtime.applySettingsUpdate(update, runtime.ctx);

    if (hasVoiceVolume && runtime.setAudioVolume) {
        runtime.setAudioVolume(nextVoiceVolume, runtime.ctx);
    }

    server.send(200, "application/json", "{\"success\":true}");
}

} // namespace WifiAudioApiService
