#include <unity.h>

#include "../../src/modules/system/connection_cycle_coordinator_module.cpp"

static ConnectionCycleCoordinatorModule module;

namespace {

struct ProviderState {
    int stopObdScanCalls = 0;
    int cancelObdConnectCalls = 0;
    int stopProxyAdvertisingCalls = 0;
    int disconnectProxyPhoneCalls = 0;
    bool obdScanStopped = false;
    bool obdConnectIdle = false;
    bool proxyFullyStopped = false;
};

ProviderState providerState;

void resetProviderState() {
    providerState = ProviderState{};
}


void stopObdScan(void*) {
    providerState.stopObdScanCalls++;
}

void cancelObdConnect(void*) {
    providerState.cancelObdConnectCalls++;
}

void stopProxyAdvertising(void*) {
    providerState.stopProxyAdvertisingCalls++;
}

void disconnectProxyPhone(void*) {
    providerState.disconnectProxyPhoneCalls++;
}

bool isObdScanStopped(void*) {
    return providerState.obdScanStopped;
}

bool isObdConnectIdle(void*) {
    return providerState.obdConnectIdle;
}

bool isProxyFullyStopped(void*) {
    return providerState.proxyFullyStopped;
}

ConnectionCycleCoordinatorModule::Providers makeProviders() {
    ConnectionCycleCoordinatorModule::Providers providers;
    providers.stopObdScan = stopObdScan;
    providers.cancelObdConnect = cancelObdConnect;
    providers.stopProxyAdvertising = stopProxyAdvertising;
    providers.disconnectProxyPhone = disconnectProxyPhone;
    providers.isObdScanStopped = isObdScanStopped;
    providers.isObdConnectIdle = isObdConnectIdle;
    providers.isProxyFullyStopped = isProxyFullyStopped;
    return providers;
}

CycleContext makeContext(const uint32_t nowMs) {
    CycleContext ctx;
    ctx.nowMs = nowMs;
    ctx.bootReady = true;
    ctx.proxyEnabled = true;
    ctx.obdScanWindowMs = kConnectionCycleObdScanWindowMsDefault;
    ctx.obdRetryIntervalMs = kConnectionCycleObdRetryIntervalMsDefault;
    ctx.proxyOpenWindowMs = kConnectionCycleProxyOpenWindowMsDefault;
    ctx.wifiOpenTimeoutMs = kConnectionCycleWifiOpenTimeoutMsDefault;
    ctx.v1SettleQuietMs = kConnectionCycleV1SettleQuietMsDefault;
    ctx.v1SettleFallbackMs = kConnectionCycleV1SettleFallbackMsDefault;
    ctx.cycleTeardownAckTimeoutMs = kConnectionCycleTeardownAckTimeoutMsDefault;
    return ctx;
}

void driveToSettling(const uint32_t connectedAtMs, const bool autoPushEnabled = true) {
    CycleContext ctx = makeContext(connectedAtMs);
    ctx.v1GattConnected = true;
    ctx.autoPushEnabled = autoPushEnabled;
    ctx.v1LastEventMs = connectedAtMs;
    module.update(ctx);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::V1_SETTLING),
                            static_cast<uint8_t>(module.state()));
}

void driveToObdScan(const uint32_t connectedAtMs, const bool autoPushEnabled = false) {
    driveToSettling(connectedAtMs, autoPushEnabled);

    CycleContext ctx = makeContext(connectedAtMs + 1600);
    ctx.v1GattConnected = true;
    ctx.autoPushEnabled = autoPushEnabled;
    ctx.v1LastEventMs = connectedAtMs;
    ctx.obdEnabled = true;
    module.update(ctx);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::OBD_SCAN),
                            static_cast<uint8_t>(module.state()));
}

void driveToProxyOpen(const uint32_t connectedAtMs) {
    driveToObdScan(connectedAtMs, false);

    CycleContext ctx = makeContext(connectedAtMs + 17001);
    ctx.v1GattConnected = true;
    ctx.v1LastEventMs = connectedAtMs;
    module.update(ctx);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::PROXY_OPEN),
                            static_cast<uint8_t>(module.state()));
}

}  // namespace

