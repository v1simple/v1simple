#include <unity.h>
#include <cstring>

#include "../../src/modules/gps/gps_runtime_module.h"
#include "../../src/modules/gps/gps_publishers.cpp"     // Pull GPS publisher globals for UNIT_TEST.
#include "../../src/modules/gps/gps_runtime_module.cpp" // Pull implementation for UNIT_TEST.
#include "../../src/modules/gps/gps_api_service.cpp"    // Pull implementation for UNIT_TEST.

static int deviceSettingsApplyCalls = 0;
static SettingsPersistMode lastPersistMode = SettingsPersistMode::Deferred;

SettingsManager::SettingsManager() = default;

void SettingsManager::applyDeviceSettingsUpdate(const DeviceSettingsUpdate& update, SettingsPersistMode persistMode) {
    ++deviceSettingsApplyCalls;
    lastPersistMode = persistMode;
    if (update.hasGpsEnabled)
        settings_.gpsEnabled = update.gpsEnabled;
    if (update.hasGpsBaud)
        settings_.gpsBaud = update.gpsBaud;
    if (update.hasGpsLogUtcToPerf)
        settings_.gpsLogUtcToPerf = update.gpsLogUtcToPerf;
    if (update.hasGpsLogUtcToAlp)
        settings_.gpsLogUtcToAlp = update.gpsLogUtcToAlp;
}

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 1000;
unsigned long mockMicros = 1000000;

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

static int markUiActivityCalls = 0;

static GpsApiService::Runtime makeRuntime(bool maintenanceBootActive = false) {
    GpsApiService::Runtime r;
    r.markUiActivity = [](void* /*ctx*/) { ++markUiActivityCalls; };
    r.ctx = nullptr;
    r.maintenanceBootActive = maintenanceBootActive;
    return r;
}

static void assertConfigRejectedWithoutApply(const char* requestBody, const char* expectedMessage) {
    SettingsManager settings;
    gpsRuntimeModule = GpsRuntimeModule();
    gpsRuntimeModule.begin(false);
    WebServer server(80);
    if (requestBody) {
        server.setArg("plain", requestBody);
    }

    GpsApiService::handleApiConfigSave(server, settings, &gpsRuntimeModule, makeRuntime(false));

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, expectedMessage));
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":"));
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":"));
    TEST_ASSERT_FALSE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(0, deviceSettingsApplyCalls);
    TEST_ASSERT_FALSE(gpsRuntimeModule.isEnabled());
}

void setUp() {
    gpsRuntimeModule = GpsRuntimeModule();
    markUiActivityCalls = 0;
    deviceSettingsApplyCalls = 0;
    lastPersistMode = SettingsPersistMode::Deferred;
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_config_get_reports_persisted_settings() {
    SettingsManager settings;
    V1Settings& values = settings.mutableSettings();
    values.gpsEnabled = true;
    values.gpsBaud = 38400;
    values.gpsEnablePinActiveHigh = true;
    values.gpsLogUtcToPerf = false;
    values.gpsLogUtcToAlp = true;
    WebServer server(80);

    GpsApiService::handleApiConfigGet(server, settings, makeRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"gpsEnabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"gpsBaud\":38400"));
    TEST_ASSERT_TRUE(responseContains(server, "\"gpsEnablePinActiveHigh\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"gpsLogUtcToPerf\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"gpsLogUtcToAlp\":true"));
    TEST_ASSERT_EQUAL_INT(1, markUiActivityCalls);
}

void test_config_save_persists_and_updates_live_runtime() {
    SettingsManager settings;
    gpsRuntimeModule.begin(false);
    WebServer server(80);
    server.setArg("plain", "{\"gpsEnabled\":true,\"gpsBaud\":38400,"
                           "\"gpsLogUtcToPerf\":false,\"gpsLogUtcToAlp\":false}");

    GpsApiService::handleApiConfigSave(server, settings, &gpsRuntimeModule, makeRuntime(false));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_INT(1, deviceSettingsApplyCalls);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPersistMode::ImmediateNvsDeferredBackup),
                          static_cast<int>(lastPersistMode));
    TEST_ASSERT_TRUE(settings.get().gpsEnabled);
    TEST_ASSERT_EQUAL_UINT32(38400, settings.get().gpsBaud);
    TEST_ASSERT_FALSE(settings.get().gpsLogUtcToPerf);
    TEST_ASSERT_FALSE(settings.get().gpsLogUtcToAlp);
    TEST_ASSERT_TRUE(gpsRuntimeModule.isEnabled());
    TEST_ASSERT_EQUAL_INT(1, markUiActivityCalls);
}

