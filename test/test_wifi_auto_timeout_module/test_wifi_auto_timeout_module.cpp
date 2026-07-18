#include <unity.h>

#include "../../src/modules/wifi/wifi_auto_timeout_module.cpp"

static WifiAutoTimeoutInput baseInput() {
    WifiAutoTimeoutInput input;
    input.timeoutMins = 1;
    input.setupModeActive = true;
    input.nowMs = 60000;
    input.setupModeStartMs = 0;
    input.lastClientSeenMs = 0;
    input.lastUiActivityMs = 0;
    input.staCount = 0;
    input.inactivityGraceMs = 60000;
    return input;
}

void test_disabled_timeout_never_stops() {
    WifiAutoTimeoutModule module;
    WifiAutoTimeoutInput input = baseInput();
    input.timeoutMins = 0;

    const WifiAutoTimeoutResult result = module.evaluate(input);
    TEST_ASSERT_FALSE(result.timeoutEnabled);
    TEST_ASSERT_FALSE(result.shouldStop);
}

void test_inactive_elapsed_no_clients_stops() {
    WifiAutoTimeoutModule module;
    WifiAutoTimeoutInput input = baseInput();

    const WifiAutoTimeoutResult result = module.evaluate(input);
    TEST_ASSERT_TRUE(result.timeoutEnabled);
    TEST_ASSERT_TRUE(result.timeoutElapsed);
    TEST_ASSERT_TRUE(result.inactiveEnough);
    TEST_ASSERT_TRUE(result.shouldStop);
}

void test_recent_ui_activity_blocks_stop() {
    WifiAutoTimeoutModule module;
    WifiAutoTimeoutInput input = baseInput();
    input.lastUiActivityMs = 55000;  // 5s ago

    const WifiAutoTimeoutResult result = module.evaluate(input);
    TEST_ASSERT_TRUE(result.timeoutElapsed);
    TEST_ASSERT_FALSE(result.inactiveEnough);
    TEST_ASSERT_FALSE(result.shouldStop);
}

void test_client_presence_blocks_stop() {
    WifiAutoTimeoutModule module;
    WifiAutoTimeoutInput input = baseInput();
    input.staCount = 1;

    const WifiAutoTimeoutResult result = module.evaluate(input);
    TEST_ASSERT_TRUE(result.timeoutElapsed);
    TEST_ASSERT_TRUE(result.inactiveEnough);
    TEST_ASSERT_FALSE(result.shouldStop);
}

void test_uses_newest_activity_between_ui_and_client() {
    WifiAutoTimeoutModule module;
    WifiAutoTimeoutInput input = baseInput();
    input.nowMs = 120000;
    input.lastUiActivityMs = 10000;
    input.lastClientSeenMs = 90000;
    input.inactivityGraceMs = 30000;

    const WifiAutoTimeoutResult result = module.evaluate(input);
    TEST_ASSERT_EQUAL_UINT32(90000u, result.lastActivityMs);
    TEST_ASSERT_TRUE(result.timeoutElapsed);
    TEST_ASSERT_TRUE(result.inactiveEnough);   // 30s since 90000
    TEST_ASSERT_TRUE(result.shouldStop);
}

void test_setup_mode_inactive_never_stops() {
    WifiAutoTimeoutModule module;
    WifiAutoTimeoutInput input = baseInput();
    input.setupModeActive = false;

    const WifiAutoTimeoutResult result = module.evaluate(input);
    TEST_ASSERT_FALSE(result.shouldStop);
}

// Regression (escaped-bug class, 2026-07): the idle auto-timeout must never
// stop WiFi underneath a deliberate maintenance session — a stopped AP there
// has no user-visible recovery besides waiting for the maintenance reboot.
void test_maintenance_boot_suppresses_stop() {
    WifiAutoTimeoutModule module;
    WifiAutoTimeoutInput input = baseInput();  // would stop otherwise
    input.maintenanceBootMode = true;

    const WifiAutoTimeoutResult result = module.evaluate(input);
    TEST_ASSERT_TRUE(result.timeoutEnabled);
    TEST_ASSERT_TRUE(result.maintenanceSuppressed);
    TEST_ASSERT_FALSE(result.shouldStop);
}

void test_non_maintenance_not_marked_suppressed() {
    WifiAutoTimeoutModule module;
    WifiAutoTimeoutInput input = baseInput();

    const WifiAutoTimeoutResult result = module.evaluate(input);
    TEST_ASSERT_FALSE(result.maintenanceSuppressed);
    TEST_ASSERT_TRUE(result.shouldStop);
}

static WifiNoClientTimeoutInput baseNoClientInput() {
    WifiNoClientTimeoutInput input;
    input.nowMs = 61000UL;
    input.lastAnyClientSeenMs = 1000UL;
    input.manualTimeoutMs = 60000UL;
    input.autoTimeoutMs = 30000UL;
    return input;
}

void test_maintenance_boot_suppresses_no_client_stop() {
    WifiAutoTimeoutModule module;
    WifiNoClientTimeoutInput input = baseNoClientInput();
    input.maintenanceBootMode = true;

    const WifiNoClientTimeoutResult result = module.evaluateNoClient(input);
    TEST_ASSERT_TRUE(result.maintenanceSuppressed);
    TEST_ASSERT_TRUE(result.refreshLastSeen);
    TEST_ASSERT_FALSE(result.shouldStop);
}

void test_no_client_timeout_uses_manual_and_auto_limits() {
    WifiAutoTimeoutModule module;
    WifiNoClientTimeoutInput input = baseNoClientInput();

    WifiNoClientTimeoutResult result = module.evaluateNoClient(input);
    TEST_ASSERT_EQUAL_UINT64(60000UL, result.timeoutMs);
    TEST_ASSERT_TRUE(result.shouldStop);

    input.autoStarted = true;
    input.nowMs = 31000UL;
    result = module.evaluateNoClient(input);
    TEST_ASSERT_EQUAL_UINT64(30000UL, result.timeoutMs);
    TEST_ASSERT_TRUE(result.shouldStop);
}

void test_client_or_pending_sta_connect_refreshes_no_client_baseline() {
    WifiAutoTimeoutModule module;
    WifiNoClientTimeoutInput input = baseNoClientInput();
    input.clientPresent = true;
    TEST_ASSERT_TRUE(module.evaluateNoClient(input).refreshLastSeen);

    input.clientPresent = false;
    input.staConnectInProgress = true;
    const WifiNoClientTimeoutResult connecting = module.evaluateNoClient(input);
    TEST_ASSERT_TRUE(connecting.refreshLastSeen);
    TEST_ASSERT_FALSE(connecting.shouldStop);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_disabled_timeout_never_stops);
    RUN_TEST(test_inactive_elapsed_no_clients_stops);
    RUN_TEST(test_recent_ui_activity_blocks_stop);
    RUN_TEST(test_client_presence_blocks_stop);
    RUN_TEST(test_uses_newest_activity_between_ui_and_client);
    RUN_TEST(test_setup_mode_inactive_never_stops);
    RUN_TEST(test_maintenance_boot_suppresses_stop);
    RUN_TEST(test_non_maintenance_not_marked_suppressed);
    RUN_TEST(test_maintenance_boot_suppresses_no_client_stop);
    RUN_TEST(test_no_client_timeout_uses_manual_and_auto_limits);
    RUN_TEST(test_client_or_pending_sta_connect_refreshes_no_client_baseline);
    return UNITY_END();
}