void setUp() {
    resetProviderState();
    module.begin(makeProviders());
}

void tearDown() {}

void test_initial_state_is_scan_v1() {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::SCAN_V1),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_EQUAL_UINT32(0, module.totalTransitionCount());
    TEST_ASSERT_EQUAL_UINT32(0, module.lastTeardownDurationMs());
    TEST_ASSERT_EQUAL_UINT32(0, module.totalObdRetryAttempts());
    TEST_ASSERT_EQUAL_UINT32(0, module.totalWifiManualPhoneKicks());
}

void test_v1_connect_transitions_to_v1_settling() {
    CycleContext ctx = makeContext(100);
    ctx.v1GattConnected = true;
    ctx.autoPushEnabled = true;
    ctx.v1LastEventMs = 100;

    module.update(ctx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::V1_SETTLING),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_EQUAL_UINT32(1, module.totalTransitionCount());
}

void test_verifypush_match_and_quiet_window_transition_to_obd_scan() {
    driveToSettling(100, true);

    CycleContext matchCtx = makeContext(200);
    matchCtx.v1GattConnected = true;
    matchCtx.autoPushEnabled = true;
    matchCtx.v1VerifyPushMatchEdge = true;
    matchCtx.v1LastEventMs = 100;
    module.update(matchCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::V1_SETTLING),
                            static_cast<uint8_t>(module.state()));

    CycleContext earlyQuietCtx = makeContext(699);
    earlyQuietCtx.v1GattConnected = true;
    earlyQuietCtx.autoPushEnabled = true;
    earlyQuietCtx.v1LastEventMs = 100;
    earlyQuietCtx.obdEnabled = true;
    module.update(earlyQuietCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::V1_SETTLING),
                            static_cast<uint8_t>(module.state()));

    CycleContext quietCtx = makeContext(700);
    quietCtx.v1GattConnected = true;
    quietCtx.autoPushEnabled = true;
    quietCtx.v1LastEventMs = 100;
    quietCtx.obdEnabled = true;
    module.update(quietCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::OBD_SCAN),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_TRUE(module.obdScanAllowed());
    TEST_ASSERT_TRUE(module.obdConnectAllowed());
}

void test_auto_push_disabled_fallback_transitions_to_obd_scan() {
    driveToSettling(100, false);

    CycleContext ctx = makeContext(1600);
    ctx.v1GattConnected = true;
    ctx.autoPushEnabled = false;
    ctx.v1LastEventMs = 100;
    ctx.obdEnabled = true;
    module.update(ctx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::OBD_SCAN),
                            static_cast<uint8_t>(module.state()));
}

void test_auto_push_enabled_without_verification_holds_obd_settle() {
    driveToSettling(100, true);

    CycleContext ctx = makeContext(1600);
    ctx.v1GattConnected = true;
    ctx.autoPushEnabled = true;
    ctx.v1LastEventMs = 100;
    ctx.obdEnabled = true;
    module.update(ctx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::V1_SETTLING),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_FALSE(module.obdScanAllowed());
    TEST_ASSERT_FALSE(module.proxyAdvertisingAllowed());
}

void test_obd_disabled_skips_to_proxy_open_after_settle() {
    driveToSettling(100, false);

    CycleContext ctx = makeContext(1600);
    ctx.v1GattConnected = true;
    ctx.autoPushEnabled = false;
    ctx.v1LastEventMs = 100;
    ctx.obdEnabled = false;
    module.update(ctx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::PROXY_OPEN),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_TRUE(module.proxyAdvertisingAllowed());
    TEST_ASSERT_TRUE(module.proxyKeepConnectionAllowed());
    TEST_ASSERT_EQUAL(0, providerState.stopObdScanCalls);
}

