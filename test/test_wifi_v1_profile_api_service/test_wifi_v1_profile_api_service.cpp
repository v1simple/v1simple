#include <unity.h>
#include <cstring>
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

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

static void assertWifiJsonAllocationsReleased() {
    TEST_ASSERT_GREATER_THAN_UINT32(0u, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(WifiJson::kPsramCaps, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_heap_caps_outstanding_allocations);
}

struct StoredProfile {
    String name;
    String description;
    bool displayOn = true;
    String profileJson;
};

struct FakeRuntime {
    std::vector<String> listNames;
    std::vector<StoredProfile> storedProfiles;

    bool parseSettingsOk = true;
    bool saveOk = true;
    String saveError = "";
    bool deleteResult = true;

    bool hasCurrent = false;
    String currentSettingsJson = "{}";
    bool connected = false;
    bool requestUserBytesResult = false;
    bool writeUserBytesResult = false;
    bool loadProfileSettingsResult = false;
    bool storedProfileDisplayOn = true;
    uint8_t storedProfileBytes[6] = {0};

    int listCalls = 0;
    int loadSummaryCalls = 0;
    int loadJsonCalls = 0;
    int loadProfileSettingsCalls = 0;
    int parseSettingsCalls = 0;
    int saveCalls = 0;
    int deleteCalls = 0;
    int requestDeferredBackupCalls = 0;
    int requestUserBytesCalls = 0;
    int writeUserBytesCalls = 0;
    int setDisplayOnCalls = 0;

    String lastSaveName;
    String lastSaveDescription;
    bool lastSaveDisplayOn = true;
    uint8_t lastSaveBytes[6] = {0};
    bool lastSetDisplayOn = true;
    uint8_t lastWrittenBytes[6] = {0};
};

static bool fakeLoadSummary(FakeRuntime& rt, const String& name, WifiV1ProfileApiService::ProfileSummary& out) {
    rt.loadSummaryCalls++;
    for (const auto& p : rt.storedProfiles) {
        if (p.name == name) {
            out.name = p.name;
            out.description = p.description;
            out.displayOn = p.displayOn;
            return true;
        }
    }
    return false;
}

static bool fakeLoadJson(FakeRuntime& rt, const String& name, String& outJson) {
    rt.loadJsonCalls++;
    for (const auto& p : rt.storedProfiles) {
        if (p.name == name) {
            outJson = p.profileJson;
            return true;
        }
    }
    return false;
}

static WifiV1ProfileApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiV1ProfileApiService::Runtime{
        [](void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->listCalls++;
            return rtp->listNames;
        }, &rt,
        [](const String& name, WifiV1ProfileApiService::ProfileSummary& summary, void* ctx) {
            return fakeLoadSummary(*static_cast<FakeRuntime*>(ctx), name, summary);
        }, &rt,
        [](const String& name, String& profileJson, void* ctx) {
            return fakeLoadJson(*static_cast<FakeRuntime*>(ctx), name, profileJson);
        }, &rt,
        [](const String&, uint8_t outBytes[6], bool& displayOn, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->loadProfileSettingsCalls++;
            if (!rtp->loadProfileSettingsResult) {
                return false;
            }
            memcpy(outBytes, rtp->storedProfileBytes, sizeof(rtp->storedProfileBytes));
            displayOn = rtp->storedProfileDisplayOn;
            return true;
        }, &rt,
        [](const JsonObject& settingsObj, uint8_t outBytes[6], void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->parseSettingsCalls++;
            if (!rtp->parseSettingsOk) {
                return false;
            }
            memset(outBytes, 0xFF, 6);
            if (settingsObj["byte0"].is<int>()) {
                outBytes[0] = static_cast<uint8_t>(settingsObj["byte0"].as<int>());
            }
            return true;
        }, &rt,
        [](const String& name,
           const String& description,
           bool displayOn,
           const uint8_t inBytes[6],
           String& error,
           void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->saveCalls++;
            rtp->lastSaveName = name;
            rtp->lastSaveDescription = description;
            rtp->lastSaveDisplayOn = displayOn;
            memcpy(rtp->lastSaveBytes, inBytes, 6);
            if (!rtp->saveOk) {
                error = rtp->saveError;
                return false;
            }
            return true;
        }, &rt,
        [](const String& /*name*/, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->deleteCalls++;
            return rtp->deleteResult;
        }, &rt,
        [](void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->requestUserBytesCalls++;
            return rtp->requestUserBytesResult;
        }, &rt,
        [](const uint8_t inBytes[6], void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->writeUserBytesCalls++;
            memcpy(rtp->lastWrittenBytes, inBytes, sizeof(rtp->lastWrittenBytes));
            return rtp->writeUserBytesResult;
        }, &rt,
        [](void* /*ctx*/) -> const V1Settings& {
            static V1Settings settings;
            return settings;
        }, nullptr,
        [](bool displayOn, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setDisplayOnCalls++;
            rtp->lastSetDisplayOn = displayOn;
        }, &rt,
        [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->hasCurrent; }, &rt,
        [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->currentSettingsJson; }, &rt,
        [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->connected; }, &rt,
        [](void* ctx) { static_cast<FakeRuntime*>(ctx)->requestDeferredBackupCalls++; }, &rt,
    };
}

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    mock_reset_heap_caps();
}