void test_config_save_is_persistence_only_without_maintenance_runtime() {
    SettingsManager settings;
    WebServer server(80);
    server.setArg("plain", "{\"gpsEnabled\":true}");

    GpsApiService::handleApiConfigSave(server, settings, nullptr, makeRuntime(true));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "live runtime resumes on next normal boot"));
    TEST_ASSERT_EQUAL_INT(1, deviceSettingsApplyCalls);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SettingsPersistMode::Immediate), static_cast<int>(lastPersistMode));
    TEST_ASSERT_TRUE(settings.get().gpsEnabled);
    TEST_ASSERT_EQUAL_INT(1, markUiActivityCalls);
}

void test_config_save_runtime_unavailable_precedes_body_validation_in_normal_mode() {
    const char* bodies[] = {nullptr, "{"};
    for (const char* body : bodies) {
        SettingsManager settings;
        WebServer server(80);
        if (body) {
            server.setArg("plain", body);
        }

        GpsApiService::handleApiConfigSave(server, settings, nullptr, makeRuntime(false));

        TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
        TEST_ASSERT_TRUE(responseContains(server, "gps runtime not wired"));
        TEST_ASSERT_EQUAL_INT(0, deviceSettingsApplyCalls);
    }
}

void test_config_save_maintenance_without_runtime_still_validates_body() {
    SettingsManager settings;
    WebServer server(80);
    server.setArg("plain", "{");

    GpsApiService::handleApiConfigSave(server, settings, nullptr, makeRuntime(true));

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Invalid JSON"));
    TEST_ASSERT_EQUAL_INT(0, deviceSettingsApplyCalls);
}

void test_config_save_rejects_missing_json_body() {
    assertConfigRejectedWithoutApply(nullptr, "Missing JSON body");
}

void test_config_save_rejects_empty_json_body() {
    assertConfigRejectedWithoutApply("", "Missing JSON body");
}

void test_config_save_rejects_invalid_json() {
    assertConfigRejectedWithoutApply("{", "Invalid JSON");
}

void test_config_save_rejects_non_object_json() {
    assertConfigRejectedWithoutApply("[]", "JSON body must be an object");
}

void test_config_save_rejects_empty_object() {
    assertConfigRejectedWithoutApply("{}", "No writable GPS settings provided");
}

void test_config_save_rejects_unknown_only_object() {
    assertConfigRejectedWithoutApply("{\"unknown\":true}", "No writable GPS settings provided");
}

void test_config_save_rejects_each_wrong_typed_supported_field() {
    struct InvalidFieldCase {
        const char* body;
        const char* field;
    };
    const InvalidFieldCase cases[] = {
        {"{\"gpsEnabled\":\"yes\"}", "gpsEnabled"},
        {"{\"gpsBaud\":\"38400\"}", "gpsBaud"},
        {"{\"gpsLogUtcToPerf\":1}", "gpsLogUtcToPerf"},
        {"{\"gpsLogUtcToAlp\":null}", "gpsLogUtcToAlp"},
        {"{\"gpsEnablePinActiveHigh\":\"high\"}", "gpsEnablePinActiveHigh"},
    };

    for (const InvalidFieldCase& testCase : cases) {
        assertConfigRejectedWithoutApply(testCase.body, testCase.field);
    }
}

void test_config_save_rejects_mixed_invalid_payload_atomically() {
    SettingsManager settings;
    V1Settings& values = settings.mutableSettings();
    values.gpsEnabled = false;
    values.gpsBaud = 9600;
    gpsRuntimeModule.begin(false);
    WebServer server(80);
    server.setArg("plain", "{\"gpsEnabled\":true,\"gpsBaud\":\"38400\"}");

    GpsApiService::handleApiConfigSave(server, settings, &gpsRuntimeModule, makeRuntime(false));

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "gpsBaud"));
    TEST_ASSERT_EQUAL_INT(0, deviceSettingsApplyCalls);
    TEST_ASSERT_FALSE(settings.get().gpsEnabled);
    TEST_ASSERT_EQUAL_UINT32(9600, settings.get().gpsBaud);
    TEST_ASSERT_FALSE(gpsRuntimeModule.isEnabled());
}

