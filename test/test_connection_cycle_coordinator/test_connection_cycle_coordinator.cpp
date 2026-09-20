#include <unity.h>

#include "../mocks/Arduino.h"
#include "../../src/modules/system/connection_cycle_coordinator_module.cpp"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

namespace {

struct ProviderProbe final : ConnectionCycleLifecycle {
    uint32_t stopObdScanCalls = 0;
    uint32_t cancelObdConnectCalls = 0;
    uint32_t stopProxyAdvertisingCalls = 0;
    uint32_t disconnectProxyPhoneCalls = 0;
    bool obdScanStopped = true;
    bool obdConnectIdle = true;
    bool proxyFullyStopped = true;
    void stopObdScan() override { ++stopObdScanCalls; }
    void cancelObdConnect() override { ++cancelObdConnectCalls; }
    void stopProxyAdvertising() override { ++stopProxyAdvertisingCalls; }
    void disconnectProxyPhones() override { ++disconnectProxyPhoneCalls; }
    bool isObdScanStopped() const override { return obdScanStopped; }
    bool isObdConnectIdle() const override { return obdConnectIdle; }
    bool isProxyFullyStopped() const override { return proxyFullyStopped; }
};

CycleContext connectedContext(uint32_t nowMs) {
    CycleContext ctx;
    ctx.nowMs = nowMs;
    ctx.bootReady = true;
    ctx.v1GattConnected = true;
    ctx.v1LastEventMs = 100;
    ctx.obdScanWindowMs = 1000;
    ctx.obdRetryIntervalMs = 30000;
    ctx.proxyOpenWindowMs = 1000;
    ctx.v1SettleFallbackMs = 500;
    ctx.cycleTeardownAckTimeoutMs = 25;
    return ctx;
}

void enterPostV1Phase(ConnectionCycleCoordinatorModule& module, CycleContext& ctx) {
    ctx.nowMs = 100;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::V1_SETTLING, module.state());
    ctx.nowMs = 600;
    module.update(ctx);
}

} // namespace

void setUp() {}
void tearDown() {}

void test_proxy_window_exits_directly_to_steady_without_wifi_dwell() {
    ProviderProbe probe;
    ConnectionCycleCoordinatorModule module;
    module.begin(probe);
    CycleContext ctx = connectedContext(0);
    ctx.proxyEnabled = true;
    ctx.obdEnabled = true;

    enterPostV1Phase(module, ctx);
    TEST_ASSERT_EQUAL(CycleState::OBD_SCAN, module.state());

    ctx.nowMs = 1600;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::PROXY_OPEN, module.state());

    ctx.nowMs = 2599;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::PROXY_OPEN, module.state());

    ctx.nowMs = 2600;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::STEADY, module.state());
    TEST_ASSERT_EQUAL_UINT32(1, probe.stopProxyAdvertisingCalls);
    TEST_ASSERT_EQUAL_UINT32(4, module.totalTransitionCount());
}

void test_obd_failure_reaches_steady_and_retry_keeps_original_attempt_anchor() {
    ProviderProbe probe;
    ConnectionCycleCoordinatorModule module;
    module.begin(probe);
    CycleContext ctx = connectedContext(0);
    ctx.obdEnabled = true;
    ctx.obdSavedAddressValid = true;

    enterPostV1Phase(module, ctx);
    TEST_ASSERT_EQUAL(CycleState::OBD_SCAN, module.state());

    ctx.nowMs = 601;
    ctx.obdState = ObdConnectionState::CONNECTING;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::OBD_CONNECT, module.state());

    ctx.nowMs = 602;
    ctx.obdState = ObdConnectionState::ERROR_BACKOFF;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::STEADY, module.state());
    TEST_ASSERT_EQUAL_UINT32(1, probe.cancelObdConnectCalls);

    TEST_ASSERT_FALSE(module.obdRetryAllowed(30600));
    TEST_ASSERT_TRUE(module.obdRetryAllowed(30601));
}

void test_obd_attempt_at_millis_zero_still_enables_bounded_retry() {
    ProviderProbe probe;
    ConnectionCycleCoordinatorModule module;
    module.begin(probe);
    CycleContext ctx = connectedContext(UINT32_MAX - 500u);
    ctx.v1LastEventMs = UINT32_MAX - 500u;
    ctx.obdEnabled = true;
    ctx.obdSavedAddressValid = true;

    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::V1_SETTLING, module.state());

    ctx.nowMs = 0u;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::OBD_SCAN, module.state());

    ctx.nowMs = 1000u;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::STEADY, module.state());
    TEST_ASSERT_FALSE(module.obdRetryAllowed(29999u));
    TEST_ASSERT_TRUE(module.obdRetryAllowed(30000u));
}

void test_obd_scan_timeout_stops_scan_before_opening_proxy() {
    ProviderProbe probe;
    ConnectionCycleCoordinatorModule module;
    module.begin(probe);
    CycleContext ctx = connectedContext(0);
    ctx.obdEnabled = true;
    ctx.obdSavedAddressValid = true;
    ctx.proxyEnabled = true;
    ctx.proxyClientConnectedOnceThisBoot = true;

    enterPostV1Phase(module, ctx);
    TEST_ASSERT_EQUAL(CycleState::OBD_SCAN, module.state());

    ctx.nowMs = 1600;
    module.update(ctx);
    TEST_ASSERT_EQUAL_UINT32(1, probe.stopObdScanCalls);
    TEST_ASSERT_EQUAL(CycleState::PROXY_OPEN, module.state());
    TEST_ASSERT_TRUE(module.proxyAdvertisingAllowed());
}

