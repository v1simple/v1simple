// Regression coverage for V1 profile HTTP response and request-body handling.
//
// handleApiProfileSave once built its 500 response by concatenating saveError
// directly into JSON. Quotes, backslashes, or control characters could then
// produce malformed JSON.
//
// Arduino WebServer buffers the whole body before dispatch, and
// WebServer::arg() returns String by value. Binding it once into a named local
// is the minimum request-body allocation achievable at this layer. The suite
// pins the payload caps and one-binding-per-handler contract.

#include <unity.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../mocks/mock_heap_caps_state.h"
#include "../mocks/esp_heap_caps.h"
#include "../../src/modules/wifi/wifi_json_document.h"
#include "../../src/modules/wifi/wifi_v1_profile_api_service.h"
#include "../../src/modules/wifi/wifi_v1_profile_api_service.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace {

struct FakeRuntime {
    bool parseSettingsOk = true;
    bool saveOk = true;
    String saveError = "";
    bool savedCreateOnly = false;
    bool slotVolumeOverrideConflict = false;

    int parseSettingsCalls = 0;
    int saveCalls = 0;
    int deleteCalls = 0;
    int backupCalls = 0;
    bool connected = true;
    String savedDescription;
    bool savedDisplayOn = true;
    uint8_t savedMainVolume = 0xFF;
    uint8_t savedMutedVolume = 0xFF;
    V1DetectorConfiguration savedDetector;
    WifiV1ProfileApiService::CatalogStatus loadStatus =
        WifiV1ProfileApiService::CatalogStatus::NotFound;
    String existingProfileJson;
    String loadedProfileName;
    int loadCalls = 0;
    WifiV1ProfileApiService::CatalogStatus deleteStatus =
        WifiV1ProfileApiService::CatalogStatus::Success;
    bool capturedSnapshotAvailable = false;
    bool capturedSourceAvailable = true;
    bool profileSchemaReady = true;
    V1DeviceRecord capturedDevice;
};

WifiV1ProfileApiService::Runtime makeRuntime(FakeRuntime& rt) {
    WifiV1ProfileApiService::Runtime runtime{};
    runtime.parseSettingsJson = [](const JsonObject& /*settingsObj*/, uint8_t outBytes[6], void* ctx) {
        auto* rtp = static_cast<FakeRuntime*>(ctx);
        rtp->parseSettingsCalls++;
        if (!rtp->parseSettingsOk) {
            return false;
        }
        memset(outBytes, 0xFF, 6);
        return true;
    };
    runtime.parseSettingsJsonCtx = &rt;
    runtime.saveProfile = [](const String& /*name*/,
                             const String& description,
                             const V1DetectorConfiguration& detector,
                             const uint8_t /*inBytes*/[6],
                             bool createOnly,
                             String& error,
                             void* ctx) {
        auto* rtp = static_cast<FakeRuntime*>(ctx);
        rtp->saveCalls++;
        rtp->savedDescription = description;
        rtp->savedDetector = detector;
        rtp->savedCreateOnly = createOnly;
        if (!rtp->saveOk) {
            error = rtp->saveError;
            return false;
        }
        return true;
    };
    runtime.saveProfileCtx = &rt;
    runtime.loadProfileJsonResult = [](const String& name, String& json, void* ctx) {
        auto* rtp = static_cast<FakeRuntime*>(ctx);
        rtp->loadCalls++;
        rtp->loadedProfileName = name;
        json = rtp->existingProfileJson;
        return rtp->loadStatus;
    };
    runtime.loadProfileJsonResultCtx = &rt;
    runtime.backupToSd = [](void* ctx) { static_cast<FakeRuntime*>(ctx)->backupCalls++; };
    runtime.backupToSdCtx = &rt;
    runtime.v1Connected = [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->connected; };
    runtime.v1ConnectedCtx = &rt;
    runtime.deleteProfileResult = [](const String&, void* ctx) {
        auto* rtp = static_cast<FakeRuntime*>(ctx);
        rtp->deleteCalls++;
        return rtp->deleteStatus;
    };
    runtime.deleteProfileResultCtx = &rt;
    runtime.profileSchemaReady = [](void* ctx) {
        return static_cast<FakeRuntime*>(ctx)->profileSchemaReady;
    };
    runtime.profileSchemaReadyCtx = &rt;
    runtime.slotVolumeOverrideConflicts = [](const String&, const V1DetectorConfiguration&, void* ctx) {
        return static_cast<FakeRuntime*>(ctx)->slotVolumeOverrideConflict;
    };
    runtime.slotVolumeOverrideConflictsCtx = &rt;
    return runtime;
}

