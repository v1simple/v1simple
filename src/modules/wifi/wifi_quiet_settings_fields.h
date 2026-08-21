#pragma once

#include <algorithm>

#include <ArduinoJson.h>
#include <WebServer.h>

#include "../../settings.h"

namespace WifiQuietSettingsFields {

inline void append(JsonDocument& doc, const V1Settings& settings, bool includeSpeedMuteVoice) {
    doc["alertVolumeFadeEnabled"] = settings.alertVolumeFadeEnabled;
    doc["alertVolumeFadeDelaySec"] = settings.alertVolumeFadeDelaySec;
    doc["alertVolumeFadeVolume"] = settings.alertVolumeFadeVolume;
    doc["speedMuteEnabled"] = settings.speedMuteEnabled;
    doc["speedMuteThresholdMph"] = settings.speedMuteThresholdMph;
    doc["speedMuteHysteresisMph"] = settings.speedMuteHysteresisMph;
    doc["speedMuteVolume"] = settings.speedMuteVolume;
    if (includeSpeedMuteVoice) {
        doc["speedMuteVoice"] = settings.speedMuteVoice;
    }
    doc["stealthEnabled"] = settings.stealthEnabled;
}

inline bool argBool(WebServer& server, const char* key) {
    return server.arg(key) == "true" || server.arg(key) == "1";
}

inline void parse(WebServer& server, AudioSettingsUpdate& update, bool includeSpeedMuteVoice) {
    if (server.hasArg("alertVolumeFadeEnabled")) {
        update.hasAlertVolumeFadeEnabled = true;
        update.alertVolumeFadeEnabled = argBool(server, "alertVolumeFadeEnabled");
    }
    if (server.hasArg("alertVolumeFadeDelaySec")) {
        const int delaySec = server.arg("alertVolumeFadeDelaySec").toInt();
        update.hasAlertVolumeFadeDelaySec = true;
        update.alertVolumeFadeDelaySec = static_cast<uint8_t>(std::max(1, std::min(delaySec, 10)));
    }
    if (server.hasArg("alertVolumeFadeVolume")) {
        const int fadeVolume = server.arg("alertVolumeFadeVolume").toInt();
        update.hasAlertVolumeFadeVolume = true;
        update.alertVolumeFadeVolume = static_cast<uint8_t>(std::max(1, std::min(fadeVolume, 9)));
    }
    if (server.hasArg("speedMuteEnabled")) {
        update.hasSpeedMuteEnabled = true;
        update.speedMuteEnabled = argBool(server, "speedMuteEnabled");
    }
    if (server.hasArg("speedMuteThresholdMph")) {
        const int threshold = server.arg("speedMuteThresholdMph").toInt();
        update.hasSpeedMuteThresholdMph = true;
        update.speedMuteThresholdMph = static_cast<uint8_t>(std::max(5, std::min(threshold, 60)));
    }
    if (server.hasArg("speedMuteHysteresisMph")) {
        const int hysteresis = server.arg("speedMuteHysteresisMph").toInt();
        update.hasSpeedMuteHysteresisMph = true;
        update.speedMuteHysteresisMph = static_cast<uint8_t>(std::max(1, std::min(hysteresis, 10)));
    }
    if (server.hasArg("speedMuteVolume")) {
        const int volume = server.arg("speedMuteVolume").toInt();
        update.hasSpeedMuteVolume = true;
        update.speedMuteVolume = (volume >= 0 && volume <= 9) ? static_cast<uint8_t>(volume) : 0;
    }
    if (includeSpeedMuteVoice && server.hasArg("speedMuteVoice")) {
        update.hasSpeedMuteVoice = true;
        update.speedMuteVoice = argBool(server, "speedMuteVoice");
    }
    if (server.hasArg("stealthEnabled")) {
        update.hasStealthEnabled = true;
        update.stealthEnabled = argBool(server, "stealthEnabled");
    }
}

} // namespace WifiQuietSettingsFields
