#include "wifi_v1_profile_api_service.h"

#include <ArduinoJson.h>

#include "wifi_api_response.h"
#include "wifi_json_document.h"
#include "profile_name.h"
#include "v1_firmware_compat.h"

namespace WifiV1ProfileApiService {

namespace {

void appendAvailability(JsonObject target, bool available) {
    target["available"] = available;
    if (!available) target["value"] = nullptr;
}

void appendCapabilities(JsonObject target, uint32_t firmwareVersion) {
    const V1FirmwareCompat::Capabilities capabilities = V1FirmwareCompat::capabilities(firmwareVersion);
    target["versionKnown"] = capabilities.versionKnown;
    target["gen2"] = capabilities.gen2;
    target["supportedUserByteCount"] = capabilities.supportedUserByteCount;
    target["customSweeps"] = capabilities.customSweeps;
    // Write support is separate from the all-volume/saved-volume observation
    // fields below, which do not become available until 4.1037.
    target["volumeChange"] = capabilities.volumeChange;
    target["modeObservation"] = capabilities.modeObservation;
    target["keepBluetoothLedOn"] = capabilities.keepBluetoothLedOn;
    target["allVolume"] = capabilities.allVolume;
    target["savedVolume"] = capabilities.savedVolume;
    target["displayActive"] = capabilities.displayActive;

    JsonObject settings = target["settings"].to<JsonObject>();
    settings["kaAlwaysPriority"] = capabilities.kaAlwaysPriority;
    settings["fastLaserDetect"] = capabilities.fastLaserDetect;
    settings["kaSensitivity"] = capabilities.kaSensitivity;
    settings["startupSequence"] = capabilities.startupSequence;
    settings["restingDisplay"] = capabilities.restingDisplay;
    settings["bsmPlus"] = capabilities.bsmPlus;
    settings["autoMute"] = capabilities.autoMute;
    settings["kSensitivity"] = capabilities.kSensitivity;
    settings["xSensitivity"] = capabilities.xSensitivity;
    settings["photoRadar"] = capabilities.photoRadar;
    settings["gatsoRT4"] = capabilities.gatsoRT4;
    settings["photoIntersectionFilter"] = capabilities.photoIntersectionFilter;
}

void sendCapturedSnapshot(WebServer& server, const Runtime& runtime) {
    WifiJson::Document doc;
    // Maintenance is a different boot from the normal-runtime detector link.
    // Never imply that the persisted observation is still live.
    doc["connected"] = false;
    doc["live"] = false;
    doc["stale"] = true;
    doc["source"] = "normal-runtime-connect";

    V1DeviceRecord device;
    if (!runtime.loadCapturedSnapshot ||
        !runtime.loadCapturedSnapshot(device, runtime.loadCapturedSnapshotCtx) || !device.snapshot.available) {
        doc["available"] = false;
        doc["staleness"] = "not-captured";
        WifiApiResponse::sendJsonDocument(server, 200, doc);
        return;
    }

    const V1DetectorSnapshot& snapshot = device.snapshot;
    doc["available"] = true;
    doc["staleness"] = "previous-boot";
    doc["address"] = device.address;
    doc["name"] = device.name;

    JsonObject provenance = doc["provenance"].to<JsonObject>();
    provenance["capturedBootId"] = snapshot.capturedBootId;
    provenance["capturedUptimeMs"] = snapshot.capturedUptimeMs;
    provenance["sessionGeneration"] = snapshot.sessionGeneration;
    provenance["captureTimedOut"] = snapshot.captureTimedOut;

    JsonObject firmware = doc["firmware"].to<JsonObject>();
    appendAvailability(firmware, snapshot.hasFirmwareVersion);
    if (snapshot.hasFirmwareVersion) firmware["value"] = snapshot.firmwareVersion;
    appendCapabilities(doc["capabilities"].to<JsonObject>(),
                       snapshot.hasFirmwareVersion ? snapshot.firmwareVersion : 0);

    JsonObject observations = doc["observations"].to<JsonObject>();
    JsonObject userBytes = observations["userBytes"].to<JsonObject>();
    appendAvailability(userBytes, snapshot.hasUserBytes);
    if (snapshot.hasUserBytes) {
        JsonArray values = userBytes["value"].to<JsonArray>();
        for (uint8_t value : snapshot.userBytes) values.add(value);

        // Keep the legacy top-level settings shape available to clients while
        // making its captured, non-live provenance explicit above.
        if (runtime.settingsJsonForBytes) {
            WifiJson::Document settingsDoc;
            const String settingsJson =
                runtime.settingsJsonForBytes(snapshot.userBytes.data(), runtime.settingsJsonForBytesCtx);
            if (!deserializeJson(settingsDoc, settingsJson.c_str())) doc["settings"] = settingsDoc;
        }
    }

    JsonObject mode = observations["mode"].to<JsonObject>();
    appendAvailability(mode, snapshot.hasMode);
    if (snapshot.hasMode) mode["value"] = String(snapshot.mode);
    JsonObject displayOn = observations["displayOn"].to<JsonObject>();
    appendAvailability(displayOn, snapshot.hasDisplayOn);
    if (snapshot.hasDisplayOn) displayOn["value"] = snapshot.displayOn;

    JsonObject currentVolume = observations["currentVolume"].to<JsonObject>();
    currentVolume["available"] = snapshot.hasCurrentVolume;
    if (snapshot.hasCurrentVolume) {
        currentVolume["main"] = snapshot.currentMainVolume;
        currentVolume["muted"] = snapshot.currentMutedVolume;
    } else {
        currentVolume["main"] = nullptr;
        currentVolume["muted"] = nullptr;
    }
    JsonObject savedVolume = observations["savedVolume"].to<JsonObject>();
    savedVolume["available"] = snapshot.hasSavedVolume;
    if (snapshot.hasSavedVolume) {
        savedVolume["main"] = snapshot.savedMainVolume;
        savedVolume["muted"] = snapshot.savedMutedVolume;
    } else {
        savedVolume["main"] = nullptr;
        savedVolume["muted"] = nullptr;
    }

    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

} // namespace

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