void tearDown() {}

void test_profiles_list_includes_loaded_profiles() {
    WebServer server(80);
    FakeRuntime rt;
    rt.listNames = {"A", "B"};
    rt.storedProfiles.push_back({"A", "desc-a", true, "{\"name\":\"A\"}"});

    mock_reset_heap_caps_tracking();
    WifiV1ProfileApiService::handleApiProfilesList(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"profiles\":[{\"name\":\"A\""));
    TEST_ASSERT_FALSE(responseContains(server, "\"name\":\"B\""));
    TEST_ASSERT_EQUAL_INT(1, rt.listCalls);
    TEST_ASSERT_EQUAL_INT(2, rt.loadSummaryCalls);
    assertWifiJsonAllocationsReleased();
}

void test_profile_get_missing_name_returns_400() {
    WebServer server(80);
    FakeRuntime rt;

    WifiV1ProfileApiService::handleApiProfileGet(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Missing profile name\""));
}

void test_profile_get_not_found_returns_404() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("name", "missing");

    WifiV1ProfileApiService::handleApiProfileGet(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(404, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Profile not found\""));
}

void test_profile_get_success_returns_profile_json() {
    WebServer server(80);
    FakeRuntime rt;
    rt.storedProfiles.push_back({"Commute", "", false, "{\"name\":\"Commute\",\"displayOn\":false}"});
    server.setArg("name", "Commute");

    WifiV1ProfileApiService::handleApiProfileGet(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"name\":\"Commute\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"displayOn\":false"));
}

void test_profile_save_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"name\":\"X\",\"settings\":{\"byte0\":4}}");

    WifiV1ProfileApiService::handleApiProfileSave(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return false; }, nullptr);

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
}

void test_profile_save_invalid_json_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{bad json");

    WifiV1ProfileApiService::handleApiProfileSave(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Invalid JSON\""));
}

void test_profile_save_invalid_settings_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    rt.parseSettingsOk = false;
    server.setArg("plain", "{\"name\":\"X\",\"settings\":{\"byte0\":4}}");

    WifiV1ProfileApiService::handleApiProfileSave(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Invalid settings\""));
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
}

void test_profile_save_success_calls_save_and_backup() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"description\":\"desc\",\"displayOn\":false,\"settings\":{\"byte0\":7}}");

    mock_reset_heap_caps_tracking();
    WifiV1ProfileApiService::handleApiProfileSave(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.saveCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.requestDeferredBackupCalls);
    TEST_ASSERT_EQUAL_STRING("RoadTrip", rt.lastSaveName.c_str());
    TEST_ASSERT_EQUAL_STRING("desc", rt.lastSaveDescription.c_str());
    TEST_ASSERT_FALSE(rt.lastSaveDisplayOn);
    TEST_ASSERT_EQUAL_UINT8(7, rt.lastSaveBytes[0]);
    assertWifiJsonAllocationsReleased();
}

