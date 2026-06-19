#include "wifi_v1_profile_api_service.h"

#include <cstring>

#include <ArduinoJson.h>

#include "wifi_api_response.h"
#include "wifi_json_document.h"
#include "../../settings.h"
#include "../../v1_profile_push_policy.h"

namespace WifiV1ProfileApiService {

static void sendMaintenanceModeError(WebServer& server) {
    WifiJson::Document doc;
    doc["error"] = "maintenance_mode";
    doc["message"] = "V1 push/pull not available in maintenance mode";
    WifiApiResponse::sendJsonDocument(server, 409, doc);
}

void handleApiProfilesList(WebServer& server, const Runtime& runtime) {
    std::vector<String> profileNames;
    if (runtime.listProfileNames) {
        profileNames = runtime.listProfileNames(runtime.listProfileNamesCtx);
    }
    Serial.printf("[V1Profiles] Listing %d profiles\n", profileNames.size());

    WifiJson::Document doc;
    JsonArray array = doc["profiles"].to<JsonArray>();

    for (const String& name : profileNames) {
        ProfileSummary profile;
        if (runtime.loadProfileSummary && runtime.loadProfileSummary(name, profile, runtime.loadProfileSummaryCtx)) {
            JsonObject obj = array.add<JsonObject>();
            obj["name"] = profile.name;
            obj["description"] = profile.description;
            obj["displayOn"] = profile.displayOn;
            Serial.printf("[V1Profiles]   - %s: %s\n", profile.name.c_str(), profile.description.c_str());
        }
    }

    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiProfileGet(WebServer& server, const Runtime& runtime) {
    if (!server.hasArg("name")) {
        server.send(400, "application/json", "{\"error\":\"Missing profile name\"}");
        return;
    }

    String name = server.arg("name");
    String profileJson;
    if (!runtime.loadProfileJson || !runtime.loadProfileJson(name, profileJson, runtime.loadProfileJsonCtx)) {
        server.send(404, "application/json", "{\"error\":\"Profile not found\"}");
        return;
    }

    server.send(200, "application/json", profileJson);
}

void handleApiProfileSave(WebServer& server,
                          const Runtime& runtime,
                          bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;

    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing request body\"}");
        return;
    }

    String body = server.arg("plain");
    if (body.length() > 4096) {
        server.send(400, "application/json", "{\"error\":\"Payload too large\"}");
        return;
    }
    Serial.printf("[V1Settings] Save request body: %s\n", body.c_str());

    WifiJson::Document doc;
    DeserializationError err = deserializeJson(doc, body.c_str());

    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    String name = doc["name"] | "";
    if (name.isEmpty()) {
        server.send(400, "application/json", "{\"error\":\"Missing profile name\"}");
        return;
    }

    if (!runtime.parseSettingsJson || !runtime.saveProfile) {
        server.send(500, "application/json", "{\"error\":\"Profile persistence unavailable\"}");
        return;
    }

    const String description = doc["description"] | "";
    const bool displayOn = doc["displayOn"] | true;  // Default to on
    uint8_t settingsBytes[6];
    memset(settingsBytes, 0xFF, sizeof(settingsBytes));

    // Parse settings from JSON
    JsonObject settingsObj = doc["settings"];
    if (!settingsObj.isNull()) {
        if (!runtime.parseSettingsJson(settingsObj, settingsBytes, runtime.parseSettingsJsonCtx)) {
            server.send(400, "application/json", "{\"error\":\"Invalid settings\"}");
            return;
        }
    } else {
        // Direct settings in root
        JsonObject rootObj = doc.as<JsonObject>();
        if (!runtime.parseSettingsJson(rootObj, settingsBytes, runtime.parseSettingsJsonCtx)) {
            server.send(400, "application/json", "{\"error\":\"Invalid settings\"}");
            return;
        }
    }

    String saveError;
    if (runtime.saveProfile(name, description, displayOn, settingsBytes, saveError, runtime.saveProfileCtx)) {
        if (runtime.backupToSd) {
            runtime.backupToSd(runtime.backupToSdCtx);
        }
        Serial.printf("[V1Profiles] Profile '%s' saved successfully\n", name.c_str());
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        Serial.printf("[V1Profiles] Failed to save profile '%s': %s\n", name.c_str(), saveError.c_str());
        String errorJson = String("{\"error\":\"") + saveError + "\"}";
        server.send(500, "application/json", errorJson);
    }
}

void handleApiProfileDelete(WebServer& server,
                            const Runtime& runtime,
                            bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;

    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing request body\"}");
        return;
    }

    String body = server.arg("plain");
    if (body.length() > 2048) {
        server.send(400, "application/json", "{\"error\":\"Payload too large\"}");
        return;
    }
    WifiJson::Document doc;
    DeserializationError err = deserializeJson(doc, body.c_str());
    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    String name = doc["name"] | "";
    if (name.isEmpty()) {
        server.send(400, "application/json", "{\"error\":\"Missing profile name\"}");
        return;
    }

    if (!runtime.deleteProfile) {
        server.send(500, "application/json", "{\"error\":\"Profile persistence unavailable\"}");
        return;
    }

    if (runtime.deleteProfile(name, runtime.deleteProfileCtx)) {
        if (runtime.backupToSd) {
            runtime.backupToSd(runtime.backupToSdCtx);
        }
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        server.send(404, "application/json", "{\"error\":\"Profile not found\"}");
    }
}

