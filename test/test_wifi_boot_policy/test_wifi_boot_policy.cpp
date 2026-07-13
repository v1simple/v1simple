// =============================================================================
// WiFi boot policy tests
//
// Pure-function tests for the deferred WiFi start gating logic extracted
// from main.cpp setup()/loop().  No hardware or RTOS dependencies.
//
// Policy under test:
//   WifiBootPolicy::shouldAutoStartWifi()
//   WifiBootPolicy::shouldProcessWifi()
// =============================================================================

#include <unity.h>
#include "../../src/modules/wifi/wifi_boot_policy.h"

using namespace WifiBootPolicy;

// Default settle / timeout constants for deferred start policy tests.
static constexpr uint32_t SETTLE_MS  = 3000;
static constexpr uint32_t TIMEOUT_MS = 30000;

void setUp() {}
void tearDown() {}

// ─── shouldAutoStartWifi ────────────────────────────────────────────

void test_blocked_when_already_started() {
    TEST_ASSERT_FALSE(shouldAutoStartWifi(
        true,   // alreadyStarted — already done
        true,   // bleConnected
        5000,
        SETTLE_MS,
        60000,
        TIMEOUT_MS,
        true));
}

void test_blocked_before_ble_settle_and_before_timeout() {
    // BLE connected but only 1 s into settle (need 3 s), boot at 8 s (need 30 s)
    TEST_ASSERT_FALSE(shouldAutoStartWifi(
        false,  // alreadyStarted
        true,   // bleConnected
        1000,   // msSinceV1Connect  (< SETTLE_MS)
        SETTLE_MS,
        8000,   // msSinceBoot       (< TIMEOUT_MS)
        TIMEOUT_MS,
        true));
}

void test_blocked_when_ble_not_connected_and_before_timeout() {
    TEST_ASSERT_FALSE(shouldAutoStartWifi(
        false,
        false,  // bleConnected = no
        0,
        SETTLE_MS,
        15000,  // before 30 s timeout
        TIMEOUT_MS,
        true));
}

void test_allowed_after_ble_settle() {
    TEST_ASSERT_TRUE(shouldAutoStartWifi(
        false,
        true,   // bleConnected
        3000,   // exactly at settle boundary
        SETTLE_MS,
        10000,  // well before timeout — doesn't matter
        TIMEOUT_MS,
        true));
}

void test_allowed_after_ble_settle_with_margin() {
    TEST_ASSERT_TRUE(shouldAutoStartWifi(
        false,
        true,
        5000,   // well past settle
        SETTLE_MS,
        12000,
        TIMEOUT_MS,
        true));
}

void test_allowed_on_timeout_without_ble() {
    TEST_ASSERT_TRUE(shouldAutoStartWifi(
        false,
        false,  // BLE never connected
        0,
        SETTLE_MS,
        30000,  // exactly at timeout boundary
        TIMEOUT_MS,
        true));
}

void test_allowed_on_timeout_past_deadline() {
    TEST_ASSERT_TRUE(shouldAutoStartWifi(
        false,
        false,
        0,
        SETTLE_MS,
        45000,  // well past timeout
        TIMEOUT_MS,
        true));
}

void test_blocked_when_dma_check_fails_despite_settle() {
    TEST_ASSERT_FALSE(shouldAutoStartWifi(
        false,
        true,
        5000,
        SETTLE_MS,
        10000,
        TIMEOUT_MS,
        false)); // canStartDma = false
}

void test_blocked_when_dma_check_fails_despite_timeout() {
    TEST_ASSERT_FALSE(shouldAutoStartWifi(
        false,
        false,
        0,
        SETTLE_MS,
        60000,
        TIMEOUT_MS,
        false)); // canStartDma = false
}

// Edge: BLE connected at exactly settle boundary, boot also at timeout
void test_ble_settle_takes_priority_over_timeout() {
    // Both conditions true simultaneously — should still return true
    TEST_ASSERT_TRUE(shouldAutoStartWifi(
        false,
        true,
        3000,    // settle met
        SETTLE_MS,
        30000,   // timeout also met
        TIMEOUT_MS,
        true));
}

// Edge: zero settle means "start immediately on BLE connect"
void test_zero_settle_starts_immediately_on_ble() {
    TEST_ASSERT_TRUE(shouldAutoStartWifi(
        false,
        true,
        0,       // just connected
        0,       // zero settle
        5000,
        TIMEOUT_MS,
        true));
}

// ─── shouldProcessWifi ──────────────────────────────────────────────

void test_process_allowed_when_setup_mode_active() {
    TEST_ASSERT_TRUE(shouldProcessWifi(
        true,   // setupModeActive
        false,  // wifiClientConnected
        false));// wifiAutoStartDone
}

void test_process_allowed_when_client_connected() {
    TEST_ASSERT_TRUE(shouldProcessWifi(
        false,
        true,   // wifiClientConnected
        false));
}

void test_process_blocked_when_wifi_entirely_off() {
    TEST_ASSERT_FALSE(shouldProcessWifi(
        false,  // setupModeActive
        false,  // wifiClientConnected
        false));// wifiAutoStartDone
}

void test_process_blocked_after_autostart_done_and_ap_off() {
    // Auto-start completed but AP subsequently stopped
    TEST_ASSERT_FALSE(shouldProcessWifi(
        false,  // setupModeActive — AP was stopped
        false,  // wifiClientConnected
        true)); // autostart already done
}

int main() {
    UNITY_BEGIN();

    // shouldAutoStartWifi
    RUN_TEST(test_blocked_when_already_started);
    RUN_TEST(test_blocked_before_ble_settle_and_before_timeout);
    RUN_TEST(test_blocked_when_ble_not_connected_and_before_timeout);
    RUN_TEST(test_allowed_after_ble_settle);
    RUN_TEST(test_allowed_after_ble_settle_with_margin);
    RUN_TEST(test_allowed_on_timeout_without_ble);
    RUN_TEST(test_allowed_on_timeout_past_deadline);
    RUN_TEST(test_blocked_when_dma_check_fails_despite_settle);
    RUN_TEST(test_blocked_when_dma_check_fails_despite_timeout);
    RUN_TEST(test_ble_settle_takes_priority_over_timeout);
    RUN_TEST(test_zero_settle_starts_immediately_on_ble);

    // shouldProcessWifi
    RUN_TEST(test_process_allowed_when_setup_mode_active);
    RUN_TEST(test_process_allowed_when_client_connected);
    RUN_TEST(test_process_blocked_when_wifi_entirely_off);
    RUN_TEST(test_process_blocked_after_autostart_done_and_ap_off);

    return UNITY_END();
}