WifiV1ProfileApiService::Runtime makeCapturedRuntime(FakeRuntime& rt) {
    WifiV1ProfileApiService::Runtime runtime = makeRuntime(rt);
    runtime.loadCapturedSnapshot = [](V1DeviceRecord& device, void* ctx) {
        auto* rtp = static_cast<FakeRuntime*>(ctx);
        device = rtp->capturedDevice;
        return rtp->capturedSnapshotAvailable;
    };
    runtime.loadCapturedSnapshotCtx = &rt;
    runtime.capturedSnapshotSourceAvailable = [](void* ctx) {
        return static_cast<FakeRuntime*>(ctx)->capturedSourceAvailable;
    };
    runtime.capturedSnapshotSourceAvailableCtx = &rt;
    runtime.settingsJsonForBytes = [](const uint8_t[6], String& output, void*) {
        output = "{\"bytes\":[255,254,253,252,251,250],\"xBand\":true}";
        return output.length() == 48;
    };
    return runtime;
}

bool alwaysAllow(void* /*ctx*/) {
    return true;
}

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

// Reparse the response to verify that error text cannot corrupt its JSON shape.
bool responseParsesAsJson(const WebServer& server, JsonDocument& out) {
    return deserializeJson(out, server.lastBody.c_str()) == DeserializationError::Ok;
}

String oversizeBody(size_t totalBytes) {
    std::string filler(totalBytes, 'x');
    std::string json = "{\"name\":\"Size Boundary\",\"description\":\"";
    json += filler;
    json += "\",\"settings\":{\"xBand\":true}}";
    return String(json.c_str());
}

std::string readProjectFile(const char* relativePath) {
    const std::filesystem::path path = std::filesystem::path(PROJECT_DIR) / relativePath;
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

}  // namespace

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    mock_reset_heap_caps();
}

void tearDown() {}

// ---------------------------------------------------------------------------
// Profile-save error JSON must be escaped
// ---------------------------------------------------------------------------

void test_save_error_with_quotes_and_backslashes_stays_valid_json() {
    WebServer server(80);
    FakeRuntime rt;
    rt.saveOk = false;
    rt.saveError = "open \"/profiles/a\\b.json\" failed";
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"settings\":{\"xBand\":true}}");

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);

    JsonDocument parsed;
    TEST_ASSERT_TRUE_MESSAGE(responseParsesAsJson(server, parsed),
                             "500 body must parse as JSON when saveError contains quotes/backslashes");
    TEST_ASSERT_EQUAL_STRING("open \"/profiles/a\\b.json\" failed", parsed["error"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("open \"/profiles/a\\b.json\" failed", parsed["message"].as<const char*>());
}

void test_save_error_with_control_characters_stays_valid_json() {
    WebServer server(80);
    FakeRuntime rt;
    rt.saveOk = false;
    rt.saveError = "write failed\nretry \"once\"\ttab";
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"settings\":{\"xBand\":true}}");

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);

    JsonDocument parsed;
    TEST_ASSERT_TRUE_MESSAGE(responseParsesAsJson(server, parsed),
                             "500 body must parse as JSON when saveError contains control characters");
    TEST_ASSERT_EQUAL_STRING("write failed\nretry \"once\"\ttab", parsed["error"].as<const char*>());
}

void test_plain_save_error_still_reports_error_field_verbatim() {
    WebServer server(80);
    FakeRuntime rt;
    rt.saveOk = false;
    rt.saveError = "disk full";
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"settings\":{\"xBand\":true}}");

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"disk full\""));
    TEST_ASSERT_EQUAL_INT(0, rt.backupCalls);

    JsonDocument parsed;
    TEST_ASSERT_TRUE(responseParsesAsJson(server, parsed));
    TEST_ASSERT_EQUAL_STRING("disk full", parsed["error"].as<const char*>());
}

