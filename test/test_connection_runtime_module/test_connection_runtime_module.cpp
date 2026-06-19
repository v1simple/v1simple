#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/ble_client.h"
#include "../mocks/modules/ble/ble_queue_module.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/ble/connection_runtime_module.cpp"

static V1BLEClient ble;
static BleQueueModule bleQueue;
static ConnectionRuntimeModule module;

static bool isBleConnected(void* ctx) {
    return static_cast<V1BLEClient*>(ctx)->isConnected();
}

static bool isBackpressured(void* ctx) {
    return static_cast<BleQueueModule*>(ctx)->isBackpressured();
}

static unsigned long getLastRxMillis(void* ctx) {
    return static_cast<BleQueueModule*>(ctx)->getLastRxMillis();
}

void setUp() {
    ble.reset();
    bleQueue.reset();
    ConnectionRuntimeModule::Providers providers;
    providers.isBleConnected = isBleConnected;
    providers.isBackpressured = isBackpressured;
    providers.getLastRxMillis = getLastRxMillis;
    providers.bleContext = &ble;
    providers.queueContext = &bleQueue;
    module.begin(providers);
}

void tearDown() {}

void test_boot_hold_expiry_requests_scanning_when_disconnected() {
    ble.setConnected(false);

    const auto snapshot = module.process(
        500,   // nowMs
        1000,  // nowUs
        0,     // lastLoopUs
        true,  // boot splash hold active
        400,   // hold until
        false  // initial scanning shown
    );

    TEST_ASSERT_FALSE(snapshot.bootSplashHoldActive);
    TEST_ASSERT_TRUE(snapshot.requestShowInitialScanning);
    TEST_ASSERT_FALSE(snapshot.initialScanningScreenShown);
}

void test_boot_hold_expiry_marks_scanning_shown_when_connected() {
    ble.setConnected(true);

    const auto snapshot = module.process(
        500,   // nowMs
        1000,  // nowUs
        0,
        true,
        400,
        false);

    TEST_ASSERT_FALSE(snapshot.bootSplashHoldActive);
    TEST_ASSERT_FALSE(snapshot.requestShowInitialScanning);
    TEST_ASSERT_TRUE(snapshot.initialScanningScreenShown);
}

void test_receiving_heartbeat_follows_last_rx_window() {
    bleQueue.setLastRxMillis(1000);

    auto snapshotFresh = module.process(2500, 1000, 0, false, 0, false);
    TEST_ASSERT_TRUE(snapshotFresh.receiving);  // 1500ms since last RX

    auto snapshotStale = module.process(3501, 2000, 0, false, 0, false);
    TEST_ASSERT_FALSE(snapshotStale.receiving);  // 2501ms since last RX
}

void test_receiving_stays_false_until_first_rx_arrives() {
    auto snapshotBeforeRx = module.process(500, 1000, 0, false, 0, false);
    TEST_ASSERT_FALSE(snapshotBeforeRx.receiving);

    bleQueue.setLastRxMillis(250);
    auto snapshotAfterRx = module.process(500, 2000, 0, false, 0, false);
    TEST_ASSERT_TRUE(snapshotAfterRx.receiving);
}

void test_backpressure_sets_skip_non_core_and_overload() {
    // Prime the runtime tick state.
    module.process(100, 1000, 0, false, 0, false);

    bleQueue.setBackpressured(true);
    const auto snapshot = module.process(200, 2000, 0, false, 0, false);

    TEST_ASSERT_TRUE(snapshot.backpressured);
    TEST_ASSERT_TRUE(snapshot.skipNonCore);
    TEST_ASSERT_TRUE(snapshot.overloaded);
}

void test_last_loop_overload_threshold_is_honored() {
    const auto snapshot = module.process(
        100,
        1000,
        30000,  // exceeds overloadLoopUs
        false,
        0,
        false);

    TEST_ASSERT_TRUE(snapshot.overloaded);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_boot_hold_expiry_requests_scanning_when_disconnected);
    RUN_TEST(test_boot_hold_expiry_marks_scanning_shown_when_connected);
    RUN_TEST(test_receiving_heartbeat_follows_last_rx_window);
    RUN_TEST(test_receiving_stays_false_until_first_rx_arrives);
    RUN_TEST(test_backpressure_sets_skip_non_core_and_overload);
    RUN_TEST(test_last_loop_overload_threshold_is_honored);
    return UNITY_END();
}
