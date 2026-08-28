#include <unity.h>

#include <string>

#include "../mocks/Arduino.h"
#include "../mocks/WebServer.h"
#include "../mocks/settings.h"
#include "../../src/modules/obd/obd_runtime_module.h"

ObdRuntimeStatus ObdRuntimeModule::snapshot(uint32_t) const { return {}; }
void ObdRuntimeModule::forgetDevice() {}

#include "../../src/modules/obd/obd_api_service.cpp"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

namespace {

constexpr const char* kSavedAddress = "A4:C1:38:00:11:22";

struct Probe {
    int activityCalls = 0;
    int rateLimitCalls = 0;
    int syncCalls = 0;
};

ObdApiService::Runtime maintenanceRuntime(Probe& probe) {
    ObdApiService::Runtime runtime;
    runtime.maintenanceBootActive = true;
    runtime.markUiActivity = [](void* ctx) { static_cast<Probe*>(ctx)->activityCalls++; };
    runtime.checkRateLimit = [](void* ctx) {
        static_cast<Probe*>(ctx)->rateLimitCalls++;
        return true;
    };
    runtime.syncAfterConfigChange = [](void* ctx) { static_cast<Probe*>(ctx)->syncCalls++; };
    runtime.ctx = &probe;
    return runtime;
}

bool bodyContains(const WebServer& server, const char* text) {
    return std::string(server.lastBody.c_str()).find(text) != std::string::npos;
}

} // namespace

void setUp() {}
void tearDown() {}

void test_maintenance_config_get_exposes_live_settings_without_retired_wifi_dwell() {
    WebServer server(80);
    SettingsManager settings;
    Probe probe;
    settings.getMutable().obdScanWindowMs = 22000;
    settings.getMutable().proxyOpenWindowMs = 44000;

    ObdApiService::handleApiConfigGet(server, settings, maintenanceRuntime(probe));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(bodyContains(server, "\"obdScanWindowMs\":22000"));
    TEST_ASSERT_TRUE(bodyContains(server, "\"proxyOpenWindowMs\":44000"));
    TEST_ASSERT_FALSE(bodyContains(server, "wifiOpenTimeoutMs"));
    TEST_ASSERT_EQUAL_INT(1, probe.activityCalls);
}

void test_maintenance_config_update_persists_immediately_without_live_runtime_sync() {
    WebServer server(80);
    SettingsManager settings;
    Probe probe;
    server.setArg("plain", "{\"obdScanWindowMs\":24000,\"v1SettleQuietMs\":900}");

    ObdApiService::handleApiConfig(server, nullptr, settings, maintenanceRuntime(probe));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_UINT32(24000, settings.get().obdScanWindowMs);
    TEST_ASSERT_EQUAL_UINT32(900, settings.get().v1SettleQuietMs);
    TEST_ASSERT_EQUAL_INT(1, settings.saveCalls);
    TEST_ASSERT_EQUAL_INT(0, settings.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL_INT(0, probe.syncCalls);
    TEST_ASSERT_EQUAL_INT(1, probe.activityCalls);
    TEST_ASSERT_EQUAL_INT(1, probe.rateLimitCalls);
}

void test_legacy_wifi_dwell_key_is_accepted_but_ignored_at_api_parse_boundary() {
    WebServer server(80);
    SettingsManager settings;
    Probe probe;
    server.setArg("plain", "{\"wifiOpenTimeoutMs\":28000}");

    ObdApiService::handleApiConfig(server, nullptr, settings, maintenanceRuntime(probe));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("{\"success\":true}", server.lastBody.c_str());
    TEST_ASSERT_EQUAL_INT(0, settings.saveCalls);
    TEST_ASSERT_EQUAL_INT(0, settings.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL_INT(0, probe.syncCalls);
}

void test_maintenance_forget_updates_storage_without_obd_runtime_instance() {
    WebServer server(80);
    SettingsManager settings;
    Probe probe;
    settings.getMutable().obdSavedAddress = kSavedAddress;
    settings.getMutable().obdSavedName = "Garage";

    ObdApiService::handleApiForget(server, nullptr, settings, maintenanceRuntime(probe));

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("", settings.get().obdSavedAddress.c_str());
    TEST_ASSERT_EQUAL_STRING("", settings.get().obdSavedName.c_str());
    TEST_ASSERT_EQUAL_INT(1, settings.saveCalls);
}

void test_config_persist_failure_returns_500_and_rolls_back() {
    WebServer server(80);
    SettingsManager settings;
    Probe probe;
    settings.obdPersistSuccess = false;
    server.setArg("plain", "{\"enabled\":true}");

    ObdApiService::handleApiConfig(server, nullptr, settings, maintenanceRuntime(probe));

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(bodyContains(server, "settings_persist_failed"));
    TEST_ASSERT_FALSE(settings.get().obdEnabled);
    TEST_ASSERT_EQUAL_INT(0, settings.saveCalls);
    TEST_ASSERT_EQUAL_INT(0, probe.syncCalls);
}

void test_forget_persist_failure_returns_500_and_preserves_device() {
    WebServer server(80);
    SettingsManager settings;
    Probe probe;
    settings.getMutable().obdSavedAddress = kSavedAddress;
    settings.getMutable().obdSavedName = "Garage";
    settings.obdPersistSuccess = false;

    ObdApiService::handleApiForget(server, nullptr, settings, maintenanceRuntime(probe));

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(bodyContains(server, "settings_persist_failed"));
    TEST_ASSERT_EQUAL_STRING(kSavedAddress, settings.get().obdSavedAddress.c_str());
    TEST_ASSERT_EQUAL_STRING("Garage", settings.get().obdSavedName.c_str());
    TEST_ASSERT_EQUAL_INT(0, settings.saveCalls);
}

void test_device_name_persist_failure_returns_500_and_preserves_name() {
    WebServer server(80);
    SettingsManager settings;
    Probe probe;
    settings.getMutable().obdSavedAddress = kSavedAddress;
    settings.getMutable().obdSavedName = "Garage";
    settings.obdPersistSuccess = false;
    server.setArg("address", kSavedAddress);
    server.setArg("name", "Road Car");

    ObdApiService::handleApiDeviceNameSave(server, settings, maintenanceRuntime(probe));

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(bodyContains(server, "settings_persist_failed"));
    TEST_ASSERT_EQUAL_STRING("Garage", settings.get().obdSavedName.c_str());
    TEST_ASSERT_EQUAL_INT(0, settings.saveCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_maintenance_config_get_exposes_live_settings_without_retired_wifi_dwell);
    RUN_TEST(test_maintenance_config_update_persists_immediately_without_live_runtime_sync);
    RUN_TEST(test_legacy_wifi_dwell_key_is_accepted_but_ignored_at_api_parse_boundary);
    RUN_TEST(test_maintenance_forget_updates_storage_without_obd_runtime_instance);
    RUN_TEST(test_config_persist_failure_returns_500_and_rolls_back);
    RUN_TEST(test_forget_persist_failure_returns_500_and_preserves_device);
    RUN_TEST(test_device_name_persist_failure_returns_500_and_preserves_name);
    return UNITY_END();
}