void test_profile_save_failure_returns_500_with_error() {
    WebServer server(80);
    FakeRuntime rt;
    rt.saveOk = false;
    rt.saveError = "disk full";
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"settings\":{\"byte0\":3}}");

    WifiV1ProfileApiService::handleApiProfileSave(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"disk full\""));
    TEST_ASSERT_EQUAL_INT(0, rt.requestDeferredBackupCalls);
}

void test_profile_delete_success_calls_backup() {
    WebServer server(80);
    FakeRuntime rt;
    rt.deleteResult = true;
    server.setArg("plain", "{\"name\":\"RoadTrip\"}");

    mock_reset_heap_caps_tracking();
    WifiV1ProfileApiService::handleApiProfileDelete(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.deleteCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.requestDeferredBackupCalls);
    assertWifiJsonAllocationsReleased();
}

void test_profile_delete_not_found_returns_404() {
    WebServer server(80);
    FakeRuntime rt;
    rt.deleteResult = false;
    server.setArg("plain", "{\"name\":\"RoadTrip\"}");

    WifiV1ProfileApiService::handleApiProfileDelete(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(404, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Profile not found\""));
    TEST_ASSERT_EQUAL_INT(0, rt.requestDeferredBackupCalls);
}

void test_current_settings_unavailable() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.hasCurrent = false;

    WifiV1ProfileApiService::handleApiCurrentSettings(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"connected\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"available\":false"));
}

void test_current_settings_available_embeds_settings_json() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = false;
    rt.hasCurrent = true;
    rt.currentSettingsJson = "{\"xBand\":true,\"bytes\":[1,2,3,4,5,6]}";

    mock_reset_heap_caps_tracking();
    WifiV1ProfileApiService::handleApiCurrentSettings(server, makeRuntime(rt));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"connected\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"available\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"settings\":{\"xBand\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"bytes\":[1,2,3,4,5,6]"));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(2u, g_mock_heap_caps_malloc_calls);
    assertWifiJsonAllocationsReleased();
}

void test_api_settings_push_raw_bytes_success_uses_wifi_json_allocator() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.writeUserBytesResult = true;
    server.setArg("plain", "{\"bytes\":[1,2,3,4,5,6],\"displayOn\":false}");

    mock_reset_heap_caps_tracking();
    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.writeUserBytesCalls);
    TEST_ASSERT_EQUAL_UINT8(1, rt.lastWrittenBytes[0]);
    TEST_ASSERT_EQUAL_UINT8(6, rt.lastWrittenBytes[5]);
    TEST_ASSERT_EQUAL_INT(1, rt.setDisplayOnCalls);
    TEST_ASSERT_FALSE(rt.lastSetDisplayOn);
    assertWifiJsonAllocationsReleased();
}

void test_api_settings_pull_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;

    WifiV1ProfileApiService::handleApiSettingsPull(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return false; }, nullptr);

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_api_settings_push_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", "{\"name\":\"RoadTrip\"}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return false; }, nullptr);

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}



