#include <unity.h>
#include <cstring>

#include "../../src/modules/gps/gps_runtime_module.h"
#include "../../src/modules/gps/gps_publishers.cpp"      // Pull GPS publisher globals for UNIT_TEST.
#include "../../src/modules/gps/gps_runtime_module.cpp"  // Pull implementation for UNIT_TEST.
#include "../../src/modules/gps/gps_api_service.cpp"     // Pull implementation for UNIT_TEST.

void SettingsManager::applyDeviceSettingsUpdate(const DeviceSettingsUpdate&,
                                                SettingsPersistMode) {}

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

void setUp() {
    gpsRuntimeModule = GpsRuntimeModule();
    markUiActivityCalls = 0;
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

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
    RUN_TEST(test_status_reports_runtime_when_not_in_maintenance);
    RUN_TEST(test_status_rejects_maintenance_mode);
    return UNITY_END();
}