void test_delete_reports_storage_busy_instead_of_not_found() {
    WebServer server(80);
    FakeRuntime rt;
    rt.deleteStatus = WifiV1ProfileApiService::CatalogStatus::Busy;
    server.setArg("plain", "{\"name\":\"RoadTrip\"}");

    WifiV1ProfileApiService::handleApiProfileDelete(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "storage busy"));
    TEST_ASSERT_EQUAL_INT(0, rt.backupCalls);
}

void test_delete_persistence_failure_is_not_reported_as_success() {
    WebServer server(80);
    FakeRuntime rt;
    rt.deleteStatus = WifiV1ProfileApiService::CatalogStatus::IoError;
    server.setArg("plain", "{\"name\":\"RoadTrip\"}");

    WifiV1ProfileApiService::handleApiProfileDelete(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Profile deletion failed"));
    TEST_ASSERT_EQUAL_INT(0, rt.backupCalls);
}

void test_delete_rejects_unknown_fields_before_storage_mutation() {
    WebServer server(80);
    FakeRuntime rt;
    const char body[] = "{\"name\":\"RoadTrip\",\"naem\":\"Other\"}";

    WifiV1ProfileApiService::handleApiProfileDeleteBody(
        server, makeRuntime(rt), reinterpret_cast<const uint8_t*>(body), sizeof(body) - 1u,
        alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.deleteCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.backupCalls);
}

// ---------------------------------------------------------------------------
// Oversize-payload caps
// ---------------------------------------------------------------------------

void test_profile_save_rejects_oversize_payload_without_saving() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", oversizeBody(V1_PROFILE_HTTP_SAVE_MAX_BYTES + 1u));

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Payload too large"));
    TEST_ASSERT_EQUAL_INT(0, rt.parseSettingsCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
}

void test_profile_save_accepts_maximum_description() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", oversizeBody(V1_PROFILE_DESCRIPTION_MAX_BYTES));

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.saveCalls);
}

void test_profile_save_forwards_create_only_and_reports_existing_name_conflict() {
    for (const bool alreadyExists : {false, true}) {
        WebServer server(80);
        FakeRuntime rt;
        rt.saveOk = !alreadyExists;
        rt.saveError = "Profile already exists";
        server.setArg("plain", "{\"name\":\"RoadTrip\",\"createOnly\":true,\"settings\":{\"xBand\":true}}");

        WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

        TEST_ASSERT_EQUAL_INT(alreadyExists ? 409 : 200, server.lastStatusCode);
        TEST_ASSERT_TRUE(rt.savedCreateOnly);
        TEST_ASSERT_EQUAL_INT(1, rt.saveCalls);
        TEST_ASSERT_EQUAL_INT(alreadyExists ? 0 : 1, rt.backupCalls);
    }
}

void test_profile_save_rejects_non_boolean_create_only() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"createOnly\":\"true\",\"settings\":{\"xBand\":true}}");

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
}

void test_profile_save_rejects_volume_policy_change_conflicting_with_assigned_slot() {
    WebServer server(80);
    FakeRuntime rt;
    rt.slotVolumeOverrideConflict = true;
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"settings\":{\"xBand\":true}}");

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "volume override"));
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.backupCalls);
}

void test_profile_save_rejects_description_above_shared_limit_before_saving() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", oversizeBody(V1_PROFILE_DESCRIPTION_MAX_BYTES + 1u));

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Invalid profile metadata"));
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
}

