#include "wifi_quiet_api_service.h"

#include <algorithm>

#include <ArduinoJson.h>

#include "wifi_api_response.h"

namespace WifiQuietApiService {

void handleApiGet(WebServer& server, const Runtime& runtime) {
    if (!runtime.getSettings) {
        server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
        return;
    }

    const V1Settings& settings = runtime.getSettings(runtime.ctx);

    JsonDocument doc;
    doc["alertVolumeFadeEnabled"] = settings.alertVolumeFadeEnabled;
    doc["alertVolumeFadeDelaySec"] = settings.alertVolumeFadeDelaySec;
    doc["alertVolumeFadeVolume"] = settings.alertVolumeFadeVolume;
    doc["speedMuteEnabled"] = settings.speedMuteEnabled;
    doc["speedMuteThresholdMph"] = settings.speedMuteThresholdMph;
    doc["speedMuteHysteresisMph"] = settings.speedMuteHysteresisMph;
    doc["speedMuteVolume"] = settings.speedMuteVolume;
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

    Serial.println("[HTTP] POST /api/quiet/settings");

    auto argBool = [&server](const char* key, bool fallback) -> bool {
        if (!server.hasArg(key))
            return fallback;
        return server.arg(key) == "true" || server.arg(key) == "1";
    };

    const V1Settings& settings = runtime.getSettings(runtime.ctx);
    QuietSettingsUpdate update;

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
    if (server.hasArg("stealthEnabled")) {
        update.hasStealthEnabled = true;
        update.stealthEnabled = argBool("stealthEnabled", settings.stealthEnabled);
    }

    runtime.applySettingsUpdate(update, runtime.ctx);

    server.send(200, "application/json", "{\"success\":true}");
}

} // namespace WifiQuietApiService
