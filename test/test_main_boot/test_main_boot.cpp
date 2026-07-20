// Tests for pure-logic functions in main_boot.cpp:
//   - resetReasonToString(esp_reset_reason_t)
//
// Hardware-dependent boot helpers (logPanicBreadcrumbs, nvsHealthCheck,
// nextBootId, fatalBootError) require ESP32 peripherals and are not
// exercised here; they compile via the mocks but are not called.

#include <unity.h>
#include <ArduinoJson.h>

#include "../mocks/mock_heap_caps_state.h"
#include "../mocks/Arduino.h"
#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../mocks/esp_system.h"
#include "../mocks/esp_heap_caps.h"
#include "../mocks/esp_core_dump.h"
#include "../mocks/nvs_flash.h"
#include "../mocks/nvs.h"
#include "../mocks/LittleFS.h"
#include "../mocks/Preferences.h"
#include "../mocks/display.h"

// main_boot.cpp requires 'extern V1Display display'
V1Display display;

#include "../../src/littlefs_mount.cpp"
#include "../../src/main_boot.cpp"

// -----------------------------------------------------------------------
// resetReasonToString
// -----------------------------------------------------------------------

void test_reset_reason_poweron() {
    TEST_ASSERT_EQUAL_STRING("POWERON", resetReasonToString(ESP_RST_POWERON));
}

void test_reset_reason_sw() {
    TEST_ASSERT_EQUAL_STRING("SW", resetReasonToString(ESP_RST_SW));
}

void test_reset_reason_panic() {
    TEST_ASSERT_EQUAL_STRING("PANIC", resetReasonToString(ESP_RST_PANIC));
}

void test_reset_reason_int_wdt() {
    TEST_ASSERT_EQUAL_STRING("WDT_INT", resetReasonToString(ESP_RST_INT_WDT));
}

void test_reset_reason_task_wdt() {
    TEST_ASSERT_EQUAL_STRING("WDT_TASK", resetReasonToString(ESP_RST_TASK_WDT));
}

void test_reset_reason_wdt() {
    TEST_ASSERT_EQUAL_STRING("WDT", resetReasonToString(ESP_RST_WDT));
}

void test_reset_reason_deepsleep() {
    TEST_ASSERT_EQUAL_STRING("DEEPSLEEP", resetReasonToString(ESP_RST_DEEPSLEEP));
}

void test_reset_reason_brownout() {
    TEST_ASSERT_EQUAL_STRING("BROWNOUT", resetReasonToString(ESP_RST_BROWNOUT));
}

void test_reset_reason_sdio() {
    TEST_ASSERT_EQUAL_STRING("SDIO", resetReasonToString(ESP_RST_SDIO));
}

void test_reset_reason_unknown() {
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", resetReasonToString(ESP_RST_UNKNOWN));
}

void test_maintenance_boot_request_is_one_shot() {
    mock_preferences::reset();

    TEST_ASSERT_TRUE(requestMaintenanceBoot());
    TEST_ASSERT_TRUE(readAndClearMaintenanceBootRequest());
    TEST_ASSERT_FALSE(readAndClearMaintenanceBootRequest());
}

void test_maintenance_boot_request_reports_nvs_write_failure() {
    mock_preferences::reset();
    mock_preferences::set_fail_writes(true);

    TEST_ASSERT_FALSE(requestMaintenanceBoot());

    mock_preferences::set_fail_writes(false);
    TEST_ASSERT_FALSE(readAndClearMaintenanceBootRequest());
}

void test_aborted_shutdown_rewrites_clean_marker_to_unclean() {
    mock_preferences::reset();

    markCleanShutdown();
    markUncleanShutdown();

    TEST_ASSERT_FALSE(readAndResetCleanShutdownMarker());
}

// -----------------------------------------------------------------------

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();

    // resetReasonToString
    RUN_TEST(test_reset_reason_poweron);
    RUN_TEST(test_reset_reason_sw);
    RUN_TEST(test_reset_reason_panic);
    RUN_TEST(test_reset_reason_int_wdt);
    RUN_TEST(test_reset_reason_task_wdt);
    RUN_TEST(test_reset_reason_wdt);
    RUN_TEST(test_reset_reason_deepsleep);
    RUN_TEST(test_reset_reason_brownout);
    RUN_TEST(test_reset_reason_sdio);
    RUN_TEST(test_reset_reason_unknown);
    RUN_TEST(test_maintenance_boot_request_is_one_shot);
    RUN_TEST(test_maintenance_boot_request_reports_nvs_write_failure);
    RUN_TEST(test_aborted_shutdown_rewrites_clean_marker_to_unclean);

    return UNITY_END();
}
