#include <unity.h>
#include <cstring>

#include "../mocks/settings.h"
#include "../../src/modules/obd/obd_runtime_module.h"

#include "../../src/modules/obd/obd_elm327_parser.cpp"
#include "../../src/modules/obd/obd_runtime_module.cpp"
#include "../../src/modules/obd/obd_runtime_transport.cpp"
#include "../../src/modules/obd/obd_runtime_commands.cpp"
#include "../../src/modules/obd/obd_runtime_state_machine.cpp"
#include "../../src/modules/obd/obd_api_service.cpp"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 1000;
unsigned long mockMicros = 1000000;

namespace {
int syncAfterConfigChangeCalls = 0;
}  // namespace

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

static ObdApiService::Runtime makeTestRuntime() {
    ObdApiService::Runtime r;
    r.markUiActivity = [](void* /*ctx*/) {};
    r.checkRateLimit = [](void* /*ctx*/) { return true; };
    r.syncAfterConfigChange = [](void* /*ctx*/) { ++syncAfterConfigChangeCalls; };
    r.ctx = nullptr;
    return r;
}

static ObdApiService::Runtime makeMaintenanceRuntime() {
    ObdApiService::Runtime r = makeTestRuntime();
    r.maintenanceBootActive = true;
    return r;
}

static void resetRuntime() {
    obdRuntimeModule = ObdRuntimeModule();
}

void setUp() {
    resetRuntime();
    syncAfterConfigChangeCalls = 0;
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_config_get_returns_persisted_settings() {
    WebServer server(80);
    SettingsManager settingsManager;
    settingsManager.settings.obdEnabled = true;
    settingsManager.settings.obdMinRssi = -62;
    settingsManager.settings.obdScanWindowMs = 18000;
    settingsManager.settings.obdRetryIntervalMs = 90000;
    settingsManager.settings.proxyOpenWindowMs = 45000;
    settingsManager.settings.wifiOpenTimeoutMs = 42000;
    settingsManager.settings.v1SettleQuietMs = 700;
    settingsManager.settings.v1SettleFallbackMs = 2200;
    settingsManager.settings.cycleTeardownAckTimeoutMs = 150;

    ObdApiService::handleApiConfigGet(server, settingsManager, makeTestRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"enabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"minRssi\":-62"));
    TEST_ASSERT_TRUE(responseContains(server, "\"obdScanWindowMs\":18000"));
    TEST_ASSERT_TRUE(responseContains(server, "\"obdRetryIntervalMs\":90000"));
    TEST_ASSERT_TRUE(responseContains(server, "\"proxyOpenWindowMs\":45000"));
    TEST_ASSERT_TRUE(responseContains(server, "\"wifiOpenTimeoutMs\":42000"));
    TEST_ASSERT_TRUE(responseContains(server, "\"v1SettleQuietMs\":700"));
    TEST_ASSERT_TRUE(responseContains(server, "\"v1SettleFallbackMs\":2200"));
    TEST_ASSERT_TRUE(responseContains(server, "\"cycleTeardownAckTimeoutMs\":150"));
}

void test_devices_list_returns_saved_obd_device_with_name() {
    WebServer server(80);
    SettingsManager settingsManager;
    settingsManager.settings.obdSavedAddress = "A4:C1:38:00:11:22";
    settingsManager.settings.obdSavedName = "Truck Adapter";
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    ObdApiService::handleApiDevicesList(server, obdRuntimeModule, settingsManager, makeTestRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"count\":1"));
    TEST_ASSERT_TRUE(responseContains(server, "\"address\":\"A4:C1:38:00:11:22\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"name\":\"Truck Adapter\""));
}