void test_explicit_proxy_mode_ignores_auto_push_for_app_window() {
    driveToSettling(100, true);

    CycleContext ctx = makeContext(1600);
    ctx.v1GattConnected = true;
    ctx.autoPushEnabled = true;
    ctx.v1LastEventMs = 100;
    ctx.obdEnabled = false;
    ctx.proxyEnabled = true;
    module.update(ctx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::PROXY_OPEN),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_TRUE(module.proxyAdvertisingAllowed());
    TEST_ASSERT_TRUE(module.proxyKeepConnectionAllowed());
}

void test_explicit_proxy_mode_keeps_advertising_available_for_late_app_launch() {
    driveToSettling(100, false);

    CycleContext openCtx = makeContext(1600);
    openCtx.v1GattConnected = true;
    openCtx.autoPushEnabled = false;
    openCtx.v1LastEventMs = 100;
    openCtx.obdEnabled = false;
    openCtx.proxyOpenWindowMs = kConnectionCycleProxyOpenWindowMsMin;
    module.update(openCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::PROXY_OPEN),
                            static_cast<uint8_t>(module.state()));

    CycleContext tooEarlyCtx = openCtx;
    tooEarlyCtx.nowMs = 1600 + kConnectionCycleProxyOpenWindowMsMin + 10;
    module.update(tooEarlyCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::PROXY_OPEN),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_TRUE(module.proxyAdvertisingAllowed());
    TEST_ASSERT_TRUE(module.proxyKeepConnectionAllowed());
    TEST_ASSERT_EQUAL(0, providerState.stopProxyAdvertisingCalls);

    CycleContext lateCtx = openCtx;
    lateCtx.nowMs = 1600 + kConnectionCycleProxyOpenWindowMsMin + 60000;
    module.update(lateCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::PROXY_OPEN),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_TRUE(module.proxyAdvertisingAllowed());
    TEST_ASSERT_TRUE(module.proxyKeepConnectionAllowed());
    TEST_ASSERT_EQUAL(0, providerState.stopProxyAdvertisingCalls);
}

void test_connected_proxy_app_can_drop_and_reopen_after_open_window() {
    driveToSettling(100, false);

    CycleContext openCtx = makeContext(1600);
    openCtx.v1GattConnected = true;
    openCtx.autoPushEnabled = false;
    openCtx.v1LastEventMs = 100;
    openCtx.obdEnabled = false;
    openCtx.proxyOpenWindowMs = kConnectionCycleProxyOpenWindowMsMin;
    module.update(openCtx);

    CycleContext connectedCtx = openCtx;
    connectedCtx.nowMs = 1600 + kConnectionCycleProxyOpenWindowMsMin + 60000;
    connectedCtx.proxyClientConnected = true;
    module.update(connectedCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::PROXY_OPEN),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_TRUE(module.proxyAdvertisingAllowed());
    TEST_ASSERT_TRUE(module.proxyKeepConnectionAllowed());
    TEST_ASSERT_EQUAL(0, providerState.stopProxyAdvertisingCalls);

    CycleContext disconnectedCtx = connectedCtx;
    disconnectedCtx.nowMs += 1;
    disconnectedCtx.proxyClientConnected = false;
    module.update(disconnectedCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::PROXY_OPEN),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_TRUE(module.proxyAdvertisingAllowed());
    TEST_ASSERT_TRUE(module.proxyKeepConnectionAllowed());
    TEST_ASSERT_EQUAL(0, providerState.stopProxyAdvertisingCalls);
}

void test_v1_disconnect_enters_teardown_from_active_state() {
    driveToObdScan(100, false);

    CycleContext ctx = makeContext(2000);
    ctx.v1GattConnected = false;
    module.update(ctx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::TEARDOWN),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_EQUAL(1, providerState.stopObdScanCalls);
    TEST_ASSERT_EQUAL(1, providerState.cancelObdConnectCalls);
}

