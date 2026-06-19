#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/wifi/wifi_auto_start_module.cpp"

static WifiAutoStartModule module;
static int startCalls = 0;
static bool wifiAutoStartDone = false;
static bool wifiManualStartIntentLatched = false;
static bool lastStartAutoStarted = false;

static bool startWifiSuccess(bool autoStarted, void* /*ctx*/) {
    startCalls++;
    lastStartAutoStarted = autoStarted;
    return true;
}

static bool startWifiFail(bool autoStarted, void* /*ctx*/) {
    startCalls++;
    lastStartAutoStarted = autoStarted;
    return false;
}

static void resetState() {
    startCalls = 0;
    wifiAutoStartDone = false;
    wifiManualStartIntentLatched = false;
    lastStartAutoStarted = false;
}

static void assertGate(WifiAutoStartGate expectedGate,
                       bool expectedShouldAutoStart,
                       bool expectedStartTriggered) {
    const WifiAutoStartDecisionSnapshot& snapshot = module.getLastDecision();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expectedGate), static_cast<uint8_t>(snapshot.gate));
    TEST_ASSERT_EQUAL(expectedShouldAutoStart, snapshot.shouldAutoStart);
    TEST_ASSERT_EQUAL(expectedStartTriggered, snapshot.startTriggered);
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_noop_when_manual_start_not_requested() {
    const bool started = module.process(5000,
                                        1000,
                                        true,
                                        true,
                                        true,
                                        false,
                                        wifiManualStartIntentLatched,
                                        wifiAutoStartDone,
                                        startWifiSuccess,
                                        nullptr);

    TEST_ASSERT_FALSE(started);
    TEST_ASSERT_FALSE(wifiAutoStartDone);
    TEST_ASSERT_EQUAL_INT(0, startCalls);
    assertGate(WifiAutoStartGate::ManualStartNotRequested, false, false);
    TEST_ASSERT_TRUE(module.getLastDecision().enableWifi);
}

void test_noop_when_already_done() {
    wifiAutoStartDone = true;
    wifiManualStartIntentLatched = true;
    const bool started = module.process(5000,
                                        1000,
                                        true,
                                        true,
                                        true,
                                        true,
                                        wifiManualStartIntentLatched,
                                        wifiAutoStartDone,
                                        startWifiSuccess,
                                        nullptr);

    TEST_ASSERT_FALSE(started);
    TEST_ASSERT_TRUE(wifiAutoStartDone);
    TEST_ASSERT_FALSE(wifiManualStartIntentLatched);
    TEST_ASSERT_EQUAL_INT(0, startCalls);
    assertGate(WifiAutoStartGate::AlreadyDone, false, false);
    TEST_ASSERT_TRUE(module.getLastDecision().wifiAutoStartDone);
}

void test_noop_when_waiting_for_coordinator_open() {
    wifiManualStartIntentLatched = true;
    const bool started = module.process(2500,
                                        1000,
                                        true,
                                        true,
                                        true,
                                        false,
                                        wifiManualStartIntentLatched,
                                        wifiAutoStartDone,
                                        startWifiSuccess,
                                        nullptr);

    TEST_ASSERT_FALSE(started);
    TEST_ASSERT_FALSE(wifiAutoStartDone);
    TEST_ASSERT_EQUAL_INT(0, startCalls);
    assertGate(WifiAutoStartGate::WaitingCoordinatorOpen, false, false);
    TEST_ASSERT_TRUE(module.getLastDecision().bleConnected);
    TEST_ASSERT_FALSE(module.getLastDecision().bootTimeoutReached);
}

void test_starts_when_coordinator_allows_manual_start() {
    wifiManualStartIntentLatched = true;
    const bool started = module.process(5000,
                                        1000,
                                        true,
                                        true,
                                        true,
                                        true,
                                        wifiManualStartIntentLatched,
                                        wifiAutoStartDone,
                                        startWifiSuccess,
                                        nullptr);

    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_TRUE(wifiAutoStartDone);
    TEST_ASSERT_FALSE(wifiManualStartIntentLatched);
    TEST_ASSERT_EQUAL_INT(1, startCalls);
    TEST_ASSERT_FALSE(lastStartAutoStarted);
    assertGate(WifiAutoStartGate::Starting, true, true);
}

void test_manual_start_does_not_require_ble_once_allowed() {
    wifiManualStartIntentLatched = true;
    const bool started = module.process(30000,
                                        0,
                                        true,
                                        false,
                                        true,
                                        true,
                                        wifiManualStartIntentLatched,
                                        wifiAutoStartDone,
                                        startWifiSuccess,
                                        nullptr);

    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_TRUE(wifiAutoStartDone);
    TEST_ASSERT_FALSE(wifiManualStartIntentLatched);
    TEST_ASSERT_EQUAL_INT(1, startCalls);
    TEST_ASSERT_FALSE(lastStartAutoStarted);
    assertGate(WifiAutoStartGate::Starting, true, true);
    TEST_ASSERT_TRUE(module.getLastDecision().bootTimeoutReached);
}

