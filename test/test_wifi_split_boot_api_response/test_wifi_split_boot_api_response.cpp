#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/WebServer.h"
#include "../mocks/mock_heap_caps_state.h"
#include "../../src/modules/wifi/wifi_split_boot_api_response.h"

void setUp() { mock_reset_heap_caps(); }
void tearDown() {}

void assertUnavailable(WifiSplitBootApiResponse::Operation operation, const char* expectedBody) {
    WebServer server(80);
    WifiSplitBootApiResponse::sendUnavailable(server, operation);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("application/json", server.lastContentType.c_str());
    TEST_ASSERT_EQUAL_STRING(expectedBody, server.lastBody.c_str());
}

void test_v1_push_pull_payload_is_preserved() {
    assertUnavailable(WifiSplitBootApiResponse::Operation::V1_PUSH_PULL,
                      "{\"error\":\"maintenance_mode\","
                      "\"message\":\"V1 push/pull not available in maintenance mode\"}");
}

void test_auto_push_payload_is_preserved() {
    assertUnavailable(WifiSplitBootApiResponse::Operation::AUTO_PUSH_NOW,
                      "{\"success\":false,\"error\":\"live_push_unavailable_in_maintenance\","
                      "\"message\":\"Live V1 push is unavailable in maintenance mode\"}");
}

void test_obd_payload_is_preserved() {
    assertUnavailable(WifiSplitBootApiResponse::Operation::OBD_RUNTIME,
                      "{\"error\":\"maintenance_mode\","
                      "\"message\":\"OBD runtime endpoints are not available in maintenance mode\"}");
}

void test_alp_payload_is_preserved() {
    assertUnavailable(WifiSplitBootApiResponse::Operation::ALP_STATUS,
                      "{\"error\":\"maintenance_mode\","
                      "\"message\":\"ALP runtime status is not available in maintenance mode\"}");
}

void test_gps_payload_is_preserved() {
    assertUnavailable(WifiSplitBootApiResponse::Operation::GPS_STATUS,
                      "{\"error\":\"maintenance_mode\","
                      "\"message\":\"GPS runtime status is not available in maintenance mode\"}");
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_v1_push_pull_payload_is_preserved);
    RUN_TEST(test_auto_push_payload_is_preserved);
    RUN_TEST(test_obd_payload_is_preserved);
    RUN_TEST(test_alp_payload_is_preserved);
    RUN_TEST(test_gps_payload_is_preserved);
    return UNITY_END();
}