void test_disconnect_during_settle_resets_cycle_after_teardown() {
    driveToSettling(100, true);

    CycleContext disconnectCtx = makeContext(300);
    disconnectCtx.v1GattConnected = false;
    disconnectCtx.v1LastEventMs = 300;
    module.update(disconnectCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::TEARDOWN),
                            static_cast<uint8_t>(module.state()));

    providerState.obdScanStopped = true;
    providerState.obdConnectIdle = true;
    CycleContext teardownStepCtx = makeContext(301);
    teardownStepCtx.v1GattConnected = false;
    teardownStepCtx.v1LastEventMs = 300;
    module.update(teardownStepCtx);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::TEARDOWN),
                            static_cast<uint8_t>(module.state()));

    providerState.proxyFullyStopped = true;
    CycleContext resetCtx = makeContext(302);
    resetCtx.v1GattConnected = false;
    resetCtx.v1LastEventMs = 300;
    module.update(resetCtx);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::SCAN_V1),
                            static_cast<uint8_t>(module.state()));
}

void test_obd_scan_timeout_transitions_to_proxy_open_and_stops_obd_scan() {
    driveToObdScan(100, false);

    CycleContext ctx = makeContext(17101);
    ctx.v1GattConnected = true;
    ctx.v1LastEventMs = 100;
    module.update(ctx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::PROXY_OPEN),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_EQUAL(1, providerState.stopObdScanCalls);
    TEST_ASSERT_TRUE(module.proxyAdvertisingAllowed());
    TEST_ASSERT_TRUE(module.proxyKeepConnectionAllowed());
}

void test_saved_obd_timeout_skips_proxy_window_when_proxy_unused_this_boot() {
    driveToObdScan(100, false);

    CycleContext ctx = makeContext(17101);
    ctx.v1GattConnected = true;
    ctx.v1LastEventMs = 100;
    ctx.obdEnabled = true;
    ctx.obdSavedAddressValid = true;
    module.update(ctx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::STEADY),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_EQUAL(1, providerState.stopObdScanCalls);
    TEST_ASSERT_FALSE(module.proxyAdvertisingAllowed());
    TEST_ASSERT_FALSE(module.proxyKeepConnectionAllowed());
}

void test_obd_settled_skips_passive_proxy_window_and_enters_steady() {
    driveToObdScan(100, false);

    CycleContext settledCtx = makeContext(1800);
    settledCtx.v1GattConnected = true;
    settledCtx.v1LastEventMs = 100;
    settledCtx.obdEnabled = true;
    settledCtx.obdSavedAddressValid = true;
    settledCtx.obdConnected = true;
    settledCtx.obdState = ObdConnectionState::POLLING;
    settledCtx.obdHasValidSpeedSample = true;
    module.update(settledCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::OBD_SETTLED),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_FALSE(module.proxyAdvertisingAllowed());
    TEST_ASSERT_FALSE(module.proxyKeepConnectionAllowed());

    CycleContext nextCtx = settledCtx;
    nextCtx.nowMs += 1;
    module.update(nextCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::STEADY),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_FALSE(module.proxyAdvertisingAllowed());
    TEST_ASSERT_FALSE(module.proxyKeepConnectionAllowed());
}

void test_saved_obd_timeout_keeps_proxy_window_after_proxy_seen_this_boot() {
    driveToObdScan(100, false);

    CycleContext ctx = makeContext(17101);
    ctx.v1GattConnected = true;
    ctx.v1LastEventMs = 100;
    ctx.obdSavedAddressValid = true;
    ctx.proxyClientConnectedOnceThisBoot = true;
    module.update(ctx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::PROXY_OPEN),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_EQUAL(1, providerState.stopObdScanCalls);
    TEST_ASSERT_TRUE(module.proxyAdvertisingAllowed());
    TEST_ASSERT_TRUE(module.proxyKeepConnectionAllowed());
}

