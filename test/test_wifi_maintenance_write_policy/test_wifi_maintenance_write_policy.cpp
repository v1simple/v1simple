#include <unity.h>

#include "../../src/modules/wifi/wifi_maintenance_write_policy.h"

using WifiMaintenanceWritePolicy::Decision;

void setUp() {}
void tearDown() {}

void test_rejects_missing_header_outside_maintenance_boot() {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Decision::RejectNotMaintenance),
        static_cast<int>(WifiMaintenanceWritePolicy::evaluate(false, false)));
}

void test_rejects_valid_header_outside_maintenance_boot() {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Decision::RejectNotMaintenance),
        static_cast<int>(WifiMaintenanceWritePolicy::evaluate(false, true)));
}

void test_rejects_missing_header_during_maintenance_boot() {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Decision::RejectHeader),
        static_cast<int>(WifiMaintenanceWritePolicy::evaluate(true, false)));
}

void test_allows_valid_header_during_maintenance_boot() {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Decision::Allow),
        static_cast<int>(WifiMaintenanceWritePolicy::evaluate(true, true)));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_rejects_missing_header_outside_maintenance_boot);
    RUN_TEST(test_rejects_valid_header_outside_maintenance_boot);
    RUN_TEST(test_rejects_missing_header_during_maintenance_boot);
    RUN_TEST(test_allows_valid_header_during_maintenance_boot);
    return UNITY_END();
}