void handleApiCurrentSettings(WebServer& server, const Runtime& runtime) {
    WifiJson::Document doc;
    doc["connected"] = runtime.v1Connected ? runtime.v1Connected(runtime.v1ConnectedCtx) : false;

    if (!runtime.hasCurrentSettings || !runtime.hasCurrentSettings(runtime.hasCurrentSettingsCtx)) {
        doc["available"] = false;
        WifiApiResponse::sendJsonDocument(server, 200, doc);
        return;
    }

    doc["available"] = true;
    // Parse existing settings JSON and embed it
    if (runtime.currentSettingsJson) {
        WifiJson::Document settingsDoc;
        String settingsJson = runtime.currentSettingsJson(runtime.currentSettingsJsonCtx);
        DeserializationError parseErr = deserializeJson(settingsDoc, settingsJson.c_str());
        if (!parseErr) {
            doc["settings"] = settingsDoc;
        }
    }

    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiSettingsPull(WebServer& server,
                           const Runtime& runtime,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;

    if (runtime.maintenanceBootActive) {
        sendMaintenanceModeError(server);
        return;
    }

    if (!runtime.v1Connected || !runtime.v1Connected(runtime.v1ConnectedCtx)) {
        server.send(503, "application/json", "{\"error\":\"V1 not connected\"}");
        return;
    }

    bool requested = false;
    if (runtime.requestUserBytes) {
        requested = runtime.requestUserBytes(runtime.requestUserBytesCtx);
    }
    if (requested) {
        // Response will come async via BLE callback
        server.send(200, "application/json", "{\"success\":true,\"message\":\"Request sent. Check current settings.\"}");
    } else {
        server.send(500, "application/json", "{\"error\":\"Failed to send request\"}");
    }
}

void handleApiSettingsPush(WebServer& server,
                           const Runtime& runtime,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;

    if (runtime.maintenanceBootActive) {
        sendMaintenanceModeError(server);
        return;
    }

    if (!runtime.v1Connected || !runtime.v1Connected(runtime.v1ConnectedCtx)) {
        server.send(503, "application/json", "{\"error\":\"V1 not connected\"}");
        return;
    }

    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing request body\"}");
        return;
    }

    String body = server.arg("plain");
    Serial.printf("[V1Settings] Push request: %s\n", body.c_str());
    if (body.length() > 4096) {
        server.send(400, "application/json", "{\"error\":\"Payload too large\"}");
        return;
    }

    WifiJson::Document doc;
    DeserializationError err = deserializeJson(doc, body.c_str());
    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    uint8_t bytes[6];
    bool displayOn = true;

    // Check if pushing a profile by name
    String profileName = doc["name"] | "";
    if (!profileName.isEmpty()) {
        if (!runtime.loadProfileSettings ||
            !runtime.loadProfileSettings(profileName, bytes, displayOn, runtime.loadProfileSettingsCtx)) {
            server.send(404, "application/json", "{\"error\":\"Profile not found\"}");
            return;
        }
        Serial.printf("[V1Settings] Pushing profile '%s': %02X %02X %02X %02X %02X %02X\n",
                      profileName.c_str(),
                      bytes[0],
                      bytes[1],
                      bytes[2],
                      bytes[3],
                      bytes[4],
                      bytes[5]);
    }
    // Check for bytes array
    else if (doc["bytes"].is<JsonArray>()) {
        JsonArray bytesArray = doc["bytes"];
        if (bytesArray.size() != 6) {
            server.send(400, "application/json", "{\"error\":\"Invalid bytes array\"}");
            return;
        }
        for (int i = 0; i < 6; i++) {
            bytes[i] = bytesArray[i].as<uint8_t>();
        }
        displayOn = doc["displayOn"] | true;
        Serial.println("[V1Settings] Using raw bytes from request");
    }
    // Parse from individual settings
    else {
        JsonObject settingsObj = doc["settings"].as<JsonObject>();
        if (settingsObj.isNull()) {
            settingsObj = doc.as<JsonObject>();
        }
        if (!runtime.parseSettingsJson || !runtime.parseSettingsJson(settingsObj, bytes, runtime.parseSettingsJsonCtx)) {
            server.send(400, "application/json", "{\"error\":\"Invalid settings\"}");
            return;
        }
        displayOn = doc["displayOn"] | true;
        Serial.printf("[V1Settings] Built bytes from settings: %02X %02X %02X %02X %02X %02X\n",
                      bytes[0],
                      bytes[1],
                      bytes[2],
                      bytes[3],
                      bytes[4],
                      bytes[5]);
    }

    bool writeOk = false;
    if (runtime.getSettings) {
        V1ProfilePushPolicy::applyBeforePush(runtime.getSettings(runtime.getSettingsCtx), bytes);
    }
    if (runtime.writeUserBytes) {
        writeOk = runtime.writeUserBytes(bytes, runtime.writeUserBytesCtx);
    }
    if (writeOk) {
        Serial.println("[V1Settings] Push sent successfully");
        if (runtime.setDisplayOn) {
            runtime.setDisplayOn(displayOn, runtime.setDisplayOnCtx);
        }
        server.send(200, "application/json", "{\"success\":true,\"message\":\"Settings sent to V1\"}");
    } else {
        Serial.println("[V1Settings] Push FAILED - write command rejected");
        server.send(500, "application/json", "{\"error\":\"Write command failed - check V1 connection\"}");
    }
}

}  // namespace WifiV1ProfileApiService