void test_proxy_open_timeout_transitions_to_wifi_open_and_stops_proxy_advertising() {
    driveToObdScan(100, false);

    CycleContext obdTimeoutCtx = makeContext(17101);
    obdTimeoutCtx.v1GattConnected = true;
    obdTimeoutCtx.v1LastEventMs = 100;
    obdTimeoutCtx.obdEnabled = true;
    module.update(obdTimeoutCtx);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::PROXY_OPEN),
                            static_cast<uint8_t>(module.state()));

    CycleContext proxyTimeoutCtx = makeContext(77102);
    proxyTimeoutCtx.v1GattConnected = true;
    proxyTimeoutCtx.v1LastEventMs = 100;
    proxyTimeoutCtx.obdEnabled = true;
    module.update(proxyTimeoutCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::WIFI_OPEN),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_EQUAL(1, providerState.stopProxyAdvertisingCalls);
    TEST_ASSERT_FALSE(module.proxyAdvertisingAllowed());
    TEST_ASSERT_FALSE(module.proxyKeepConnectionAllowed());
    TEST_ASSERT_FALSE(module.wifiAutoStartAllowed());

    providerState.proxyFullyStopped = true;
    TEST_ASSERT_TRUE(module.wifiAutoStartAllowed());
}

void test_wifi_open_timeout_transitions_to_steady_with_custom_timeout() {
    driveToObdScan(100, false);

    CycleContext obdTimeoutCtx = makeContext(17101);
    obdTimeoutCtx.v1GattConnected = true;
    obdTimeoutCtx.v1LastEventMs = 100;
    obdTimeoutCtx.obdEnabled = true;
    module.update(obdTimeoutCtx);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::PROXY_OPEN),
                            static_cast<uint8_t>(module.state()));

    CycleContext proxyTimeoutCtx = makeContext(77102);
    proxyTimeoutCtx.v1GattConnected = true;
    proxyTimeoutCtx.v1LastEventMs = 100;
    proxyTimeoutCtx.obdEnabled = true;
    proxyTimeoutCtx.wifiOpenTimeoutMs = 2500;
    module.update(proxyTimeoutCtx);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::WIFI_OPEN),
                            static_cast<uint8_t>(module.state()));

    CycleContext earlyWifiCtx = makeContext(79601);
    earlyWifiCtx.v1GattConnected = true;
    earlyWifiCtx.v1LastEventMs = 100;
    earlyWifiCtx.obdEnabled = true;
    earlyWifiCtx.wifiOpenTimeoutMs = 2500;
    module.update(earlyWifiCtx);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::WIFI_OPEN),
                            static_cast<uint8_t>(module.state()));

    CycleContext timeoutWifiCtx = makeContext(79602);
    timeoutWifiCtx.v1GattConnected = true;
    timeoutWifiCtx.v1LastEventMs = 100;
    timeoutWifiCtx.obdEnabled = true;
    timeoutWifiCtx.wifiOpenTimeoutMs = 2500;
    module.update(timeoutWifiCtx);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::STEADY),
                            static_cast<uint8_t>(module.state()));
}

void test_manual_wifi_intent_from_proxy_open_disconnects_phone_and_transitions() {
    driveToProxyOpen(100);

    CycleContext manualWifiCtx = makeContext(18000);
    manualWifiCtx.v1GattConnected = true;
    manualWifiCtx.v1LastEventMs = 100;
    manualWifiCtx.proxyClientConnected = true;
    manualWifiCtx.wifiManualStartIntentLatched = true;
    module.update(manualWifiCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::WIFI_OPEN),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_TRUE(module.shouldPreemptProxyForManualWifiStart());
    TEST_ASSERT_EQUAL(1, providerState.stopProxyAdvertisingCalls);
    TEST_ASSERT_EQUAL(1, providerState.disconnectProxyPhoneCalls);
    TEST_ASSERT_EQUAL_UINT32(1, module.totalWifiManualPhoneKicks());
    TEST_ASSERT_FALSE(module.proxyAdvertisingAllowed());
    TEST_ASSERT_FALSE(module.proxyKeepConnectionAllowed());
    TEST_ASSERT_FALSE(module.wifiAutoStartAllowed());

    providerState.proxyFullyStopped = true;
    TEST_ASSERT_TRUE(module.wifiAutoStartAllowed());
}