void test_profile_save_preserves_omitted_existing_metadata() {
    WebServer server(80);
    FakeRuntime rt;
    rt.loadStatus = WifiV1ProfileApiService::CatalogStatus::Success;
    rt.existingProfileJson =
        "{\"schemaVersion\":3,\"name\":\"RoadTrip\",\"description\":\"Existing\","
        "\"detector\":{\"userSettings\":\"value\",\"mode\":{\"policy\":\"value\",\"value\":2},"
        "\"display\":\"off\",\"volume\":{\"policy\":\"temporary\",\"main\":7,\"muted\":2,"
        "\"feedback\":\"none\",\"disconnect\":\"restore_saved\"},"
        "\"bluetoothLed\":\"off\",\"customFrequencies\":{\"policy\":\"unchanged\"}},\"settings\":{}}";
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"settings\":{\"xBand\":true}}");

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.saveCalls);
    TEST_ASSERT_EQUAL_STRING("Existing", rt.savedDescription.c_str());
    TEST_ASSERT_EQUAL_INT(V1ModePolicy::Value, rt.savedDetector.modePolicy);
    TEST_ASSERT_EQUAL_UINT8(2, rt.savedDetector.mode);
    TEST_ASSERT_EQUAL_INT(V1DisplayPolicy::Off, rt.savedDetector.displayPolicy);
    TEST_ASSERT_EQUAL_INT(V1VolumePolicy::Temporary, rt.savedDetector.volumePolicy);
    TEST_ASSERT_EQUAL_UINT8(7, rt.savedDetector.mainVolume);
    TEST_ASSERT_EQUAL_UINT8(2, rt.savedDetector.mutedVolume);
}

void test_profile_save_accepts_explicit_metadata_without_resetting_it() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain",
                  "{\"schemaVersion\":3,\"name\":\"RoadTrip\",\"description\":\"Edited\","
                  "\"detector\":{\"userSettings\":\"unchanged\",\"mode\":{\"policy\":\"unchanged\"},"
                  "\"display\":\"off\",\"volume\":{\"policy\":\"saved\",\"main\":8,\"muted\":3,"
                  "\"feedback\":\"always\",\"disconnect\":\"restore_saved\"},"
                  "\"bluetoothLed\":\"on\",\"customFrequencies\":{\"policy\":\"unchanged\"}},"
                  "\"settings\":{\"xBand\":true}}");

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("Edited", rt.savedDescription.c_str());
    TEST_ASSERT_EQUAL_INT(V1UserSettingsPolicy::Unchanged, rt.savedDetector.userSettingsPolicy);
    TEST_ASSERT_EQUAL_INT(V1DisplayPolicy::Off, rt.savedDetector.displayPolicy);
    TEST_ASSERT_EQUAL_INT(V1VolumePolicy::Saved, rt.savedDetector.volumePolicy);
    TEST_ASSERT_EQUAL_UINT8(8, rt.savedDetector.mainVolume);
    TEST_ASSERT_EQUAL_UINT8(3, rt.savedDetector.mutedVolume);
    TEST_ASSERT_EQUAL_INT(V1VolumeFeedbackPolicy::Always, rt.savedDetector.volumeFeedback);
    TEST_ASSERT_EQUAL_INT(V1BluetoothLedPolicy::On, rt.savedDetector.bluetoothLedPolicy);
}

void test_profile_save_accepts_complete_64_definition_profile_above_old_cap() {
    WebServer server(80);
    FakeRuntime rt;
    JsonDocument request;
    request["schemaVersion"] = V1_PROFILE_SCHEMA_VERSION;
    request["name"] = std::string(MAX_PROFILE_NAME_LEN, 'M');
    request["description"] = std::string(V1_PROFILE_DESCRIPTION_MAX_BYTES, '\"');
    V1DetectorConfiguration detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Value;
    detector.mode = 3;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.volumePolicy = V1VolumePolicy::Temporary;
    detector.mainVolume = 9;
    detector.mutedVolume = 9;
    detector.volumeFeedback = V1VolumeFeedbackPolicy::ChangedOnly;
    detector.volumeDisconnect = V1VolumeDisconnectPolicy::RestoreSaved;
    detector.bluetoothLedPolicy = V1BluetoothLedPolicy::Unchanged;
    detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    for (uint8_t index = 0; index < 64; ++index) {
        detector.customFrequencyDefinitions.push_back({index, 65534, 65535});
    }
    appendV1DetectorConfiguration(request["detector"].to<JsonObject>(), detector);
    JsonObject settings = request["settings"].to<JsonObject>();
    JsonArray baseBytes = settings["baseBytes"].to<JsonArray>();
    const uint8_t maximumRaw[] = {192, 217, 252, 255, 255, 255};
    for (uint8_t byte : maximumRaw) baseBytes.add(byte);
    const char* booleanFields[] = {
        "xBand", "kBand", "kaBand", "laser", "kuBand", "euro", "kVerifier", "laserRear",
        "customFreqs", "kaAlwaysPriority", "fastLaserDetect", "muteToMuteVolume",
        "bogeyLockLoud", "muteXKRear", "startupSequence", "restingDisplay", "bsmPlus", "mrct",
        "driveSafe3D", "driveSafe3DHD", "redflexHalo", "redflexNK7", "ekin", "photoVerifier",
        "gatsoRT4", "photoIntersectionFilter"};
    for (const char* field : booleanFields) settings[field] = false;
    settings["kaSensitivity"] = 3;
    settings["kSensitivity"] = 3;
    settings["xSensitivity"] = 3;
    settings["autoMute"] = 3;
    String body;
    serializeJson(request, body);
    TEST_ASSERT_EQUAL_UINT(12199, body.length());
    TEST_ASSERT_EQUAL_UINT(4185, V1_PROFILE_HTTP_SAVE_MAX_BYTES - body.length());
    TEST_ASSERT_LESS_THAN(V1_PROFILE_HTTP_SAVE_MAX_BYTES, body.length());
    server.setArg("plain", body);

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.saveCalls);
    TEST_ASSERT_EQUAL_INT(V1CustomFrequencyPolicy::Value, rt.savedDetector.customFrequencyPolicy);
    TEST_ASSERT_EQUAL_UINT32(64, static_cast<uint32_t>(rt.savedDetector.customFrequencyDefinitions.size()));
    TEST_ASSERT_EQUAL_UINT8(63, rt.savedDetector.customFrequencyDefinitions.back().index);
}