void test_noop_when_dma_not_available() {
    wifiManualStartIntentLatched = true;
    const bool started = module.process(5000,
                                        1000,
                                        true,
                                        true,
                                        false,
                                        true,
                                        wifiManualStartIntentLatched,
                                        wifiAutoStartDone,
                                        startWifiSuccess,
                                        nullptr);

    TEST_ASSERT_FALSE(started);
    TEST_ASSERT_FALSE(wifiAutoStartDone);
    TEST_ASSERT_EQUAL_INT(0, startCalls);
    assertGate(WifiAutoStartGate::WaitingDma, false, false);
    TEST_ASSERT_FALSE(module.getLastDecision().canStartDma);
}

void test_v1_timestamp_ahead_of_now_saturates_elapsed() {
    wifiManualStartIntentLatched = true;
    const bool started = module.process(1000,
                                        2000,
                                        true,
                                        true,
                                        true,
                                        false,
                                        wifiManualStartIntentLatched,
                                        wifiAutoStartDone,
                                        startWifiSuccess,
                                        nullptr);

    TEST_ASSERT_FALSE(started);
    TEST_ASSERT_FALSE(wifiAutoStartDone);
    TEST_ASSERT_EQUAL_INT(0, startCalls);
    assertGate(WifiAutoStartGate::WaitingCoordinatorOpen, false, false);
    TEST_ASSERT_EQUAL_UINT32(0, module.getLastDecision().msSinceV1Connect);
}

void test_noop_when_wifi_master_disabled() {
    wifiManualStartIntentLatched = true;
    const bool started = module.process(5000,
                                        1000,
                                        false,
                                        true,
                                        true,
                                        true,
                                        wifiManualStartIntentLatched,
                                        wifiAutoStartDone,
                                        startWifiSuccess,
                                        nullptr);

    TEST_ASSERT_FALSE(started);
    TEST_ASSERT_FALSE(wifiAutoStartDone);
    TEST_ASSERT_FALSE(wifiManualStartIntentLatched);
    TEST_ASSERT_EQUAL_INT(0, startCalls);
    assertGate(WifiAutoStartGate::WifiDisabled, false, false);
    TEST_ASSERT_FALSE(module.getLastDecision().enableWifi);
}

void test_manual_start_stays_latched_until_coordinator_opens() {
    wifiManualStartIntentLatched = true;
    const bool started = module.process(10000,
                                        0,
                                        true,
                                        false,
                                        true,
                                        false,
                                        wifiManualStartIntentLatched,
                                        wifiAutoStartDone,
                                        startWifiSuccess,
                                        nullptr);

    TEST_ASSERT_FALSE(started);
    TEST_ASSERT_FALSE(wifiAutoStartDone);
    TEST_ASSERT_TRUE(wifiManualStartIntentLatched);
    TEST_ASSERT_EQUAL_INT(0, startCalls);
    assertGate(WifiAutoStartGate::WaitingCoordinatorOpen, false, false);
}

void test_failed_start_does_not_mark_auto_start_done() {
    wifiManualStartIntentLatched = true;
    const bool started = module.process(5000,
                                        1000,
                                        true,
                                        true,
                                        true,
                                        true,
                                        wifiManualStartIntentLatched,
                                        wifiAutoStartDone,
                                        startWifiFail,
                                        nullptr);

    TEST_ASSERT_FALSE(started);
    TEST_ASSERT_FALSE(wifiAutoStartDone);
    TEST_ASSERT_TRUE(wifiManualStartIntentLatched);
    TEST_ASSERT_EQUAL_INT(1, startCalls);
    TEST_ASSERT_FALSE(lastStartAutoStarted);
    TEST_ASSERT_TRUE(module.getLastDecision().startTriggered);
    TEST_ASSERT_FALSE(module.getLastDecision().startSucceeded);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(WifiAutoStartGate::StartFailed),
                            static_cast<uint8_t>(module.getLastDecision().gate));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_noop_when_manual_start_not_requested);
    RUN_TEST(test_noop_when_already_done);
    RUN_TEST(test_noop_when_waiting_for_coordinator_open);
    RUN_TEST(test_starts_when_coordinator_allows_manual_start);
    RUN_TEST(test_manual_start_does_not_require_ble_once_allowed);
    RUN_TEST(test_noop_when_dma_not_available);
    RUN_TEST(test_v1_timestamp_ahead_of_now_saturates_elapsed);
    RUN_TEST(test_noop_when_wifi_master_disabled);
    RUN_TEST(test_manual_start_stays_latched_until_coordinator_opens);
    RUN_TEST(test_failed_start_does_not_mark_auto_start_done);
    return UNITY_END();
}