void test_sequential_teardown_waits_for_acks_before_returning_to_scan_v1() {
    driveToObdScan(100, false);

    CycleContext dropCtx = makeContext(2000);
    dropCtx.v1GattConnected = false;
    module.update(dropCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::TEARDOWN),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_EQUAL(1, providerState.stopObdScanCalls);
    TEST_ASSERT_EQUAL(1, providerState.cancelObdConnectCalls);
    TEST_ASSERT_EQUAL(0, providerState.stopProxyAdvertisingCalls);

    CycleContext waitCtx = makeContext(2010);
    waitCtx.v1GattConnected = false;
    module.update(waitCtx);
    TEST_ASSERT_EQUAL(0, providerState.stopProxyAdvertisingCalls);

    providerState.obdScanStopped = true;
    providerState.obdConnectIdle = true;
    CycleContext obdAckCtx = makeContext(2020);
    obdAckCtx.v1GattConnected = false;
    module.update(obdAckCtx);
    TEST_ASSERT_EQUAL(1, providerState.stopProxyAdvertisingCalls);
    TEST_ASSERT_EQUAL(1, providerState.disconnectProxyPhoneCalls);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::TEARDOWN),
                            static_cast<uint8_t>(module.state()));

    providerState.proxyFullyStopped = true;
    CycleContext proxyAckCtx = makeContext(2030);
    proxyAckCtx.v1GattConnected = false;
    module.update(proxyAckCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::SCAN_V1),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_EQUAL_UINT32(30, module.lastTeardownDurationMs());
}

void test_record_obd_retry_attempt_resets_retry_interval() {
    driveToProxyOpen(100);

    TEST_ASSERT_TRUE(module.obdRetryAllowed(121700));

    module.recordObdRetryAttempt(121700);

    TEST_ASSERT_EQUAL_UINT32(1, module.totalObdRetryAttempts());
    TEST_ASSERT_FALSE(module.obdRetryAllowed(121701));
    TEST_ASSERT_FALSE(module.obdRetryAllowed(241699));
    TEST_ASSERT_TRUE(module.obdRetryAllowed(241700));
}

void test_custom_obd_scan_window_is_honored() {
    driveToObdScan(100, false);

    CycleContext earlyCtx = makeContext(4199);
    earlyCtx.v1GattConnected = true;
    earlyCtx.v1LastEventMs = 100;
    earlyCtx.obdScanWindowMs = 2500;
    module.update(earlyCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::OBD_SCAN),
                            static_cast<uint8_t>(module.state()));

    CycleContext timeoutCtx = makeContext(4200);
    timeoutCtx.v1GattConnected = true;
    timeoutCtx.v1LastEventMs = 100;
    timeoutCtx.obdScanWindowMs = 2500;
    module.update(timeoutCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::PROXY_OPEN),
                            static_cast<uint8_t>(module.state()));
}

void test_custom_obd_retry_interval_is_honored() {
    driveToProxyOpen(100);

    CycleContext configCtx = makeContext(18000);
    configCtx.v1GattConnected = true;
    configCtx.v1LastEventMs = 100;
    configCtx.obdRetryIntervalMs = 45000;
    module.update(configCtx);

    TEST_ASSERT_FALSE(module.obdRetryAllowed(46699));
    TEST_ASSERT_TRUE(module.obdRetryAllowed(46700));
}

void test_wifi_open_transitions_to_steady_when_wifi_active() {
    driveToObdScan(100, false);

    CycleContext obdTimeoutCtx = makeContext(17101);
    obdTimeoutCtx.v1GattConnected = true;
    obdTimeoutCtx.v1LastEventMs = 100;
    obdTimeoutCtx.obdEnabled = true;
    module.update(obdTimeoutCtx);

    CycleContext proxyTimeoutCtx = makeContext(77102);
    proxyTimeoutCtx.v1GattConnected = true;
    proxyTimeoutCtx.v1LastEventMs = 100;
    proxyTimeoutCtx.obdEnabled = true;
    module.update(proxyTimeoutCtx);

    CycleContext wifiActiveCtx = makeContext(77103);
    wifiActiveCtx.v1GattConnected = true;
    wifiActiveCtx.v1LastEventMs = 100;
    wifiActiveCtx.obdEnabled = true;
    wifiActiveCtx.wifiActive = true;
    providerState.proxyFullyStopped = true;
    module.update(wifiActiveCtx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::STEADY),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_FALSE(module.proxyAdvertisingAllowed());
    TEST_ASSERT_FALSE(module.proxyKeepConnectionAllowed());
    TEST_ASSERT_TRUE(module.wifiAutoStartAllowed());
}

