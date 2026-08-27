#include "wifi_v1_profile_api_service.h"

#include <ArduinoJson.h>

#include "wifi_api_response.h"
#include "wifi_json_document.h"
#include "profile_name.h"

namespace WifiV1ProfileApiService {

void handleApiProfilesList(WebServer& server, const Runtime& runtime) {
    std::vector<String> profileNames;
    if (runtime.listProfileNamesResult) {
        const CatalogStatus status =
            runtime.listProfileNamesResult(profileNames, runtime.listProfileNamesResultCtx);
        if (status != CatalogStatus::Success) {
            const int httpStatus = status == CatalogStatus::Busy ? 409 : 500;
            const char* error = status == CatalogStatus::Busy ? "Profile storage busy; retry" :
                                status == CatalogStatus::Corrupt ? "Profile catalog corrupt" :
                                "Profile catalog unavailable";
            server.send(httpStatus, "application/json", String("{\"error\":\"") + error + "\"}");
            return;
        }
    } else if (runtime.listProfileNames) {
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
        }
    }

    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiProfileGet(WebServer& server, const Runtime& runtime) {
    if (!server.hasArg("name")) {
        server.send(400, "application/json", "{\"error\":\"Missing profile name\"}");
        return;
    }

    String name;
    const ProfileNameStatus nameStatus = canonicalizeProfileName(server.arg("name"), name);
    if (nameStatus != ProfileNameStatus::Valid) {
        server.send(400, "application/json", String("{\"error\":\"") + profileNameStatusMessage(nameStatus) +
                                                 "\"}");
        return;
    }
    String profileJson;
    if (runtime.loadProfileJsonResult) {
        const CatalogStatus status =
            runtime.loadProfileJsonResult(name, profileJson, runtime.loadProfileJsonResultCtx);
        if (status == CatalogStatus::Success) {
            server.send(200, "application/json", profileJson);
            return;
        }
        const int httpStatus = status == CatalogStatus::NotFound ? 404 : status == CatalogStatus::Busy ? 409 : 500;
        const char* error = status == CatalogStatus::NotFound ? "Profile not found" :
                            status == CatalogStatus::Busy ? "Profile storage busy; retry" :
                            status == CatalogStatus::Corrupt ? "Profile is corrupt" : "Profile read failed";
        server.send(httpStatus, "application/json", String("{\"error\":\"") + error + "\"}");
        return;
    }
    if (!runtime.loadProfileJson || !runtime.loadProfileJson(name, profileJson, runtime.loadProfileJsonCtx)) {
        server.send(404, "application/json", "{\"error\":\"Profile not found\"}");
        return;
    }

    server.send(200, "application/json", profileJson);
}

void handleApiProfileSave(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                          void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;

    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing request body\"}");
        return;
    }

    // Arduino WebServer buffers the entire body before dispatching this handler,
    // so this cap is an application limit on what we will parse.
    // This cap does not bound the transport allocation or prevent a large
    // upload from exhausting the heap. WebServer::arg() returns String by
    // value, so binding it once is the minimum; another call would allocate
    // the whole body a second time.
    const String body = server.arg("plain");
    if (body.length() > 4096) {
        server.send(400, "application/json", "{\"error\":\"Payload too large\"}");
        return;
    }
    Serial.printf("[V1Settings] Save request body accepted (%u bytes)\n", static_cast<unsigned>(body.length()));

    WifiJson::Document doc;
    DeserializationError err = deserializeJson(doc, body.c_str());

    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    String requestedName = doc["name"] | "";
    String name;
    const ProfileNameStatus nameStatus = canonicalizeProfileName(requestedName, name);
    if (nameStatus != ProfileNameStatus::Valid) {
        server.send(400, "application/json", String("{\"error\":\"") + profileNameStatusMessage(nameStatus) +
                                                 "\"}");
        return;
    }

    if (!runtime.parseSettingsJson || !runtime.saveProfile) {
        server.send(500, "application/json", "{\"error\":\"Profile persistence unavailable\"}");
        return;
    }

    const String description = doc["description"] | "";
    const bool displayOn = doc["displayOn"] | true; // Default to on
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
        Serial.println("[V1Profiles] Profile saved successfully");
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        Serial.println("[V1Profiles] Failed to save profile");
        // saveError is filesystem/profile-store text and can contain quotes or
        // backslashes; build the response through ArduinoJson so it is escaped
        // and the UI's res.json() cannot throw on a malformed body.
        WifiJson::Document errorDoc;
        WifiApiResponse::setErrorAndMessage(errorDoc, saveError.c_str());
        WifiApiResponse::sendJsonDocument(server, saveError.indexOf("busy") >= 0 ? 409 : 500, errorDoc);
    }
}

void handleApiProfileDelete(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                            void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;

    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing request body\"}");
        return;
    }

    // Same story as the save handler: WebServer already buffered the body, so
    // this is a semantic/application cap on what we will parse, NOT a bound on
    // the transport allocation.
    const String body = server.arg("plain");
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

    String requestedName = doc["name"] | "";
    String name;
    const ProfileNameStatus nameStatus = canonicalizeProfileName(requestedName, name);
    if (nameStatus != ProfileNameStatus::Valid) {
        server.send(400, "application/json", String("{\"error\":\"") + profileNameStatusMessage(nameStatus) +
                                                 "\"}");
        return;
    }

    if (!runtime.deleteProfileResult && !runtime.deleteProfile) {
        server.send(500, "application/json", "{\"error\":\"Profile persistence unavailable\"}");
        return;
    }

    CatalogStatus deleteStatus = CatalogStatus::IoError;
    if (runtime.deleteProfileResult) {
        deleteStatus = runtime.deleteProfileResult(name, runtime.deleteProfileResultCtx);
    } else if (runtime.deleteProfile(name, runtime.deleteProfileCtx)) {
        deleteStatus = CatalogStatus::Success;
    } else {
        deleteStatus = CatalogStatus::NotFound;
    }

    if (deleteStatus == CatalogStatus::Success) {
        if (runtime.backupToSd) {
            runtime.backupToSd(runtime.backupToSdCtx);
        }
        server.send(200, "application/json", "{\"success\":true}");
    } else if (deleteStatus == CatalogStatus::NotFound) {
        server.send(404, "application/json", "{\"error\":\"Profile not found\"}");
    } else if (deleteStatus == CatalogStatus::Busy) {
        server.send(409, "application/json", "{\"error\":\"Profile storage busy; retry\"}");
    } else if (deleteStatus == CatalogStatus::InvalidName) {
        server.send(400, "application/json", "{\"error\":\"Invalid profile name\"}");
    } else if (deleteStatus == CatalogStatus::Corrupt) {
        server.send(500, "application/json", "{\"error\":\"Profile is corrupt\"}");
    } else {
        server.send(500, "application/json", "{\"error\":\"Profile deletion failed\"}");
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

} // namespace WifiV1ProfileApiService