namespace ProfilePushPull {

static bool responseContainsPushPull(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

struct FakeRuntime {
    bool connected = false;
    bool requestResult = false;
    bool profileFound = false;
    bool parseSettingsOk = true;
    bool writeResult = true;

    uint8_t profileBytes[6] = {0};
    bool profileDisplayOn = true;

    int requestCalls = 0;
    int loadProfileSettingsCalls = 0;
    int parseSettingsCalls = 0;
    int writeCalls = 0;
    int setDisplayCalls = 0;

    uint8_t lastWriteBytes[6] = {0};
    bool lastDisplayOn = true;
};

static WifiV1ProfileApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiV1ProfileApiService::Runtime{
        [](void* /*ctx*/) { return std::vector<String>{}; }, nullptr,
        [](const String&, WifiV1ProfileApiService::ProfileSummary&, void* /*ctx*/) { return false; }, nullptr,
        [](const String&, String&, void* /*ctx*/) { return false; }, nullptr,
        [](const String&, uint8_t outBytes[6], bool& displayOn, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->loadProfileSettingsCalls++;
            if (!rtp->profileFound) {
                return false;
            }
            memcpy(outBytes, rtp->profileBytes, 6);
            displayOn = rtp->profileDisplayOn;
            return true;
        }, &rt,
        [](const JsonObject& settingsObj, uint8_t outBytes[6], void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->parseSettingsCalls++;
            if (!rtp->parseSettingsOk) {
                return false;
            }
            memset(outBytes, 0, 6);
            if (settingsObj["byte0"].is<int>()) {
                outBytes[0] = static_cast<uint8_t>(settingsObj["byte0"].as<int>());
            }
            return true;
        }, &rt,
        [](const String&, const String&, bool, const uint8_t[6], String&, void* /*ctx*/) { return false; }, nullptr,
        [](const String&, void* /*ctx*/) { return false; }, nullptr,
        [](void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->requestCalls++;
            return rtp->requestResult;
        }, &rt,
        [](const uint8_t inBytes[6], void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->writeCalls++;
            memcpy(rtp->lastWriteBytes, inBytes, 6);
            return rtp->writeResult;
        }, &rt,
        [](void* /*ctx*/) -> const V1Settings& {
            static V1Settings settings;
            return settings;
        }, nullptr,
        [](bool displayOn, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setDisplayCalls++;
            rtp->lastDisplayOn = displayOn;
        }, &rt,
        [](void* /*ctx*/) { return false; }, nullptr,
        [](void* /*ctx*/) { return String("{}"); }, nullptr,
        [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->connected; }, &rt,
        [](void* /*ctx*/) {}, nullptr,
    };
}

static WifiV1ProfileApiService::Runtime makeMaintenanceRuntime(FakeRuntime& rt) {
    auto runtime = makeRuntime(rt);
    runtime.maintenanceBootActive = true;
    return runtime;
}

void test_profile_settings_pull_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.requestResult = true;

    WifiV1ProfileApiService::handleApiSettingsPull(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return false; }, nullptr);

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.requestCalls);
}

void test_profile_settings_pull_requires_connected_v1() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = false;

    WifiV1ProfileApiService::handleApiSettingsPull(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "\"error\":\"V1 not connected\""));
}

void test_profile_settings_pull_rejects_maintenance_boot_active() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.requestResult = true;

    WifiV1ProfileApiService::handleApiSettingsPull(
        server,
        makeMaintenanceRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "\"error\":\"maintenance_mode\""));
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "V1 push/pull not available in maintenance mode"));
    TEST_ASSERT_EQUAL_INT(0, rt.requestCalls);
}

void test_profile_settings_pull_request_failure_returns_500() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.requestResult = false;

    WifiV1ProfileApiService::handleApiSettingsPull(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "\"error\":\"Failed to send request\""));
    TEST_ASSERT_EQUAL_INT(1, rt.requestCalls);
}

void test_profile_settings_pull_success_returns_200() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.requestResult = true;

    WifiV1ProfileApiService::handleApiSettingsPull(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "\"message\":\"Request sent. Check current settings.\""));
    TEST_ASSERT_EQUAL_INT(1, rt.requestCalls);
}

void test_profile_settings_push_rate_limited_short_circuits() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    server.setArg("plain", "{\"name\":\"RoadTrip\"}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return false; }, nullptr);

    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(0, rt.writeCalls);
}

void test_profile_settings_push_requires_connected_v1() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = false;
    server.setArg("plain", "{\"name\":\"RoadTrip\"}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "\"error\":\"V1 not connected\""));
}

void test_profile_settings_push_rejects_maintenance_boot_active() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    server.setArg("plain", "{\"name\":\"RoadTrip\"}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeMaintenanceRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "\"error\":\"maintenance_mode\""));
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "V1 push/pull not available in maintenance mode"));
    TEST_ASSERT_EQUAL_INT(0, rt.loadProfileSettingsCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.parseSettingsCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.writeCalls);
}

