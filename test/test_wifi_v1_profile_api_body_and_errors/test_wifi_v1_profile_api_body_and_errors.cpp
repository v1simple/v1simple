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

    int parseSettingsCalls = 0;
    int saveCalls = 0;
    int backupCalls = 0;
    bool connected = true;
    String savedDescription;
    bool savedDisplayOn = true;
    uint8_t savedMainVolume = 0xFF;
    uint8_t savedMutedVolume = 0xFF;
    WifiV1ProfileApiService::CatalogStatus loadStatus =
        WifiV1ProfileApiService::CatalogStatus::NotFound;
    String existingProfileJson;
    WifiV1ProfileApiService::CatalogStatus deleteStatus =
        WifiV1ProfileApiService::CatalogStatus::Success;
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
                             bool displayOn,
                             uint8_t mainVolume,
                             uint8_t mutedVolume,
                             const uint8_t /*inBytes*/[6],
                             String& error,
                             void* ctx) {
        auto* rtp = static_cast<FakeRuntime*>(ctx);
        rtp->saveCalls++;
        rtp->savedDescription = description;
        rtp->savedDisplayOn = displayOn;
        rtp->savedMainVolume = mainVolume;
        rtp->savedMutedVolume = mutedVolume;
        if (!rtp->saveOk) {
            error = rtp->saveError;
            return false;
        }
        return true;
    };
    runtime.saveProfileCtx = &rt;
    runtime.loadProfileJsonResult = [](const String&, String& json, void* ctx) {
        auto* rtp = static_cast<FakeRuntime*>(ctx);
        json = rtp->existingProfileJson;
        return rtp->loadStatus;
    };
    runtime.loadProfileJsonResultCtx = &rt;
    runtime.backupToSd = [](void* ctx) { static_cast<FakeRuntime*>(ctx)->backupCalls++; };
    runtime.backupToSdCtx = &rt;
    runtime.v1Connected = [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->connected; };
    runtime.v1ConnectedCtx = &rt;
    runtime.deleteProfileResult = [](const String&, void* ctx) {
        return static_cast<FakeRuntime*>(ctx)->deleteStatus;
    };
    runtime.deleteProfileResultCtx = &rt;
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
    json += "\",\"settings\":{\"byte0\":1}}";
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
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"settings\":{\"byte0\":3}}");

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
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"settings\":{\"byte0\":3}}");

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
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"settings\":{\"byte0\":3}}");

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

// ---------------------------------------------------------------------------
// Oversize-payload caps
// ---------------------------------------------------------------------------

void test_profile_save_rejects_oversize_payload_without_saving() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", oversizeBody(4200));

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Payload too large"));
    TEST_ASSERT_EQUAL_INT(0, rt.parseSettingsCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
}

void test_profile_save_accepts_payload_just_under_the_cap() {
    WebServer server(80);
    FakeRuntime rt;
    // 4000 bytes of filler plus the JSON scaffolding stays under 4096.
    server.setArg("plain", oversizeBody(4000));

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.saveCalls);
}

void test_profile_save_preserves_omitted_existing_metadata() {
    WebServer server(80);
    FakeRuntime rt;
    rt.loadStatus = WifiV1ProfileApiService::CatalogStatus::Success;
    rt.existingProfileJson =
        "{\"name\":\"RoadTrip\",\"description\":\"Existing\",\"displayOn\":false,"
        "\"mainVolume\":7,\"mutedVolume\":2,\"settings\":{}}";
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"settings\":{\"byte0\":3}}");

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.saveCalls);
    TEST_ASSERT_EQUAL_STRING("Existing", rt.savedDescription.c_str());
    TEST_ASSERT_FALSE(rt.savedDisplayOn);
    TEST_ASSERT_EQUAL_UINT8(7, rt.savedMainVolume);
    TEST_ASSERT_EQUAL_UINT8(2, rt.savedMutedVolume);
}

void test_profile_save_accepts_explicit_metadata_without_resetting_it() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain",
                  "{\"name\":\"RoadTrip\",\"description\":\"Edited\",\"displayOn\":false,"
                  "\"mainVolume\":8,\"mutedVolume\":3,\"settings\":{\"byte0\":3}}");

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("Edited", rt.savedDescription.c_str());
    TEST_ASSERT_FALSE(rt.savedDisplayOn);
    TEST_ASSERT_EQUAL_UINT8(8, rt.savedMainVolume);
    TEST_ASSERT_EQUAL_UINT8(3, rt.savedMutedVolume);
}

void test_profile_save_rejects_invalid_volume_metadata() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"mainVolume\":10,\"settings\":{\"byte0\":3}}");

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
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
    RUN_TEST(test_profile_save_rejects_oversize_payload_without_saving);
    RUN_TEST(test_profile_save_accepts_payload_just_under_the_cap);
    RUN_TEST(test_profile_save_preserves_omitted_existing_metadata);
    RUN_TEST(test_profile_save_accepts_explicit_metadata_without_resetting_it);
    RUN_TEST(test_profile_save_rejects_invalid_volume_metadata);
    RUN_TEST(test_post_handlers_bind_the_request_body_exactly_once_per_handler);
    RUN_TEST(test_body_caps_are_backstopped_by_socket_preflight_before_framework_parser);
    return UNITY_END();
}