void test_device_name_save_updates_saved_name_and_persists_setting() {
    WebServer server(80);
    SettingsManager settingsManager;
    settingsManager.settings.obdSavedAddress = "A4:C1:38:00:11:22";

    server.setArg("address", "a4:c1:38:00:11:22");
    server.setArg("name", "  Family Car  ");

    ObdApiService::handleApiDeviceNameSave(server, settingsManager, makeTestRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_EQUAL_STRING("Family Car", settingsManager.settings.obdSavedName.c_str());
    TEST_ASSERT_EQUAL_INT(0, settingsManager.saveCalls);
    TEST_ASSERT_EQUAL_INT(1, settingsManager.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL_INT(0, settingsManager.requestDeferredPersistCalls);
}

void test_device_name_save_rejects_unknown_saved_device() {
    WebServer server(80);
    SettingsManager settingsManager;
    settingsManager.settings.obdSavedAddress = "A4:C1:38:00:11:22";

    server.setArg("address", "B4:C1:38:00:11:33");
    server.setArg("name", "Spare");

    ObdApiService::handleApiDeviceNameSave(server, settingsManager, makeTestRuntime());

    TEST_ASSERT_EQUAL_INT(404, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Saved OBD device not found"));
    TEST_ASSERT_EQUAL_STRING("", settingsManager.settings.obdSavedName.c_str());
    TEST_ASSERT_EQUAL_INT(0, settingsManager.saveCalls);
    TEST_ASSERT_EQUAL_INT(0, settingsManager.requestDeferredPersistCalls);
}

void test_config_updates_runtime_settings_and_selector_inputs() {
    WebServer server(80);
    SettingsManager settingsManager;
    syncAfterConfigChangeCalls = 0;

    obdRuntimeModule.begin(nullptr, false, "", 0, -80);
    server.setArg(
        "plain",
        "{\"enabled\":true,\"minRssi\":-55,\"obdScanWindowMs\":18000,\"obdRetryIntervalMs\":90000,\"proxyOpenWindowMs\":45000,\"wifiOpenTimeoutMs\":42000,\"v1SettleQuietMs\":700,\"v1SettleFallbackMs\":2200,\"cycleTeardownAckTimeoutMs\":150}");

    ObdApiService::handleApiConfig(server,
                                   obdRuntimeModule,
                                   settingsManager,
                                   makeTestRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":true"));
    TEST_ASSERT_TRUE(settingsManager.settings.obdEnabled);
    TEST_ASSERT_EQUAL_INT8(-55, settingsManager.settings.obdMinRssi);
    TEST_ASSERT_EQUAL_UINT32(18000u, settingsManager.settings.obdScanWindowMs);
    TEST_ASSERT_EQUAL_UINT32(90000u, settingsManager.settings.obdRetryIntervalMs);
    TEST_ASSERT_EQUAL_UINT32(45000u, settingsManager.settings.proxyOpenWindowMs);
    TEST_ASSERT_EQUAL_UINT32(42000u, settingsManager.settings.wifiOpenTimeoutMs);
    TEST_ASSERT_EQUAL_UINT32(700u, settingsManager.settings.v1SettleQuietMs);
    TEST_ASSERT_EQUAL_UINT32(2200u, settingsManager.settings.v1SettleFallbackMs);
    TEST_ASSERT_EQUAL_UINT32(150u, settingsManager.settings.cycleTeardownAckTimeoutMs);
    TEST_ASSERT_EQUAL_INT(1, syncAfterConfigChangeCalls);
    TEST_ASSERT_EQUAL_INT(0, settingsManager.saveCalls);
    TEST_ASSERT_EQUAL_INT(1, settingsManager.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL_INT(0, settingsManager.requestDeferredPersistCalls);
}

void test_config_save_allowed_in_maintenance_mode() {
    WebServer server(80);
    SettingsManager settingsManager;
    syncAfterConfigChangeCalls = 0;

    obdRuntimeModule.begin(nullptr, false, "", 0, -80);
    server.setArg("plain", "{\"enabled\":true,\"minRssi\":-55}");

    ObdApiService::handleApiConfig(server,
                                   obdRuntimeModule,
                                   settingsManager,
                                   makeMaintenanceRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":true"));
    TEST_ASSERT_TRUE(settingsManager.settings.obdEnabled);
    TEST_ASSERT_EQUAL_INT8(-55, settingsManager.settings.obdMinRssi);
    TEST_ASSERT_EQUAL_INT(1, syncAfterConfigChangeCalls);
    TEST_ASSERT_EQUAL_INT(1, settingsManager.saveCalls);
    TEST_ASSERT_EQUAL_INT(0, settingsManager.requestDeferredPersistCalls);
}

void test_forget_clears_saved_address_and_persists_setting() {
    WebServer server(80);
    SettingsManager settingsManager;
    settingsManager.settings.obdSavedAddress = "A4:C1:38:00:11:22";
    settingsManager.settings.obdSavedName = "Truck Adapter";
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    ObdApiService::handleApiForget(server,
                                   obdRuntimeModule,
                                   settingsManager,
                                   makeTestRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":true"));
    TEST_ASSERT_EQUAL_STRING("", settingsManager.settings.obdSavedAddress.c_str());
    TEST_ASSERT_EQUAL_STRING("", settingsManager.settings.obdSavedName.c_str());
    TEST_ASSERT_EQUAL_INT(0, settingsManager.saveCalls);
    TEST_ASSERT_EQUAL_INT(1, settingsManager.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL_INT(0, settingsManager.requestDeferredPersistCalls);

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(mockMillis);
    TEST_ASSERT_FALSE(status.savedAddressValid);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
}

void test_forget_allowed_in_maintenance_clears_settings_only() {
    WebServer server(80);
    SettingsManager settingsManager;
    settingsManager.settings.obdSavedAddress = "A4:C1:38:00:11:22";
    settingsManager.settings.obdSavedName = "Truck Adapter";
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    ObdApiService::handleApiForget(server,
                                   obdRuntimeModule,
                                   settingsManager,
                                   makeMaintenanceRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":true"));
    TEST_ASSERT_EQUAL_STRING("", settingsManager.settings.obdSavedAddress.c_str());
    TEST_ASSERT_EQUAL_STRING("", settingsManager.settings.obdSavedName.c_str());
    TEST_ASSERT_EQUAL_INT(1, settingsManager.saveCalls);
    TEST_ASSERT_EQUAL_INT(0, settingsManager.requestDeferredPersistCalls);
    // Maintenance boot saves the preference for the next normal boot without
    // touching the live OBD runtime that is intentionally inactive there.
    TEST_ASSERT_TRUE(obdRuntimeModule.snapshot(mockMillis).savedAddressValid);
}

void test_config_rejects_missing_json_body() {
    WebServer server(80);
    SettingsManager settingsManager;
    obdRuntimeModule.begin(nullptr, false, "", 0, -80);

    ObdApiService::handleApiConfig(server,
                                   obdRuntimeModule,
                                   settingsManager,
                                   makeTestRuntime());

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Missing JSON body"));
    TEST_ASSERT_EQUAL_INT(0, settingsManager.saveCalls);
    TEST_ASSERT_EQUAL_INT(0, settingsManager.requestDeferredPersistCalls);
}

void test_scan_rejects_when_obd_is_disabled() {
    WebServer server(80);
    obdRuntimeModule.begin(nullptr, false, "", 0, -80);

    ObdApiService::handleApiScan(server, obdRuntimeModule, makeTestRuntime());

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":\"OBD is disabled\""));
    TEST_ASSERT_FALSE(obdRuntimeModule.snapshot(mockMillis).scanInProgress);
}

