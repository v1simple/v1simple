#include "wifi_quiet_api_service.h"

#include <ArduinoJson.h>

#include "wifi_api_response.h"
#include "wifi_json_document.h"
#include "wifi_quiet_settings_fields.h"

namespace WifiQuietApiService {

void handleApiGet(WebServer& server, const Runtime& runtime) {
    if (!runtime.getSettings) {
        server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
        return;
    }

    const V1Settings& settings = runtime.getSettings(runtime.ctx);

    WifiJson::Document doc;
    WifiQuietSettingsFields::append(doc, settings, false);

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

    AudioSettingsUpdate update;
    WifiQuietSettingsFields::parse(server, update, false);

    const SettingsPersistResult result = runtime.applySettingsUpdate(update, runtime.ctx);
    if (!result.success) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"settings_persist_failed\"}");
        return;
    }

    server.send(200, "application/json", "{\"success\":true}");
}

} // namespace WifiQuietApiService
