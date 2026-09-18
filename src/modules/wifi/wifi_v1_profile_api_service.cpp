#include "wifi_v1_profile_api_service.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <cstring>
#include <utility>

#include "wifi_api_response.h"
#include "exact_urlencoded_form.h"
#include "../../json_exact_input.h"
#include "../../psram_json_document.h"
#include "wifi_json_document.h"
#include "profile_name.h"
#include "v1_firmware_compat.h"

namespace WifiV1ProfileApiService {

namespace {

bool jsonKeyEquals(JsonString key, const char* expected) {
    const size_t length = std::strlen(expected);
    return key.c_str() && key.size() == length && std::memcmp(key.c_str(), expected, length) == 0;
}

bool objectHasOnlyKeys(JsonObjectConst object, const char* const* allowed, size_t allowedCount) {
    for (JsonPairConst pair : object) {
        bool known = false;
        for (size_t index = 0; index < allowedCount; ++index) {
            if (jsonKeyEquals(pair.key(), allowed[index])) {
                known = true;
                break;
            }
        }
        if (!known) return false;
    }
    return true;
}

bool profileSettingsKeysAreKnown(JsonObjectConst settings) {
    static constexpr const char* ALLOWED[] = {
        "bytes", "baseBytes", "xBand", "kBand", "kaBand", "laser", "kuBand", "euro",
        "kVerifier", "laserRear", "customFreqs", "kaAlwaysPriority", "fastLaserDetect",
        "kaSensitivity", "kSensitivity", "xSensitivity", "autoMute", "muteToMuteVolume",
        "bogeyLockLoud", "muteXKRear", "startupSequence", "restingDisplay", "bsmPlus", "mrct",
        "driveSafe3D", "driveSafe3DHD", "redflexHalo", "redflexNK7", "ekin", "photoVerifier",
        "gatsoRT4", "photoIntersectionFilter",
    };
    return objectHasOnlyKeys(settings, ALLOWED, sizeof(ALLOWED) / sizeof(ALLOWED[0]));
}

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
    target["keepCurrentVolumeOnDisconnect"] =
        firmwareVersion >= V1FirmwareCompat::kKeepCurrentVolumeOnDisconnectVersion && capabilities.gen2;
    target["displayActive"] = capabilities.displayActive;
    // The destructive workflow is admitted only for a persisted, captured
    // Gen2 target and is executed by the durable normal-boot job engine.
    target["detectorFactoryResetWorkflowAvailable"] = capabilities.gen2;
    target["localDefaultsScope"] = "profile_draft_only";

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
    PsramJson::Document doc;
    // Maintenance is a different boot from the normal-runtime detector link.
    // Never imply that the persisted observation is still live.
    doc["connected"] = false;
    doc["live"] = false;
    doc["stale"] = true;
    doc["source"] = "normal-runtime-connect";

    if (runtime.capturedSnapshotSourceAvailable &&
        !runtime.capturedSnapshotSourceAvailable(runtime.capturedSnapshotSourceAvailableCtx)) {
        server.send(503, "application/json",
                    "{\"error\":\"Captured settings storage unavailable\",\"retryable\":true}");
        return;
    }

    V1DeviceRecord device;
    if (!runtime.loadCapturedSnapshot ||
        !runtime.loadCapturedSnapshot(device, runtime.loadCapturedSnapshotCtx) || !device.snapshot.available) {
        doc["available"] = false;
        doc["staleness"] = "not-captured";
        if (doc.overflowed() || measureJson(doc) == 0) {
            server.send(503, "application/json", "{\"error\":\"Captured settings unavailable\"}");
            return;
        }
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
            PsramJson::Document settingsDoc;
            String settingsJson;
            if (!runtime.settingsJsonForBytes(snapshot.userBytes.data(), settingsJson,
                                              runtime.settingsJsonForBytesCtx) ||
                deserializeJson(settingsDoc, settingsJson.c_str()) || settingsDoc.overflowed()) {
                server.send(503, "application/json", "{\"error\":\"Captured settings unavailable\"}");
                return;
            }
            doc["settings"] = settingsDoc;
        }
    }