// -- SCAN_V1 fallback / manual-intent tests --------------------------------

void test_scan_v1_manual_wifi_intent_transitions_to_wifi_open() {
    // No V1 present. Manual WiFi intent should skip directly to WIFI_OPEN.
    providerState.proxyFullyStopped = true;  // Proxy never started.
    CycleContext ctx = makeContext(5000);
    ctx.wifiManualStartIntentLatched = true;
    ctx.wifiEnabled = true;
    module.update(ctx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::WIFI_OPEN),
                            static_cast<uint8_t>(module.state()));
    TEST_ASSERT_TRUE(module.wifiAutoStartAllowed());
}

void test_scan_v1_timeout_transitions_to_steady_without_manual_intent() {
    // No V1, no manual intent. After 30s with bootReady, normal runtime stays WiFi-off.
    CycleContext ctx = makeContext(500);
    module.update(ctx);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::SCAN_V1),
                            static_cast<uint8_t>(module.state()));

    ctx = makeContext(30501);
    module.update(ctx);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::STEADY),
                            static_cast<uint8_t>(module.state()));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_initial_state_is_scan_v1);
    RUN_TEST(test_v1_connect_transitions_to_v1_settling);
    RUN_TEST(test_verifypush_match_and_quiet_window_transition_to_obd_scan);
    RUN_TEST(test_auto_push_disabled_fallback_transitions_to_obd_scan);
    RUN_TEST(test_auto_push_enabled_without_verification_holds_obd_settle);
    RUN_TEST(test_obd_disabled_skips_to_proxy_open_after_settle);
    RUN_TEST(test_explicit_proxy_mode_ignores_auto_push_for_app_window);
    RUN_TEST(test_explicit_proxy_mode_keeps_advertising_available_for_late_app_launch);
    RUN_TEST(test_connected_proxy_app_can_drop_and_reopen_after_open_window);
    RUN_TEST(test_v1_disconnect_enters_teardown_from_active_state);
    RUN_TEST(test_disconnect_during_settle_resets_cycle_after_teardown);
    RUN_TEST(test_obd_scan_timeout_transitions_to_proxy_open_and_stops_obd_scan);
    RUN_TEST(test_saved_obd_timeout_skips_proxy_window_when_proxy_unused_this_boot);
    RUN_TEST(test_obd_settled_skips_passive_proxy_window_and_enters_steady);
    RUN_TEST(test_saved_obd_timeout_keeps_proxy_window_after_proxy_seen_this_boot);
    RUN_TEST(test_proxy_open_timeout_transitions_to_wifi_open_and_stops_proxy_advertising);
    RUN_TEST(test_wifi_open_timeout_transitions_to_steady_with_custom_timeout);
    RUN_TEST(test_manual_wifi_intent_from_proxy_open_disconnects_phone_and_transitions);
    RUN_TEST(test_sequential_teardown_waits_for_acks_before_returning_to_scan_v1);
    RUN_TEST(test_record_obd_retry_attempt_resets_retry_interval);
    RUN_TEST(test_custom_obd_scan_window_is_honored);
    RUN_TEST(test_custom_obd_retry_interval_is_honored);
    RUN_TEST(test_wifi_open_transitions_to_steady_when_wifi_active);
    RUN_TEST(test_scan_v1_manual_wifi_intent_transitions_to_wifi_open);
    RUN_TEST(test_scan_v1_timeout_transitions_to_steady_without_manual_intent);
    return UNITY_END();
}