void test_config_save_accepts_partial_update_and_leaves_absent_fields() {
    SettingsManager settings;
    V1Settings& values = settings.mutableSettings();
    values.gpsEnabled = false;
    values.gpsBaud = 38400;
    values.gpsLogUtcToPerf = true;
    WebServer server(80);
    server.setArg("plain", "{\"gpsLogUtcToPerf\":false}");

    GpsApiService::handleApiConfigSave(server, settings, &gpsRuntimeModule, makeRuntime(false));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, deviceSettingsApplyCalls);
    TEST_ASSERT_FALSE(settings.get().gpsEnabled);
    TEST_ASSERT_EQUAL_UINT32(38400, settings.get().gpsBaud);
    TEST_ASSERT_FALSE(settings.get().gpsLogUtcToPerf);
}

void test_config_save_accepts_deprecated_boolean_alongside_writable_update_but_ignores_it() {
    SettingsManager settings;
    settings.mutableSettings().gpsEnablePinActiveHigh = true;
    WebServer server(80);
    server.setArg("plain", "{\"gpsEnabled\":true,\"gpsEnablePinActiveHigh\":false}");

    GpsApiService::handleApiConfigSave(server, settings, &gpsRuntimeModule, makeRuntime(false));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, deviceSettingsApplyCalls);
    TEST_ASSERT_TRUE(settings.get().gpsEnabled);
    TEST_ASSERT_TRUE(settings.get().gpsEnablePinActiveHigh);
    TEST_ASSERT_TRUE(gpsRuntimeModule.isEnabled());
}

void test_config_save_rejects_deprecated_only_update() {
    assertConfigRejectedWithoutApply("{\"gpsEnablePinActiveHigh\":false}", "No writable GPS settings provided");
}

void test_status_reports_runtime_when_not_in_maintenance() {
    WebServer server(80);
    gpsRuntimeModule.begin(true);

    GpsApiService::handleApiStatus(server, gpsRuntimeModule, makeRuntime(false));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"enabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"parserActive\""));
    TEST_ASSERT_EQUAL_INT(1, markUiActivityCalls);
}

void test_status_rejects_maintenance_mode() {
    WebServer server(80);
    gpsRuntimeModule.begin(true);

    GpsApiService::handleApiStatus(server, gpsRuntimeModule, makeRuntime(true));

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"maintenance_mode\""));
    TEST_ASSERT_TRUE(responseContains(server, "GPS runtime status is not available in maintenance mode"));
    TEST_ASSERT_EQUAL_INT(1, markUiActivityCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_config_get_reports_persisted_settings);
    RUN_TEST(test_config_save_persists_and_updates_live_runtime);
    RUN_TEST(test_config_save_is_persistence_only_without_maintenance_runtime);
    RUN_TEST(test_config_save_runtime_unavailable_precedes_body_validation_in_normal_mode);
    RUN_TEST(test_config_save_maintenance_without_runtime_still_validates_body);
    RUN_TEST(test_config_save_rejects_missing_json_body);
    RUN_TEST(test_config_save_rejects_empty_json_body);
    RUN_TEST(test_config_save_rejects_invalid_json);
    RUN_TEST(test_config_save_rejects_non_object_json);
    RUN_TEST(test_config_save_rejects_empty_object);
    RUN_TEST(test_config_save_rejects_unknown_only_object);
    RUN_TEST(test_config_save_rejects_each_wrong_typed_supported_field);
    RUN_TEST(test_config_save_rejects_mixed_invalid_payload_atomically);
    RUN_TEST(test_config_save_accepts_partial_update_and_leaves_absent_fields);
    RUN_TEST(test_config_save_accepts_deprecated_boolean_alongside_writable_update_but_ignores_it);
    RUN_TEST(test_config_save_rejects_deprecated_only_update);
    RUN_TEST(test_status_reports_runtime_when_not_in_maintenance);
    RUN_TEST(test_status_rejects_maintenance_mode);
    return UNITY_END();
}
