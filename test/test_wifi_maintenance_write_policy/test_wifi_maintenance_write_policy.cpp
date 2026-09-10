/**
 * test_wifi_maintenance_write_policy.cpp
 *
 * Pure-logic tests for the dependency-free maintenance write admission policy
 * and the request-shape literals shared with the maintenance WebUI.
 */

#include <unity.h>

#include "../../src/modules/wifi/wifi_maintenance_write_policy.h"

void setUp() {}
void tearDown() {}

void test_non_maintenance_mode_rejects_before_header_validation() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMaintenanceWritePolicy::Decision::RejectNotMaintenance),
                          static_cast<int>(WifiMaintenanceWritePolicy::evaluate(false, false)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMaintenanceWritePolicy::Decision::RejectNotMaintenance),
                          static_cast<int>(WifiMaintenanceWritePolicy::evaluate(false, true)));
}

void test_maintenance_mode_rejects_missing_write_header() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMaintenanceWritePolicy::Decision::RejectHeader),
                          static_cast<int>(WifiMaintenanceWritePolicy::evaluate(true, false)));
}

void test_maintenance_mode_allows_valid_write_header() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WifiMaintenanceWritePolicy::Decision::Allow),
                          static_cast<int>(WifiMaintenanceWritePolicy::evaluate(true, true)));
}

void test_request_shape_literals_match_the_webui_contract() {
    TEST_ASSERT_EQUAL_STRING("X-V1Simple-Request", WifiMaintenanceWritePolicy::kRequestShapeHeader);
    TEST_ASSERT_EQUAL_STRING("maintenance-ui", WifiMaintenanceWritePolicy::kRequestShapeValue);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_non_maintenance_mode_rejects_before_header_validation);
    RUN_TEST(test_maintenance_mode_rejects_missing_write_header);
    RUN_TEST(test_maintenance_mode_allows_valid_write_header);
    RUN_TEST(test_request_shape_literals_match_the_webui_contract);
    return UNITY_END();
}