    // The registered maintenance ingress rejects oversized non-multipart
    // bodies before WebServer's body-sized allocation. Keep this tighter
    // endpoint cap as a semantic limit on profile JSON.
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

    const JsonVariantConst descriptionValue = doc["description"];
    const JsonVariantConst displayOnValue = doc["displayOn"];
    const JsonVariantConst mainVolumeValue = doc["mainVolume"];
    const JsonVariantConst mutedVolumeValue = doc["mutedVolume"];
    const bool hasDescription = !descriptionValue.isUnbound();
    const bool hasDisplayOn = !displayOnValue.isUnbound();
    const bool hasMainVolume = !mainVolumeValue.isUnbound();
    const bool hasMutedVolume = !mutedVolumeValue.isUnbound();
    if ((hasDescription && !descriptionValue.is<const char*>()) ||
        (hasDisplayOn && !displayOnValue.is<bool>()) ||
        (hasMainVolume && !mainVolumeValue.is<int>()) ||
        (hasMutedVolume && !mutedVolumeValue.is<int>())) {
        server.send(400, "application/json", "{\"error\":\"Invalid profile metadata\"}");
        return;
    }

    auto validVolume = [](int value) { return (value >= 0 && value <= 9) || value == 0xFF; };
    if ((hasMainVolume && !validVolume(mainVolumeValue.as<int>())) ||
        (hasMutedVolume && !validVolume(mutedVolumeValue.as<int>()))) {
        server.send(400, "application/json", "{\"error\":\"Invalid profile volume\"}");
        return;
    }

    JsonDocument existingDoc;
    const bool needsExisting = !hasDescription || !hasDisplayOn || !hasMainVolume || !hasMutedVolume;
    if (needsExisting) {
        String existingJson;
        CatalogStatus status = CatalogStatus::NotFound;
        if (runtime.loadProfileJsonResult) {
            status = runtime.loadProfileJsonResult(name, existingJson, runtime.loadProfileJsonResultCtx);
        } else if (runtime.loadProfileJson && runtime.loadProfileJson(name, existingJson, runtime.loadProfileJsonCtx)) {
            status = CatalogStatus::Success;
        }

        if (status == CatalogStatus::Success && deserializeJson(existingDoc, existingJson)) {
            server.send(500, "application/json", "{\"error\":\"Existing profile metadata is corrupt\"}");
            return;
        }
        if (status != CatalogStatus::Success && status != CatalogStatus::NotFound) {
            const int httpStatus = status == CatalogStatus::Busy ? 409 : 500;
            const char* error = status == CatalogStatus::Busy ? "Profile storage busy; retry" :
                                status == CatalogStatus::Corrupt ? "Profile is corrupt" :
                                "Profile read failed";
            server.send(httpStatus, "application/json", String("{\"error\":\"") + error + "\"}");
            return;
        }
    }

    const String description = hasDescription ? descriptionValue.as<const char*>() :
                                                 String(existingDoc["description"] | "");
    const bool displayOn = hasDisplayOn ? displayOnValue.as<bool>() : (existingDoc["displayOn"] | true);
    const uint8_t mainVolume = hasMainVolume ? static_cast<uint8_t>(mainVolumeValue.as<int>()) :
                                              static_cast<uint8_t>(existingDoc["mainVolume"] | 0xFF);
    const uint8_t mutedVolume = hasMutedVolume ? static_cast<uint8_t>(mutedVolumeValue.as<int>()) :
                                                static_cast<uint8_t>(existingDoc["mutedVolume"] | 0xFF);
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
    if (runtime.saveProfile(name, description, displayOn, mainVolume, mutedVolume, settingsBytes, saveError,
                            runtime.saveProfileCtx)) {
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

    // Keep a tighter semantic limit than the transport-wide ingress cap.
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
    if (runtime.loadCapturedSnapshot) {
        sendCapturedSnapshot(server, runtime);
        return;
    }

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