void test_scan_rejects_maintenance_mode() {
    WebServer server(80);
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    ObdApiService::handleApiScan(server, obdRuntimeModule, makeMaintenanceRuntime());

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"maintenance_mode\""));
    TEST_ASSERT_TRUE(responseContains(server, "OBD runtime endpoints are not available in maintenance mode"));
    TEST_ASSERT_FALSE(obdRuntimeModule.snapshot(mockMillis).manualScanPending);
}

void test_scan_reports_requested_when_obd_is_enabled() {
    WebServer server(80);
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    ObdApiService::handleApiScan(server, obdRuntimeModule, makeTestRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"requested\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"scanInProgress\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":\"OBD scan requested\""));
    const ObdRuntimeStatus status = obdRuntimeModule.snapshot(mockMillis);
    TEST_ASSERT_TRUE(status.manualScanPending);
    TEST_ASSERT_FALSE(status.scanInProgress);
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::PREEMPT_PROXY_FOR_MANUAL_SCAN,
                      obdRuntimeModule.getBleArbitrationRequest());
}

void test_scan_rejects_when_request_already_pending() {
    WebServer server(80);
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);
    TEST_ASSERT_TRUE(obdRuntimeModule.requestManualPairScan(mockMillis));

    ObdApiService::handleApiScan(server, obdRuntimeModule, makeTestRuntime());

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":false"));
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":\"OBD scan already requested or in progress\""));
}

void test_status_reports_manual_scan_pending() {
    WebServer server(80);
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);
    TEST_ASSERT_TRUE(obdRuntimeModule.requestManualPairScan(mockMillis));

    ObdApiService::handleApiStatus(server, obdRuntimeModule, makeTestRuntime());

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"manualScanPending\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"savedAddressValid\":false"));
}

void test_status_rejects_maintenance_mode() {
    WebServer server(80);
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);
    TEST_ASSERT_TRUE(obdRuntimeModule.requestManualPairScan(mockMillis));

    ObdApiService::handleApiStatus(server, obdRuntimeModule, makeMaintenanceRuntime());

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"maintenance_mode\""));
    TEST_ASSERT_TRUE(responseContains(server, "OBD runtime endpoints are not available in maintenance mode"));
}

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_config_get_returns_persisted_settings);
    RUN_TEST(test_devices_list_returns_saved_obd_device_with_name);
    RUN_TEST(test_device_name_save_updates_saved_name_and_persists_setting);
    RUN_TEST(test_device_name_save_rejects_unknown_saved_device);
    RUN_TEST(test_config_updates_runtime_settings_and_selector_inputs);
    RUN_TEST(test_config_save_allowed_in_maintenance_mode);
    RUN_TEST(test_forget_clears_saved_address_and_persists_setting);
    RUN_TEST(test_forget_allowed_in_maintenance_clears_settings_only);
    RUN_TEST(test_config_rejects_missing_json_body);
    RUN_TEST(test_scan_rejects_when_obd_is_disabled);
    RUN_TEST(test_scan_rejects_maintenance_mode);
    RUN_TEST(test_scan_reports_requested_when_obd_is_enabled);
    RUN_TEST(test_scan_rejects_when_request_already_pending);
    RUN_TEST(test_status_reports_manual_scan_pending);
    RUN_TEST(test_status_rejects_maintenance_mode);

    return UNITY_END();
}
