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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_disabled_timeout_never_stops);
    RUN_TEST(test_inactive_elapsed_no_clients_stops);
    RUN_TEST(test_recent_ui_activity_blocks_stop);
    RUN_TEST(test_client_presence_blocks_stop);
    RUN_TEST(test_uses_newest_activity_between_ui_and_client);
    RUN_TEST(test_setup_mode_inactive_never_stops);
    return UNITY_END();
}