    JsonObject mode = observations["mode"].to<JsonObject>();
    appendAvailability(mode, snapshot.hasMode);
    if (snapshot.hasMode) {
        // ArduinoJson copies mutable character buffers into the document.
        // Keep the single-byte glyph allocation-free at the source without
        // linking the document to a temporary stack buffer.
        char modeValue[2] = {snapshot.mode, '\0'};
        mode["value"] = modeValue;
    }
    JsonObject displayOn = observations["displayOn"].to<JsonObject>();
    appendAvailability(displayOn, snapshot.hasDisplayOn);
    if (snapshot.hasDisplayOn) displayOn["value"] = snapshot.displayOn;
    JsonObject bluetooth = observations["bluetoothIndicator"].to<JsonObject>();
    appendAvailability(bluetooth, snapshot.hasBluetoothIndicator);
    if (snapshot.hasBluetoothIndicator) {
        bluetooth["value"] = snapshot.bluetoothIndicator == V1BluetoothIndicatorState::Off
                                 ? "off"
                                 : (snapshot.bluetoothIndicator == V1BluetoothIndicatorState::On
                                        ? "on"
                                        : "blinking");
    }

    JsonObject currentVolume = observations["currentVolume"].to<JsonObject>();
    currentVolume["available"] = snapshot.hasCurrentVolume;
    if (snapshot.hasCurrentVolume) {
        currentVolume["main"] = snapshot.currentMainVolume;
        currentVolume["muted"] = snapshot.currentMutedVolume;
    } else {
        currentVolume["main"] = nullptr;
        currentVolume["muted"] = nullptr;
    }
    JsonObject sweeps = observations["customFrequencies"].to<JsonObject>();
    sweeps["sectionsAvailable"] = snapshot.hasSweepSections;
    sweeps["maxIndexAvailable"] = snapshot.hasMaxSweepIndex;
    sweeps["definitionsAvailable"] = snapshot.hasSweepDefinitions;
    if (snapshot.hasMaxSweepIndex) sweeps["maxIndex"] = snapshot.maxSweepIndex;
    if (snapshot.hasSweepSections) {
        JsonArray sections = sweeps["sections"].to<JsonArray>();
        for (uint8_t index = 0; index < snapshot.sweepSectionCount; ++index) {
            JsonObject section = sections.add<JsonObject>();
            section["index"] = snapshot.sweepSections[index].index;
            section["count"] = snapshot.sweepSections[index].count;
            section["lowerMHz"] = snapshot.sweepSections[index].lowerMHz;
            section["upperMHz"] = snapshot.sweepSections[index].upperMHz;
            section["unused"] = snapshot.sweepSections[index].lowerMHz == 0 &&
                                snapshot.sweepSections[index].upperMHz == 0;
        }
    }
    if (snapshot.hasSweepDefinitions) {
        JsonArray definitions = sweeps["effectiveDefinitions"].to<JsonArray>();
        for (uint8_t index = 0; index <= snapshot.maxSweepIndex; ++index) {
            JsonObject definition = definitions.add<JsonObject>();
            definition["index"] = index;
            definition["lowerMHz"] = snapshot.sweepDefinitions[index].lowerMHz;
            definition["upperMHz"] = snapshot.sweepDefinitions[index].upperMHz;
        }
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

    if (doc.overflowed() || measureJson(doc) == 0) {
        server.send(503, "application/json", "{\"error\":\"Captured settings unavailable\"}");
        return;
    }
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

} // namespace

static bool parseProfilePageValues(WebServer& server, const String& after, const String* rawLimit,
                                   size_t& limit) {
    String canonicalAfter;
    if (std::strlen(after.c_str()) != after.length() ||
        (after.length() > 0 &&
         (canonicalizeProfileName(after, canonicalAfter) != ProfileNameStatus::Valid || canonicalAfter != after))) {
        server.send(400, "application/json", "{\"error\":\"Invalid profile page cursor\"}");
        return false;
    }
    if (rawLimit) {
        size_t parsed = 0;
        if (rawLimit->length() == 0 || rawLimit->length() > 2) {
            server.send(400, "application/json", "{\"error\":\"Invalid profile page limit\"}");
            return false;
        }
        for (size_t index = 0; index < rawLimit->length(); ++index) {
            const char character = (*rawLimit)[index];
            if (character < '0' || character > '9') {
                server.send(400, "application/json", "{\"error\":\"Invalid profile page limit\"}");
                return false;
            }
            parsed = parsed * 10u + static_cast<size_t>(character - '0');
        }
        if (parsed == 0 || parsed > V1_PROFILE_CATALOG_MAX_COUNT) {
            server.send(400, "application/json", "{\"error\":\"Invalid profile page limit\"}");
            return false;
        }
        limit = parsed;
    }
    return true;
}

static void handleApiProfilesListResolved(WebServer& server, const Runtime& runtime,
                                          const String& after, size_t limit) {
    std::vector<String> profileNames;
    size_t total = 0;
    bool hasMore = false;
    String nextCursor;
    if (runtime.listProfilePageResult) {
        ProfilePageResult page =
            runtime.listProfilePageResult(after, limit, runtime.listProfilePageResultCtx);
        if (!page.success()) {
            const int httpStatus = page.status == ProfileStorageStatus::Busy ? 409
                                   : page.status == ProfileStorageStatus::InvalidName ? 400
                                   : 500;
            server.send(httpStatus, "application/json",
                        page.error.length() > 0 ? String("{\"error\":\"") + page.error + "\"}"
                                                : "{\"error\":\"Profile catalog unavailable\"}");
            return;
        }
        profileNames = std::move(page.profiles);
        total = page.total;
        hasMore = page.hasMore;
        nextCursor = std::move(page.nextCursor);
    } else if (runtime.listProfileNamesResult) {
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
    if (!runtime.listProfilePageResult) {
        std::sort(profileNames.begin(), profileNames.end(), [](const String& lhs, const String& rhs) {
            return std::strcmp(lhs.c_str(), rhs.c_str()) < 0;
        });
        total = profileNames.size();
        std::vector<String> page;
        page.reserve(limit);
        for (const String& name : profileNames) {
            if (after.length() > 0 && std::strcmp(name.c_str(), after.c_str()) <= 0) continue;
            if (page.size() < limit) page.push_back(name);
            else hasMore = true;
        }
        profileNames = std::move(page);
        if (!profileNames.empty()) nextCursor = profileNames.back();
    }
    Serial.printf("[V1Profiles] Listing %d profiles\n", profileNames.size());

    PsramJson::Document doc;
    const bool schemaReady = !runtime.profileSchemaReady ||
                             runtime.profileSchemaReady(runtime.profileSchemaReadyCtx);
    doc["schemaVersion"] = schemaReady ? V1_PROFILE_SCHEMA_VERSION : 1;
    doc["detectorConfigurationOwner"] = schemaReady ? "profile" : "legacy-slot";
    JsonArray array = doc["profiles"].to<JsonArray>();

    for (const String& name : profileNames) {
        ProfileSummary profile;
        if (!runtime.loadProfileSummary ||
            !runtime.loadProfileSummary(name, profile, runtime.loadProfileSummaryCtx)) {
            server.send(500, "application/json", "{\"error\":\"Profile catalog unavailable\"}");
            return;
        }
        JsonObject obj = array.add<JsonObject>();
        obj["name"] = profile.name;
        obj["description"] = profile.description;
        if (doc.overflowed()) {
            server.send(503, "application/json", "{\"error\":\"Profile catalog memory unavailable\"}");
            return;
        }
    }
    doc["total"] = total;
    doc["limit"] = limit;
    doc["hasMore"] = hasMore;
    if (hasMore) doc["nextCursor"] = nextCursor;
    else doc["nextCursor"] = nullptr;
    const size_t responseBytes = measureJson(doc);
    if (doc.overflowed() || responseBytes == 0 || responseBytes > V1_PROFILE_CATALOG_DOCUMENT_MAX_BYTES) {
        server.send(doc.overflowed() ? 503 : 500, "application/json",
                    doc.overflowed() ? "{\"error\":\"Profile catalog memory unavailable\"}"
                                     : "{\"error\":\"Profile catalog exceeds supported envelope\"}");
        return;
    }
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiProfilesListQuery(WebServer& server, const Runtime& runtime,
                                const uint8_t* query, size_t querySize) {
    String after;
    String rawLimit;
    const String* rawLimitPtr = nullptr;
    if (querySize != 0) {
        const ExactUrlEncodedForm form(query, querySize);
        static constexpr const char* ALLOWED[] = {"after", "limit"};
        if (!form.valid() || !form.hasOnly(ALLOWED, sizeof(ALLOWED) / sizeof(ALLOWED[0])) ||
            (form.has("after") && !form.read("after", after)) ||
            (form.has("limit") && !form.read("limit", rawLimit))) {
            server.send(400, "application/json", "{\"error\":\"Invalid profile page query\"}");
            return;
        }
        if (form.has("limit")) rawLimitPtr = &rawLimit;
    }
    size_t limit = V1_PROFILE_CATALOG_MAX_COUNT;
    if (!parseProfilePageValues(server, after, rawLimitPtr, limit)) return;
    handleApiProfilesListResolved(server, runtime, after, limit);
}

static void handleApiProfileGetResolved(WebServer& server, const Runtime& runtime,
                                        const String& rawName) {
    if (rawName.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"Missing profile name\"}");
        return;
    }

    String name;
    const ProfileNameStatus nameStatus = canonicalizeProfileName(rawName, name);
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

void handleApiProfileGetQuery(WebServer& server, const Runtime& runtime,
                              const uint8_t* query, size_t querySize) {
    const ExactUrlEncodedForm form(query, querySize);
    if (!form.valid() || form.fieldCount() != 1u || !form.has("name")) {
        server.send(400, "application/json", "{\"error\":\"Invalid profile query\"}");
        return;
    }
    String rawName;
    if (!form.read("name", rawName)) {
        // The exact parser rejects invalid encodings and allocation failures
        // before a truncated name can select a different existing profile.
        server.send(400, "application/json", "{\"error\":\"Invalid profile name\"}");
        return;
    }
    handleApiProfileGetResolved(server, runtime, rawName);
}

void handleApiProfileSaveBody(WebServer& server, const Runtime& runtime, const uint8_t* body,
                              const size_t bodySize, bool (*checkRateLimit)(void* ctx),
                              void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;
    if (runtime.profileSchemaReady && !runtime.profileSchemaReady(runtime.profileSchemaReadyCtx)) {
        server.send(409, "application/json",
                    "{\"error\":\"Profile settings migration is pending; saving is temporarily read-only\"}");
        return;
    }

    if (!body && bodySize != 0) {
        server.send(400, "application/json", "{\"error\":\"Missing request body\"}");
        return;
    }

    // A complete 64-entry profile needs more than the old 4 KiB cap, but a
    // single profile does not justify the transport-wide 128 KiB envelope.
    if (bodySize > V1_PROFILE_HTTP_SAVE_MAX_BYTES) {
        server.send(400, "application/json", "{\"error\":\"Payload too large\"}");
        return;
    }
    Serial.printf("[V1Settings] Save request body accepted (%u bytes)\n", static_cast<unsigned>(bodySize));

    const char* bodyData = reinterpret_cast<const char*>(body);
    const ExactJsonInput::Status exact = ExactJsonInput::validate(bodyData, bodySize);
    if (exact == ExactJsonInput::Status::MemoryUnavailable) {
        server.send(503, "application/json", "{\"error\":\"Profile parse memory unavailable\",\"retryable\":true}");
        return;
    }
    if (exact != ExactJsonInput::Status::Ok) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    PsramJson::Document doc;
    DeserializationError err = deserializeJson(doc, bodyData, bodySize);

    if (doc.overflowed() || err == DeserializationError::NoMemory) {
        server.send(503, "application/json", "{\"error\":\"Profile parse memory unavailable\",\"retryable\":true}");
        return;
    }
    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    static constexpr const char* ROOT_KEYS[] = {
        "schemaVersion", "name", "description", "detector", "settings",
    };
    if (!doc.is<JsonObjectConst>() ||
        !objectHasOnlyKeys(doc.as<JsonObjectConst>(), ROOT_KEYS,
                           sizeof(ROOT_KEYS) / sizeof(ROOT_KEYS[0])) ||
        !doc["settings"].is<JsonObjectConst>() ||
        !profileSettingsKeysAreKnown(doc["settings"].as<JsonObjectConst>())) {
        server.send(400, "application/json", "{\"error\":\"Invalid profile schema\"}");
        return;
    }

    String requestedName;
    if (!exactV1JsonString(doc["name"], requestedName, MAX_PROFILE_NAME_LEN)) {
        server.send(400, "application/json", "{\"error\":\"Invalid profile name\"}");
        return;
    }
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
    const JsonVariantConst detectorValue = doc["detector"];
    const bool hasDescription = !descriptionValue.isUnbound();
    const bool hasDetector = !detectorValue.isUnbound();
    String parsedDescription;
    if ((hasDescription &&
         !exactV1JsonString(descriptionValue, parsedDescription, V1_PROFILE_DESCRIPTION_MAX_BYTES)) ||
        (hasDetector && !detectorValue.is<JsonObjectConst>()) ||
        (!doc["schemaVersion"].isUnbound() &&
         (!doc["schemaVersion"].is<int>() ||
          doc["schemaVersion"].as<int>() != V1_PROFILE_SCHEMA_VERSION)) ||
        !doc["displayOn"].isUnbound() || !doc["mainVolume"].isUnbound() ||
        !doc["mutedVolume"].isUnbound()) {
        server.send(400, "application/json", "{\"error\":\"Invalid profile metadata\"}");
        return;
    }

    PsramJson::Document existingDoc;
    const bool needsExisting = !hasDescription || !hasDetector;
    if (needsExisting) {
        String existingJson;
        CatalogStatus status = CatalogStatus::NotFound;
        if (runtime.loadProfileJsonResult) {
            status = runtime.loadProfileJsonResult(name, existingJson, runtime.loadProfileJsonResultCtx);
        } else if (runtime.loadProfileJson && runtime.loadProfileJson(name, existingJson, runtime.loadProfileJsonCtx)) {
            status = CatalogStatus::Success;
        }

        if (status == CatalogStatus::Success &&
            (ExactJsonInput::validate(existingJson.c_str(), existingJson.length()) != ExactJsonInput::Status::Ok ||
             deserializeJson(existingDoc, existingJson.c_str(), existingJson.length()) || existingDoc.overflowed())) {
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

    String description;
    if (hasDescription) {
        description = std::move(parsedDescription);
    } else if (existingDoc["description"].isUnbound()) {
        description = "";
    } else if (!exactV1JsonString(existingDoc["description"], description,
                                  V1_PROFILE_DESCRIPTION_MAX_BYTES)) {
        server.send(500, "application/json", "{\"error\":\"Existing profile metadata is corrupt\"}");
        return;
    }
    V1DetectorConfiguration detector;
    if (hasDetector) {
        if (!parseV1DetectorConfiguration(detectorValue.as<JsonObjectConst>(), detector)) {
            server.send(400, "application/json", "{\"error\":\"Invalid detector configuration\"}");
            return;
        }
    } else if (existingDoc["detector"].is<JsonObjectConst>() &&
               !parseV1DetectorConfiguration(existingDoc["detector"].as<JsonObjectConst>(), detector)) {
        server.send(500, "application/json", "{\"error\":\"Existing detector configuration is corrupt\"}");
        return;
    }
    uint8_t settingsBytes[6];
    memset(settingsBytes, 0xFF, sizeof(settingsBytes));

    // Schema-v3 writes use one explicit nested settings object. Keeping
    // metadata and setting names in disjoint objects lets typos and conflicting
    // representations fail before the persistence callback is reached.
    JsonObject settingsObj = doc["settings"].as<JsonObject>();
    if (!runtime.parseSettingsJson(settingsObj, settingsBytes, runtime.parseSettingsJsonCtx)) {
        server.send(400, "application/json", "{\"error\":\"Invalid settings\"}");
        return;
    }

    String saveError;
    if (runtime.saveProfile(name, description, detector, settingsBytes, saveError,
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
        const bool conflict = saveError.indexOf("busy") >= 0 || saveError == V1_PROFILE_CATALOG_LIMIT_ERROR;
        WifiApiResponse::sendJsonDocument(server, conflict ? 409 : 500, errorDoc);
    }
}

void handleApiProfileSave(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                          void* rateLimitCtx) {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing request body\"}");
        return;
    }
    const String body = server.arg("plain");
    handleApiProfileSaveBody(server, runtime, reinterpret_cast<const uint8_t*>(body.c_str()), body.length(),
                             checkRateLimit, rateLimitCtx);
}

void handleApiProfileDeleteBody(WebServer& server, const Runtime& runtime, const uint8_t* body,
                                const size_t bodySize, bool (*checkRateLimit)(void* ctx),
                                void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx))
        return;
    if (!body && bodySize != 0) {
        server.send(400, "application/json", "{\"error\":\"Missing request body\"}");
        return;
    }

    // Keep a tighter semantic limit than the transport-wide ingress cap.
    if (bodySize > V1_PROFILE_HTTP_DELETE_MAX_BYTES) {
        server.send(400, "application/json", "{\"error\":\"Payload too large\"}");
        return;
    }
    const char* bodyData = reinterpret_cast<const char*>(body);
    const ExactJsonInput::Status exact = ExactJsonInput::validate(bodyData, bodySize);
    if (exact == ExactJsonInput::Status::MemoryUnavailable) {
        server.send(503, "application/json", "{\"error\":\"Profile parse memory unavailable\",\"retryable\":true}");
        return;
    }
    if (exact != ExactJsonInput::Status::Ok) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    PsramJson::Document doc;
    DeserializationError err = deserializeJson(doc, bodyData, bodySize);
    if (doc.overflowed() || err == DeserializationError::NoMemory) {
        server.send(503, "application/json", "{\"error\":\"Profile parse memory unavailable\",\"retryable\":true}");
        return;
    }
    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    String requestedName;
    if (!doc.is<JsonObjectConst>() || doc.size() != 1u ||
        !exactV1JsonString(doc["name"], requestedName, MAX_PROFILE_NAME_LEN)) {
        server.send(400, "application/json", "{\"error\":\"Invalid profile name\"}");
        return;
    }
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

void handleApiProfileDelete(WebServer& server, const Runtime& runtime, bool (*checkRateLimit)(void* ctx),
                            void* rateLimitCtx) {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing request body\"}");
        return;
    }
    const String body = server.arg("plain");
    handleApiProfileDeleteBody(server, runtime, reinterpret_cast<const uint8_t*>(body.c_str()), body.length(),
                               checkRateLimit, rateLimitCtx);
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
        if (doc.overflowed() || measureJson(doc) == 0) {
            server.send(503, "application/json", "{\"error\":\"Captured settings unavailable\"}");
            return;
        }
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

    if (doc.overflowed() || measureJson(doc) == 0) {
        server.send(503, "application/json", "{\"error\":\"Captured settings unavailable\"}");
        return;
    }
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

} // namespace WifiV1ProfileApiService
