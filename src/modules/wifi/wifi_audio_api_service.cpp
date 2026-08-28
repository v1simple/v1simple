#include "wifi_audio_api_service.h"

#include <algorithm>

#include <ArduinoJson.h>

#include "wifi_api_response.h"
#include "wifi_json_document.h"
#include "wifi_quiet_settings_fields.h"

namespace WifiAudioApiService {

void handleApiGet(WebServer& server, const Runtime& runtime) {
    if (!runtime.getSettings) {
        server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
        return;
    }

    const V1Settings& settings = runtime.getSettings(runtime.ctx);

    WifiJson::Document doc;
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
    WifiQuietSettingsFields::append(doc, settings, true);

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
        update.voiceDirectionEnabled = WifiQuietSettingsFields::argBool(server, "voiceDirectionEnabled");
    }
    if (server.hasArg("announceBogeyCount")) {
        update.hasAnnounceBogeyCount = true;
        update.announceBogeyCount = WifiQuietSettingsFields::argBool(server, "announceBogeyCount");
    }
    if (server.hasArg("muteVoiceIfVolZero")) {
        update.hasMuteVoiceIfVolZero = true;
        update.muteVoiceIfVolZero = WifiQuietSettingsFields::argBool(server, "muteVoiceIfVolZero");
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
        update.announceSecondaryAlerts = WifiQuietSettingsFields::argBool(server, "announceSecondaryAlerts");
    }
    if (server.hasArg("secondaryLaser")) {
        update.hasSecondaryLaser = true;
        update.secondaryLaser = WifiQuietSettingsFields::argBool(server, "secondaryLaser");
    }
    if (server.hasArg("secondaryKa")) {
        update.hasSecondaryKa = true;
        update.secondaryKa = WifiQuietSettingsFields::argBool(server, "secondaryKa");
    }
    if (server.hasArg("secondaryK")) {
        update.hasSecondaryK = true;
        update.secondaryK = WifiQuietSettingsFields::argBool(server, "secondaryK");
    }
    if (server.hasArg("secondaryX")) {
        update.hasSecondaryX = true;
        update.secondaryX = WifiQuietSettingsFields::argBool(server, "secondaryX");
    }
    WifiQuietSettingsFields::parse(server, update, true);

    const SettingsPersistResult result = runtime.applySettingsUpdate(update, runtime.ctx);
    if (!result.success) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"settings_persist_failed\"}");
        return;
    }

    if (hasVoiceVolume && runtime.setAudioVolume) {
        runtime.setAudioVolume(nextVoiceVolume, runtime.ctx);
    }

    server.send(200, "application/json", "{\"success\":true}");
}

} // namespace WifiAudioApiService