void test_profile_settings_push_requires_body() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "\"error\":\"Missing request body\""));
}

void test_profile_settings_push_invalid_json_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    server.setArg("plain", "{bad json");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "\"error\":\"Invalid JSON\""));
}

void test_profile_settings_push_profile_name_not_found_returns_404() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.profileFound = false;
    server.setArg("plain", "{\"name\":\"Unknown\"}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(404, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "\"error\":\"Profile not found\""));
}

void test_profile_settings_push_profile_name_success_writes_and_sets_display() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.profileFound = true;
    rt.profileDisplayOn = false;
    rt.profileBytes[0] = 0xAA;
    rt.profileBytes[5] = 0x55;
    server.setArg("plain", "{\"name\":\"RoadTrip\"}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.loadProfileSettingsCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.writeCalls);
    TEST_ASSERT_EQUAL_UINT8(0xAA, rt.lastWriteBytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0x55, rt.lastWriteBytes[5]);
    TEST_ASSERT_EQUAL_INT(1, rt.setDisplayCalls);
    TEST_ASSERT_FALSE(rt.lastDisplayOn);
}

void test_profile_settings_push_disables_v1_laser_when_alp_policy_enabled() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.profileFound = true;
    rt.profileBytes[0] = 0xFF;
    server.setArg("plain", "{\"name\":\"RoadTrip\"}");

    V1Settings policySettings;
    policySettings.alpEnabled = true;
    policySettings.alpDisableV1LaserOnPush = true;
    auto runtime = makeRuntime(rt);
    runtime.getSettings = [](void* ctx) -> const V1Settings& {
        return *static_cast<V1Settings*>(ctx);
    };
    runtime.getSettingsCtx = &policySettings;

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        runtime,
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.writeCalls);
    TEST_ASSERT_EQUAL_HEX8(0xF7, rt.lastWriteBytes[0]);
}

void test_profile_settings_push_keeps_v1_laser_when_alp_policy_disabled() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.profileFound = true;
    rt.profileBytes[0] = 0xFF;
    server.setArg("plain", "{\"name\":\"RoadTrip\"}");

    V1Settings policySettings;
    policySettings.alpEnabled = true;
    policySettings.alpDisableV1LaserOnPush = false;
    auto runtime = makeRuntime(rt);
    runtime.getSettings = [](void* ctx) -> const V1Settings& {
        return *static_cast<V1Settings*>(ctx);
    };
    runtime.getSettingsCtx = &policySettings;

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        runtime,
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.writeCalls);
    TEST_ASSERT_EQUAL_HEX8(0xFF, rt.lastWriteBytes[0]);
}

void test_profile_settings_push_raw_bytes_requires_six_items() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    server.setArg("plain", "{\"bytes\":[1,2,3]}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "\"error\":\"Invalid bytes array\""));
}

void test_profile_settings_push_raw_bytes_success() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    server.setArg("plain", "{\"bytes\":[1,2,3,4,5,6],\"displayOn\":false}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.writeCalls);
    TEST_ASSERT_EQUAL_UINT8(1, rt.lastWriteBytes[0]);
    TEST_ASSERT_EQUAL_UINT8(6, rt.lastWriteBytes[5]);
    TEST_ASSERT_EQUAL_INT(1, rt.setDisplayCalls);
    TEST_ASSERT_FALSE(rt.lastDisplayOn);
}

void test_profile_settings_push_settings_parse_failure_returns_400() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.parseSettingsOk = false;
    server.setArg("plain", "{\"settings\":{\"byte0\":9}}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "\"error\":\"Invalid settings\""));
    TEST_ASSERT_EQUAL_INT(0, rt.writeCalls);
}

void test_profile_settings_push_settings_root_fallback_success() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.parseSettingsOk = true;
    server.setArg("plain", "{\"byte0\":9}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, rt.parseSettingsCalls);
    TEST_ASSERT_EQUAL_INT(1, rt.writeCalls);
    TEST_ASSERT_EQUAL_UINT8(9, rt.lastWriteBytes[0]);
    TEST_ASSERT_EQUAL_INT(1, rt.setDisplayCalls);
    TEST_ASSERT_TRUE(rt.lastDisplayOn);
}

