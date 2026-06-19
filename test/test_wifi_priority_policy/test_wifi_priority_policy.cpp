#include <unity.h>

#include "../mocks/ble_client.h"
#include "../mocks/wifi_manager.h"

#include "../../src/modules/wifi/wifi_priority_policy_module.cpp"

static V1BLEClient ble;
static WiFiManager wifi;
static WifiPriorityPolicyModule policy;

void setUp() {
    ble.reset();
    wifi = WiFiManager();
    policy.reset();
}

void tearDown() {}

void test_wifi_processing_disabled_when_wifi_idle() {
    TEST_ASSERT_FALSE(isWifiProcessingEnabledPolicy(wifi));
}

void test_wifi_processing_enabled_for_service_pending_or_sta() {
    wifi.setWifiServiceActive(true);
    TEST_ASSERT_TRUE(isWifiProcessingEnabledPolicy(wifi));

    wifi = WiFiManager();
    wifi.setPendingLifecycleWork(true);
    TEST_ASSERT_TRUE(isWifiProcessingEnabledPolicy(wifi));

    wifi = WiFiManager();
    wifi.setConnected(true);
    TEST_ASSERT_TRUE(isWifiProcessingEnabledPolicy(wifi));
}

void test_priority_does_not_enable_until_hold_and_debounce_pass() {
    ble.setConnected(true);
    wifi.setUiActive(true);

    policy.apply(1000, ble, wifi);
    TEST_ASSERT_EQUAL(0, ble.setWifiPriorityCalls);

    policy.apply(13000, ble, wifi);
    TEST_ASSERT_EQUAL(0, ble.setWifiPriorityCalls);
    TEST_ASSERT_EQUAL(3500UL, wifi.lastUiTimeoutMs());

    policy.apply(14499, ble, wifi);
    TEST_ASSERT_EQUAL(0, ble.setWifiPriorityCalls);

    policy.apply(14500, ble, wifi);
    TEST_ASSERT_EQUAL(1, ble.setWifiPriorityCalls);
    TEST_ASSERT_TRUE(ble.isWifiPriority());
    TEST_ASSERT_TRUE(ble.lastWifiPriorityValue);
}

void test_priority_stays_off_when_v1_disconnected() {
    ble.setConnected(false);
    wifi.setUiActive(true);

    policy.apply(13000, ble, wifi);
    policy.apply(14500, ble, wifi);

    TEST_ASSERT_EQUAL(0, ble.setWifiPriorityCalls);
    TEST_ASSERT_FALSE(ble.isWifiPriority());
}

void test_priority_disable_uses_long_ui_timeout_and_debounce() {
    ble.setConnected(true);
    ble.setWifiPriorityForTest(true);
    wifi.setUiActive(false);

    policy.apply(13000, ble, wifi);
    TEST_ASSERT_EQUAL(20000UL, wifi.lastUiTimeoutMs());
    TEST_ASSERT_EQUAL(0, ble.setWifiPriorityCalls);

    policy.apply(14500, ble, wifi);
    TEST_ASSERT_EQUAL(1, ble.setWifiPriorityCalls);
    TEST_ASSERT_FALSE(ble.isWifiPriority());
    TEST_ASSERT_FALSE(ble.lastWifiPriorityValue);
}

void test_reset_clears_pending_transition() {
    ble.setConnected(true);
    wifi.setUiActive(true);

    policy.apply(13000, ble, wifi);
    policy.reset();
    policy.apply(14500, ble, wifi);

    TEST_ASSERT_EQUAL(0, ble.setWifiPriorityCalls);

    policy.apply(16000, ble, wifi);
    TEST_ASSERT_EQUAL(1, ble.setWifiPriorityCalls);
    TEST_ASSERT_TRUE(ble.isWifiPriority());
}

void runAllTests() {
    RUN_TEST(test_wifi_processing_disabled_when_wifi_idle);
    RUN_TEST(test_wifi_processing_enabled_for_service_pending_or_sta);
    RUN_TEST(test_priority_does_not_enable_until_hold_and_debounce_pass);
    RUN_TEST(test_priority_stays_off_when_v1_disconnected);
    RUN_TEST(test_priority_disable_uses_long_ui_timeout_and_debounce);
    RUN_TEST(test_reset_clears_pending_transition);
}

#ifdef ARDUINO
void setup() {
    delay(2000);
    UNITY_BEGIN();
    runAllTests();
    UNITY_END();
}
void loop() {}
#else
int main(int argc, char** argv) {
    UNITY_BEGIN();
    runAllTests();
    return UNITY_END();
}
#endif