void test_profile_save_rejects_invalid_volume_metadata() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"mainVolume\":10,\"settings\":{\"byte0\":3}}");

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
}

void test_profile_save_rejects_half_zero_custom_definition_without_saving() {
    WebServer server(80);
    FakeRuntime rt;
    JsonDocument request;
    request["schemaVersion"] = V1_PROFILE_SCHEMA_VERSION;
    request["name"] = "Half Zero";
    V1DetectorConfiguration detector;
    detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    const std::array<V1CustomFrequencyDefinition, 1> definitions{{{0, 24000, 24100}}};
    TEST_ASSERT_TRUE(detector.customFrequencyDefinitions.assign(definitions));
    appendV1DetectorConfiguration(request["detector"].to<JsonObject>(), detector);
    request["detector"]["customFrequencies"]["definitions"][0]["lowerMHz"] = 0;
    request["detector"]["customFrequencies"]["definitions"][0]["upperMHz"] = 1;
    request["settings"]["xBand"] = true;
    String body;
    serializeJson(request, body);
    server.setArg("plain", body);

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
}

void test_profile_save_rejects_unknown_root_or_setting_keys_before_saving() {
    for (const char* body : {
             "{\"name\":\"RoadTrip\",\"settings\":{\"xBand\":false,\"xBnnd\":true}}",
             "{\"name\":\"RoadTrip\",\"settingz\":{\"xBand\":true},\"settings\":{\"xBand\":true}}",
         }) {
        WebServer server(80);
        FakeRuntime rt;
        WifiV1ProfileApiService::handleApiProfileSaveBody(
            server, makeRuntime(rt), reinterpret_cast<const uint8_t*>(body), std::strlen(body),
            alwaysAllow, nullptr);
        TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
        TEST_ASSERT_EQUAL_INT(0, rt.parseSettingsCalls);
        TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
    }
}

void test_profile_save_is_read_only_until_durable_profile_ownership_commits() {
    WebServer server(80);
    FakeRuntime rt;
    rt.profileSchemaReady = false;
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"settings\":{\"xBand\":true}}");

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "migration is pending"));
    TEST_ASSERT_EQUAL_INT(0, rt.parseSettingsCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
}

void test_profile_delete_remains_available_to_prune_a_catalog_while_migration_is_pending() {
    WebServer server(80);
    FakeRuntime rt;
    rt.profileSchemaReady = false;
    server.setArg("plain", "{\"name\":\"RoadTrip\"}");

    WifiV1ProfileApiService::handleApiProfileDelete(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.deleteCalls);
}

