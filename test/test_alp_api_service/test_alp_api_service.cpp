#include <unity.h>
#include <cstring>

#include "../../src/modules/alp/alp_runtime_module.h"
#include "../../src/modules/alp/alp_sd_logger.h"
#include "../../src/modules/gps/gps_publishers.cpp"
#include "../../src/modules/alp/alp_sd_logger.cpp"
#include "../../src/modules/alp/alp_runtime_module.cpp"
#include "../../src/modules/alp/alp_api_service.cpp"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 1000;
unsigned long mockMicros = 1000000;

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

static int markUiActivityCalls = 0;

static void markUiActivity(void* /*ctx*/) {
    ++markUiActivityCalls;
}

void setUp() {
    alpRuntimeModule = AlpRuntimeModule();
    markUiActivityCalls = 0;
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_status_reports_runtime_when_not_in_maintenance() {
    WebServer server(80);
    alpRuntimeModule.begin(true);

    AlpApiService::handleApiStatus(server, alpRuntimeModule, markUiActivity, nullptr, false);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"enabled\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"stateName\""));
    TEST_ASSERT_EQUAL_INT(1, markUiActivityCalls);
}

void test_status_rejects_maintenance_mode() {
    WebServer server(80);
    alpRuntimeModule.begin(true);

    AlpApiService::handleApiStatus(server, alpRuntimeModule, markUiActivity, nullptr, true);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"maintenance_mode\""));
    TEST_ASSERT_TRUE(responseContains(server, "ALP runtime status is not available in maintenance mode"));
    TEST_ASSERT_EQUAL_INT(1, markUiActivityCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_status_reports_runtime_when_not_in_maintenance);
    RUN_TEST(test_status_rejects_maintenance_mode);
    return UNITY_END();
}