void test_successful_obd_settle_is_not_reordered_behind_proxy() {
    ProviderProbe probe;
    ConnectionCycleCoordinatorModule module;
    module.begin(probe);
    CycleContext ctx = connectedContext(0);
    ctx.obdEnabled = true;
    ctx.obdSavedAddressValid = true;
    ctx.proxyEnabled = true;

    enterPostV1Phase(module, ctx);
    TEST_ASSERT_EQUAL(CycleState::OBD_SCAN, module.state());

    ctx.nowMs = 700;
    ctx.obdConnected = true;
    ctx.obdState = ObdConnectionState::POLLING;
    ctx.obdHasValidSpeedSample = true;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::OBD_SETTLED, module.state());

    ctx.nowMs = 701;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::STEADY, module.state());
    TEST_ASSERT_FALSE(module.proxyAdvertisingAllowed());
}

void test_v1_drop_teardown_returns_to_scan_before_any_new_obd_attempt() {
    ProviderProbe probe;
    ConnectionCycleCoordinatorModule module;
    module.begin(probe);
    CycleContext ctx = connectedContext(0);

    enterPostV1Phase(module, ctx);
    TEST_ASSERT_EQUAL(CycleState::STEADY, module.state());

    ctx.nowMs = 700;
    ctx.v1GattConnected = false;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::TEARDOWN, module.state());
    TEST_ASSERT_EQUAL_UINT32(1, probe.stopObdScanCalls);
    TEST_ASSERT_EQUAL_UINT32(1, probe.cancelObdConnectCalls);

    ctx.nowMs = 701;
    module.update(ctx);
    TEST_ASSERT_EQUAL_UINT32(1, probe.stopProxyAdvertisingCalls);
    TEST_ASSERT_EQUAL_UINT32(1, probe.disconnectProxyPhoneCalls);

    ctx.nowMs = 702;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::SCAN_V1, module.state());
}

void test_verify_push_quiet_window_waits_for_both_ordinary_anchors() {
    ProviderProbe probe;
    ConnectionCycleCoordinatorModule module;
    module.begin(probe);
    CycleContext ctx = connectedContext(100);
    ctx.autoPushEnabled = true;
    ctx.obdEnabled = true;
    ctx.v1SettleQuietMs = 100;

    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::V1_SETTLING, module.state());

    ctx.nowMs = 110;
    ctx.v1VerifyPushMatchEdge = true;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::V1_SETTLING, module.state());

    ctx.v1VerifyPushMatchEdge = false;
    ctx.nowMs = 209;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::V1_SETTLING, module.state());

    ctx.nowMs = 210;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::OBD_SCAN, module.state());
}

void test_verify_push_quiet_window_is_wrap_safe_when_match_is_newer() {
    ProviderProbe probe;
    ConnectionCycleCoordinatorModule module;
    module.begin(probe);
    CycleContext ctx = connectedContext(0xFFFFFF00u);
    ctx.v1LastEventMs = 0xFFFFFF00u;
    ctx.autoPushEnabled = true;
    ctx.obdEnabled = true;
    ctx.v1SettleQuietMs = 100;

    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::V1_SETTLING, module.state());

    ctx.nowMs = 0x00000010u;
    ctx.v1VerifyPushMatchEdge = true;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::V1_SETTLING, module.state());

    ctx.v1VerifyPushMatchEdge = false;
    ctx.nowMs = 0x00000073u;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::V1_SETTLING, module.state());

    ctx.nowMs = 0x00000074u;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::OBD_SCAN, module.state());
}

void test_verify_push_quiet_window_is_wrap_safe_when_v1_event_is_newer() {
    ProviderProbe probe;
    ConnectionCycleCoordinatorModule module;
    module.begin(probe);
    CycleContext ctx = connectedContext(0xFFFFFF00u);
    ctx.v1LastEventMs = 0xFFFFFF00u;
    ctx.autoPushEnabled = true;
    ctx.obdEnabled = true;
    ctx.v1SettleQuietMs = 100;

    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::V1_SETTLING, module.state());

    ctx.nowMs = 0xFFFFFF10u;
    ctx.v1VerifyPushMatchEdge = true;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::V1_SETTLING, module.state());

    ctx.v1VerifyPushMatchEdge = false;
    ctx.v1LastEventMs = 0x00000020u;
    ctx.nowMs = 0x00000083u;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::V1_SETTLING, module.state());

    ctx.nowMs = 0x00000084u;
    module.update(ctx);
    TEST_ASSERT_EQUAL(CycleState::OBD_SCAN, module.state());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_proxy_window_exits_directly_to_steady_without_wifi_dwell);
    RUN_TEST(test_obd_failure_reaches_steady_and_retry_keeps_original_attempt_anchor);
    RUN_TEST(test_obd_attempt_at_millis_zero_still_enables_bounded_retry);
    RUN_TEST(test_obd_scan_timeout_stops_scan_before_opening_proxy);
    RUN_TEST(test_successful_obd_settle_is_not_reordered_behind_proxy);
    RUN_TEST(test_v1_drop_teardown_returns_to_scan_before_any_new_obd_attempt);
    RUN_TEST(test_verify_push_quiet_window_waits_for_both_ordinary_anchors);
    RUN_TEST(test_verify_push_quiet_window_is_wrap_safe_when_match_is_newer);
    RUN_TEST(test_verify_push_quiet_window_is_wrap_safe_when_v1_event_is_newer);
    return UNITY_END();
}