void test_current_endpoint_returns_persisted_snapshot_with_explicit_provenance_and_availability() {
    WebServer server(80);
    FakeRuntime rt;
    rt.capturedSnapshotAvailable = true;
    rt.capturedDevice.address = "AA:BB:CC:DD:EE:FF";
    rt.capturedDevice.name = "Road V1";
    V1DetectorSnapshot& snapshot = rt.capturedDevice.snapshot;
    snapshot.available = true;
    snapshot.capturedBootId = 41;
    snapshot.capturedUptimeMs = 1234;
    snapshot.sessionGeneration = 7;
    snapshot.hasFirmwareVersion = true;
    snapshot.firmwareVersion = 41039;
    snapshot.hasUserBytes = true;
    snapshot.userBytes = {{0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA}};
    snapshot.hasMode = true;
    snapshot.mode = 'A';
    snapshot.hasDisplayOn = true;
    snapshot.displayOn = false;
    snapshot.hasCurrentVolume = true;
    snapshot.currentMainVolume = 7;
    snapshot.currentMutedVolume = 2;
    snapshot.hasSweepSections = true;
    snapshot.sweepSectionCount = 3;
    snapshot.sweepSections = {{{0, 3, 23900, 25000}, {1, 3, 0, 0}, {2, 3, 33000, 37000}}};

    WifiV1ProfileApiService::handleApiCurrentSettings(server, makeCapturedRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    JsonDocument parsed;
    TEST_ASSERT_TRUE(responseParsesAsJson(server, parsed));
    TEST_ASSERT_TRUE(parsed["available"].as<bool>());
    TEST_ASSERT_FALSE(parsed["connected"].as<bool>());
    TEST_ASSERT_FALSE(parsed["live"].as<bool>());
    TEST_ASSERT_TRUE(parsed["stale"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("previous-boot", parsed["staleness"].as<const char*>());
    TEST_ASSERT_EQUAL_UINT32(41, parsed["provenance"]["capturedBootId"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(41039, parsed["firmware"]["value"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT8(6, parsed["capabilities"]["supportedUserByteCount"].as<uint8_t>());
    TEST_ASSERT_TRUE(parsed["capabilities"]["volumeChange"].as<bool>());
    TEST_ASSERT_TRUE(parsed["capabilities"]["allVolume"].as<bool>());
    TEST_ASSERT_TRUE(parsed["capabilities"]["detectorFactoryResetWorkflowAvailable"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("profile_draft_only",
                             parsed["capabilities"]["localDefaultsScope"].as<const char*>());
    TEST_ASSERT_TRUE(parsed["capabilities"]["settings"]["gatsoRT4"].as<bool>());
    TEST_ASSERT_EQUAL_UINT8(0xFA, parsed["observations"]["userBytes"]["value"][5].as<uint8_t>());
    TEST_ASSERT_EQUAL_STRING("A", parsed["observations"]["mode"]["value"].as<const char*>());
    TEST_ASSERT_FALSE(parsed["observations"]["displayOn"]["value"].as<bool>());
    TEST_ASSERT_FALSE(parsed["observations"]["savedVolume"]["available"].as<bool>());
    TEST_ASSERT_TRUE(parsed["observations"]["savedVolume"]["main"].isNull());
    TEST_ASSERT_TRUE(parsed["observations"]["customFrequencies"]["sectionsAvailable"].as<bool>());
    TEST_ASSERT_TRUE(parsed["observations"]["customFrequencies"]["sections"][1]["unused"].as<bool>());
    TEST_ASSERT_EQUAL_UINT8(
        3, parsed["observations"]["customFrequencies"]["sections"][1]["count"].as<uint8_t>());
    TEST_ASSERT_EQUAL_UINT16(
        0, parsed["observations"]["customFrequencies"]["sections"][1]["lowerMHz"].as<uint16_t>());
    TEST_ASSERT_EQUAL_UINT16(
        0, parsed["observations"]["customFrequencies"]["sections"][1]["upperMHz"].as<uint16_t>());
    TEST_ASSERT_TRUE(parsed["settings"]["xBand"].as<bool>());

    const std::string source =
        readProjectFile("src/modules/wifi/wifi_v1_profile_api_service.cpp");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("char modeValue[2]"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("const char modeValue[2]"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("String(snapshot.mode)"));
}

void test_current_endpoint_reports_not_captured_without_live_fallback() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true; // A maintenance API must not use this as live proof.

    WifiV1ProfileApiService::handleApiCurrentSettings(server, makeCapturedRuntime(rt));

    JsonDocument parsed;
    TEST_ASSERT_TRUE(responseParsesAsJson(server, parsed));
    TEST_ASSERT_FALSE(parsed["available"].as<bool>());
    TEST_ASSERT_FALSE(parsed["connected"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("not-captured", parsed["staleness"].as<const char*>());
}

void test_current_endpoint_keeps_future_firmware_bytes_raw_but_unqualified() {
    WebServer server(80);
    FakeRuntime rt;
    rt.capturedSnapshotAvailable = true;
    rt.capturedDevice.address = "AA:BB:CC:DD:EE:FF";
    V1DetectorSnapshot& snapshot = rt.capturedDevice.snapshot;
    snapshot.available = true;
    snapshot.hasFirmwareVersion = true;
    snapshot.firmwareVersion = 50000;
    snapshot.hasUserBytes = true;
    snapshot.userBytes = {{1, 2, 3, 4, 5, 6}};

    WifiV1ProfileApiService::handleApiCurrentSettings(server, makeCapturedRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    JsonDocument parsed;
    TEST_ASSERT_TRUE(responseParsesAsJson(server, parsed));
    TEST_ASSERT_TRUE(parsed["capabilities"]["versionKnown"].as<bool>());
    TEST_ASSERT_FALSE(parsed["capabilities"]["gen2"].as<bool>());
    TEST_ASSERT_EQUAL_UINT8(0, parsed["capabilities"]["supportedUserByteCount"].as<uint8_t>());
    TEST_ASSERT_TRUE(parsed["observations"]["userBytes"]["available"].as<bool>());
    TEST_ASSERT_EQUAL_UINT8(6, parsed["observations"]["userBytes"]["value"][5].as<uint8_t>());
}

void test_current_endpoint_reports_storage_unavailable_instead_of_not_captured() {
    WebServer server(80);
    FakeRuntime rt;
    rt.capturedSourceAvailable = false;

    WifiV1ProfileApiService::handleApiCurrentSettings(server, makeCapturedRuntime(rt));

    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "storage unavailable"));
    TEST_ASSERT_FALSE(responseContains(server, "not-captured"));
}

void test_profile_get_uses_exact_query_bytes_and_decodes_one_name() {
    WebServer server(80);
    FakeRuntime rt;
    rt.loadStatus = WifiV1ProfileApiService::CatalogStatus::Success;
    rt.existingProfileJson = "{\"name\":\"Road Trip\"}";
    const char query[] = "name=Road%20Trip";

    WifiV1ProfileApiService::handleApiProfileGetQuery(
        server, makeRuntime(rt), reinterpret_cast<const uint8_t*>(query), sizeof(query) - 1u);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.loadCalls);
    TEST_ASSERT_EQUAL_STRING("Road Trip", rt.loadedProfileName.c_str());
}

void test_profile_get_rejects_duplicate_unknown_or_truncated_query_before_lookup() {
    const char* invalidQueries[] = {
        "name=Road&name=Trip",
        "name=Road&extra=1",
        "name=Road%",
        "name=Road%00Trip",
        "name=Road%FF",
    };
    for (const char* query : invalidQueries) {
        WebServer server(80);
        FakeRuntime rt;
        rt.loadStatus = WifiV1ProfileApiService::CatalogStatus::Success;
        WifiV1ProfileApiService::handleApiProfileGetQuery(
            server, makeRuntime(rt), reinterpret_cast<const uint8_t*>(query), std::strlen(query));
        TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
        TEST_ASSERT_EQUAL_INT(0, rt.loadCalls);
    }
}

// ---------------------------------------------------------------------------
// Request-body allocation contract
// ---------------------------------------------------------------------------

// WebServer::arg() returns String by value, so every call allocates a full body
// copy. Each handler must bind the result once and reuse that binding.
void test_post_handlers_bind_the_request_body_exactly_once_per_handler() {
    struct Expectation {
        const char* path;
        int maxArgCalls; // one per handler that reads a body
    };
    const Expectation sources[] = {
        // save and delete
        {"src/modules/wifi/wifi_v1_profile_api_service.cpp", 2},
        // restore (backup-now takes no body)
        {"src/modules/wifi/backup_api_service.cpp", 1},
    };

    for (const Expectation& expected : sources) {
        const std::string source = readProjectFile(expected.path);
        TEST_ASSERT_FALSE_MESSAGE(source.empty(), expected.path);

        int argCalls = 0;
        const std::string needle = "server.arg(\"plain\")";
        for (size_t at = source.find(needle); at != std::string::npos; at = source.find(needle, at + 1)) {
            ++argCalls;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected.maxArgCalls, argCalls, expected.path);

        // Require a named binding rather than repeated inline temporaries.
        TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos,
                                      source.find("const String body = server.arg(\"plain\")"),
                                      expected.path);
    }
}

void test_body_caps_are_backstopped_by_socket_preflight_before_framework_parser() {
    const std::string serverSource =
        readProjectFile("src/modules/wifi/wifi_maintenance_web_server.h");
    const std::string policySource =
        readProjectFile("src/modules/wifi/wifi_maintenance_http_preflight.h");
    TEST_ASSERT_FALSE(serverSource.empty());
    TEST_ASSERT_FALSE(policySource.empty());

    const size_t inspectCall = serverSource.find("inspectCurrentRequest()");
    const size_t frameworkParse = serverSource.find("_parseRequest(_currentClient)");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, inspectCall);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, frameworkParse);
    TEST_ASSERT_TRUE(inspectCall < frameworkParse);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, policySource.find("RejectMultipart"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, policySource.find("kMaxBodyBytes"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_save_error_with_quotes_and_backslashes_stays_valid_json);
    RUN_TEST(test_save_error_with_control_characters_stays_valid_json);
    RUN_TEST(test_plain_save_error_still_reports_error_field_verbatim);
    RUN_TEST(test_delete_reports_storage_busy_instead_of_not_found);
    RUN_TEST(test_delete_persistence_failure_is_not_reported_as_success);
    RUN_TEST(test_delete_rejects_unknown_fields_before_storage_mutation);
    RUN_TEST(test_profile_save_rejects_oversize_payload_without_saving);
    RUN_TEST(test_profile_save_accepts_maximum_description);
    RUN_TEST(test_profile_save_forwards_create_only_and_reports_existing_name_conflict);
    RUN_TEST(test_profile_save_rejects_non_boolean_create_only);
    RUN_TEST(test_profile_save_rejects_volume_policy_change_conflicting_with_assigned_slot);
    RUN_TEST(test_profile_save_rejects_description_above_shared_limit_before_saving);
    RUN_TEST(test_profile_save_preserves_omitted_existing_metadata);
    RUN_TEST(test_profile_save_accepts_explicit_metadata_without_resetting_it);
    RUN_TEST(test_profile_save_accepts_complete_64_definition_profile_above_old_cap);
    RUN_TEST(test_profile_save_rejects_invalid_volume_metadata);
    RUN_TEST(test_profile_save_rejects_half_zero_custom_definition_without_saving);
    RUN_TEST(test_profile_save_rejects_unknown_root_or_setting_keys_before_saving);
    RUN_TEST(test_profile_save_is_read_only_until_durable_profile_ownership_commits);
    RUN_TEST(test_profile_delete_remains_available_to_prune_a_catalog_while_migration_is_pending);
    RUN_TEST(test_current_endpoint_returns_persisted_snapshot_with_explicit_provenance_and_availability);
    RUN_TEST(test_current_endpoint_reports_not_captured_without_live_fallback);
    RUN_TEST(test_current_endpoint_keeps_future_firmware_bytes_raw_but_unqualified);
    RUN_TEST(test_current_endpoint_reports_storage_unavailable_instead_of_not_captured);
    RUN_TEST(test_profile_get_uses_exact_query_bytes_and_decodes_one_name);
    RUN_TEST(test_profile_get_rejects_duplicate_unknown_or_truncated_query_before_lookup);
    RUN_TEST(test_post_handlers_bind_the_request_body_exactly_once_per_handler);
    RUN_TEST(test_body_caps_are_backstopped_by_socket_preflight_before_framework_parser);
    return UNITY_END();
}