void test_profile_settings_push_write_failure_returns_500() {
    WebServer server(80);
    FakeRuntime rt;
    rt.connected = true;
    rt.parseSettingsOk = true;
    rt.writeResult = false;
    server.setArg("plain", "{\"settings\":{\"byte0\":2},\"displayOn\":false}");

    WifiV1ProfileApiService::handleApiSettingsPush(
        server,
        makeRuntime(rt),
        [](void* /*ctx*/) { return true; }, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContainsPushPull(server, "\"error\":\"Write command failed - check V1 connection\""));
    TEST_ASSERT_EQUAL_INT(0, rt.setDisplayCalls);
}


}  // namespace ProfilePushPull

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_profiles_list_includes_loaded_profiles);
    RUN_TEST(test_profile_get_missing_name_returns_400);
    RUN_TEST(test_profile_get_not_found_returns_404);
    RUN_TEST(test_profile_get_success_returns_profile_json);
    RUN_TEST(test_profile_save_rate_limited_short_circuits);
    RUN_TEST(test_profile_save_invalid_json_returns_400);
    RUN_TEST(test_profile_save_invalid_settings_returns_400);
    RUN_TEST(test_profile_save_success_calls_save_and_backup);
    RUN_TEST(test_profile_save_failure_returns_500_with_error);
    RUN_TEST(test_profile_delete_success_calls_backup);
    RUN_TEST(test_profile_delete_not_found_returns_404);
    RUN_TEST(test_current_settings_unavailable);
    RUN_TEST(test_current_settings_available_embeds_settings_json);
    RUN_TEST(test_api_settings_pull_rate_limited_short_circuits);
    RUN_TEST(test_api_settings_push_raw_bytes_success_uses_wifi_json_allocator);
    RUN_TEST(test_api_settings_push_rate_limited_short_circuits);
    RUN_TEST(ProfilePushPull::test_profile_settings_pull_rate_limited_short_circuits);
    RUN_TEST(ProfilePushPull::test_profile_settings_pull_requires_connected_v1);
    RUN_TEST(ProfilePushPull::test_profile_settings_pull_rejects_maintenance_boot_active);
    RUN_TEST(ProfilePushPull::test_profile_settings_pull_request_failure_returns_500);
    RUN_TEST(ProfilePushPull::test_profile_settings_pull_success_returns_200);
    RUN_TEST(ProfilePushPull::test_profile_settings_push_rate_limited_short_circuits);
    RUN_TEST(ProfilePushPull::test_profile_settings_push_requires_connected_v1);
    RUN_TEST(ProfilePushPull::test_profile_settings_push_rejects_maintenance_boot_active);
    RUN_TEST(ProfilePushPull::test_profile_settings_push_requires_body);
    RUN_TEST(ProfilePushPull::test_profile_settings_push_invalid_json_returns_400);
    RUN_TEST(ProfilePushPull::test_profile_settings_push_profile_name_not_found_returns_404);
    RUN_TEST(ProfilePushPull::test_profile_settings_push_profile_name_success_writes_and_sets_display);
    RUN_TEST(ProfilePushPull::test_profile_settings_push_disables_v1_laser_when_alp_policy_enabled);
    RUN_TEST(ProfilePushPull::test_profile_settings_push_keeps_v1_laser_when_alp_policy_disabled);
    RUN_TEST(ProfilePushPull::test_profile_settings_push_raw_bytes_requires_six_items);
    RUN_TEST(ProfilePushPull::test_profile_settings_push_raw_bytes_success);
    RUN_TEST(ProfilePushPull::test_profile_settings_push_settings_parse_failure_returns_400);
    RUN_TEST(ProfilePushPull::test_profile_settings_push_settings_root_fallback_success);
    RUN_TEST(ProfilePushPull::test_profile_settings_push_write_failure_returns_500);
    return UNITY_END();
}
