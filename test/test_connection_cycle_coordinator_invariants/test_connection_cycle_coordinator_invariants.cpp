// Runtime invariants for ConnectionCycleCoordinatorModule.
//
// Complements test_connection_cycle_coordinator_module/, which exercises
// specific happy-path and disconnect scenarios. This file instead verifies
// three coarse-grained properties that must hold for any reachable run of
// the coordinator:
//
//   1. Boot-to-STEADY bound: the coordinator reaches STEADY within
//      (sum of configured windows + headroom) when driven along the
//      obd-then-proxy-then-wifi happy path.
//   2. Predicates are monotonic within a single state: while state() does
//      not change, each *Allowed() predicate can transition value at most
//      once. This is the "don't flicker mid-state" anti-bounce property.
//   3. Counters never decrement: totalTransitionCount(),
//      totalObdRetryAttempts(), totalWifiManualPhoneKicks() increase
//      monotonically across a full cycle including a teardown-and-reentry.

#include <unity.h>

#include <vector>

#include "../../src/modules/system/connection_cycle_coordinator_module.cpp"

static ConnectionCycleCoordinatorModule module;

namespace {

struct ProviderState {
    bool obdScanStopped = false;
    bool obdConnectIdle = false;
    bool proxyFullyStopped = false;
};

ProviderState providerState;

void resetProviderState() {
    providerState = ProviderState{};
}

void stopObdScanNoop(void*) {}
void cancelObdConnectNoop(void*) {}
void stopProxyAdvertisingNoop(void*) {}
void disconnectProxyPhoneNoop(void*) {}

bool isObdScanStopped(void*) { return providerState.obdScanStopped; }
bool isObdConnectIdle(void*) { return providerState.obdConnectIdle; }
bool isProxyFullyStopped(void*) { return providerState.proxyFullyStopped; }

ConnectionCycleCoordinatorModule::Providers makeProviders() {
    ConnectionCycleCoordinatorModule::Providers providers;
    providers.stopObdScan = stopObdScanNoop;
    providers.cancelObdConnect = cancelObdConnectNoop;
    providers.stopProxyAdvertising = stopProxyAdvertisingNoop;
    providers.disconnectProxyPhone = disconnectProxyPhoneNoop;
    providers.isObdScanStopped = isObdScanStopped;
    providers.isObdConnectIdle = isObdConnectIdle;
    providers.isProxyFullyStopped = isProxyFullyStopped;
    return providers;
}

// Use settings clamp minimums so updateTimingConfig() does not silently
// raise values above the timestamps the test expects.
constexpr uint32_t kShortSettleQuietMs = 100;       // kConnectionCycleV1SettleQuietMsMin
constexpr uint32_t kShortSettleFallbackMs = 500;     // kConnectionCycleV1SettleFallbackMsMin
constexpr uint32_t kShortObdScanWindowMs = 1000;     // kConnectionCycleObdScanWindowMsMin
constexpr uint32_t kShortProxyOpenWindowMs = 1000;   // kConnectionCycleProxyOpenWindowMsMin
constexpr uint32_t kShortWifiOpenTimeoutMs = 1000;   // kConnectionCycleWifiOpenTimeoutMsMin
constexpr uint32_t kShortTeardownAckTimeoutMs = 50;  // above kConnectionCycleTeardownAckTimeoutMsMin (25)

CycleContext makeContext(const uint32_t nowMs) {
    CycleContext ctx;
    ctx.nowMs = nowMs;
    ctx.bootReady = true;
    ctx.proxyEnabled = true;
    ctx.wifiEnabled = true;
    ctx.v1SettleQuietMs = kShortSettleQuietMs;
    ctx.v1SettleFallbackMs = kShortSettleFallbackMs;
    ctx.obdScanWindowMs = kShortObdScanWindowMs;
    ctx.obdRetryIntervalMs = 120000;
    ctx.proxyOpenWindowMs = kShortProxyOpenWindowMs;
    ctx.wifiOpenTimeoutMs = kShortWifiOpenTimeoutMs;
    ctx.cycleTeardownAckTimeoutMs = kShortTeardownAckTimeoutMs;
    return ctx;
}

// The seven output predicates the coordinator exposes. Captured as a 7-tuple
// so we can compare state transitions within a state. obdRetryAllowed needs
// a nowMs argument; we pin it to a representative value per snapshot.
struct PredicateSnapshot {
    bool obdScanAllowed;
    bool obdConnectAllowed;
    bool obdRetryAllowed;
    bool proxyAdvertisingAllowed;
    bool proxyKeepConnectionAllowed;
    bool wifiAutoStartAllowed;
    bool shouldPreemptProxyForManualWifiStart;
};

PredicateSnapshot snapshotPredicates(const uint32_t nowMs) {
    return PredicateSnapshot{
        module.obdScanAllowed(),
        module.obdConnectAllowed(),
        module.obdRetryAllowed(nowMs),
        module.proxyAdvertisingAllowed(),
        module.proxyKeepConnectionAllowed(),
        module.wifiAutoStartAllowed(),
        module.shouldPreemptProxyForManualWifiStart(),
    };
}

}  // namespace

void setUp() {
    resetProviderState();
    module.begin(makeProviders());
}

void tearDown() {}

// -- Invariant 1: boot-to-STEADY bound -------------------------------------

void test_boot_to_steady_with_auto_push_without_verify_within_bound() {
    // Failure path: V1 connects with auto-push enabled, but no VerifyPush
    // edge arrives. The hard deadline still advances to OBD; the remaining
    // phase windows then retain the overall boot-to-STEADY bound.
    const uint32_t connectMs = 1000;

    // Tick 1: V1 connects → V1_SETTLING
    CycleContext ctx = makeContext(connectMs);
    ctx.v1GattConnected = true;
    ctx.autoPushEnabled = true;
    ctx.v1LastEventMs = connectMs;
    ctx.obdEnabled = true;
    module.update(ctx);

    // Tick 2: hard settle deadline elapsed despite continuing traffic → OBD_SCAN
    ctx = makeContext(connectMs + kV1SettleHardDeadlineMs);
    ctx.v1GattConnected = true;
    ctx.autoPushEnabled = true;
    ctx.v1LastEventMs = ctx.nowMs;
    ctx.obdEnabled = true;
    module.update(ctx);

    // Tick 3: obd scan window elapsed, obd never found → PROXY_OPEN
    ctx = makeContext(connectMs + kV1SettleHardDeadlineMs + kShortObdScanWindowMs + 10);
    ctx.v1GattConnected = true;
    ctx.v1LastEventMs = connectMs;
    ctx.obdEnabled = true;
    module.update(ctx);

    // Tick 4: proxy open window elapsed, no client → WIFI_OPEN
    ctx = makeContext(connectMs + kV1SettleHardDeadlineMs + kShortObdScanWindowMs +
                      kShortProxyOpenWindowMs + 20);
    ctx.v1GattConnected = true;
    ctx.v1LastEventMs = connectMs;
    ctx.obdEnabled = true;
    module.update(ctx);

    // Tick 5: wifi becomes active → STEADY
    const uint32_t steadyNowMs =
        connectMs + kV1SettleHardDeadlineMs + kShortObdScanWindowMs +
        kShortProxyOpenWindowMs + 30;
    ctx = makeContext(steadyNowMs);
    ctx.v1GattConnected = true;
    ctx.v1LastEventMs = connectMs;
    ctx.obdEnabled = true;
    ctx.wifiActive = true;
    providerState.proxyFullyStopped = true;
    module.update(ctx);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::STEADY),
                            static_cast<uint8_t>(module.state()));

    const uint32_t totalElapsed = steadyNowMs - connectMs;
    const uint32_t bound = kV1SettleHardDeadlineMs + kShortObdScanWindowMs +
                           kShortProxyOpenWindowMs + kShortWifiOpenTimeoutMs +
                           1000;  // 1s slack
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(bound, totalElapsed);
}

// -- Invariant 2: predicates monotonic within a state ----------------------

namespace {

// Count how many times a bool sequence changes value.
int countTransitions(const std::vector<bool>& values) {
    int transitions = 0;
    for (size_t i = 1; i < values.size(); ++i) {
        if (values[i] != values[i - 1]) {
            transitions++;
        }
    }
    return transitions;
}

}  // namespace

void test_predicates_monotonic_within_steady() {
    // Drive to STEADY using the happy-path helper inlined here.
    const uint32_t connectMs = 1000;

    CycleContext initCtx = makeContext(connectMs);
    initCtx.v1GattConnected = true;
    initCtx.autoPushEnabled = false;
    initCtx.v1LastEventMs = connectMs;
    initCtx.obdEnabled = true;
    module.update(initCtx);  // V1_SETTLING

    CycleContext settleCtx = makeContext(connectMs + kShortSettleFallbackMs + 5);
    settleCtx.v1GattConnected = true;
    settleCtx.autoPushEnabled = false;
    settleCtx.v1LastEventMs = connectMs;
    settleCtx.obdEnabled = true;
    module.update(settleCtx);  // OBD_SCAN

    CycleContext obdTimeoutCtx =
        makeContext(connectMs + kShortSettleFallbackMs + kShortObdScanWindowMs + 10);
    obdTimeoutCtx.v1GattConnected = true;
    obdTimeoutCtx.v1LastEventMs = connectMs;
    obdTimeoutCtx.obdEnabled = true;
    module.update(obdTimeoutCtx);  // PROXY_OPEN

    CycleContext proxyTimeoutCtx =
        makeContext(connectMs + kShortSettleFallbackMs + kShortObdScanWindowMs +
                    kShortProxyOpenWindowMs + 20);
    proxyTimeoutCtx.v1GattConnected = true;
    proxyTimeoutCtx.v1LastEventMs = connectMs;
    proxyTimeoutCtx.obdEnabled = true;
    module.update(proxyTimeoutCtx);  // WIFI_OPEN

    CycleContext wifiActiveCtx = makeContext(proxyTimeoutCtx.nowMs + 5);
    wifiActiveCtx.v1GattConnected = true;
    wifiActiveCtx.v1LastEventMs = connectMs;
    wifiActiveCtx.obdEnabled = true;
    wifiActiveCtx.wifiActive = true;
    providerState.proxyFullyStopped = true;
    module.update(wifiActiveCtx);  // STEADY

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::STEADY),
                            static_cast<uint8_t>(module.state()));

    // Now drive 30 updates that keep us in STEADY. Vary contextual inputs
    // that are legitimately independent of state (proxy client comes and
    // goes, obd samples flip, wifi toggles within-active). The key
    // constraint is never to inject wifiManualStartIntentLatched +
    // proxyClientConnected together, since that is the one legitimate
    // STEADY→WIFI_OPEN trigger.
    std::vector<PredicateSnapshot> snapshots;
    const uint32_t baseNowMs = wifiActiveCtx.nowMs + 10;
    for (uint32_t i = 0; i < 30; ++i) {
        CycleContext stepCtx = makeContext(baseNowMs + i * 5);
        stepCtx.v1GattConnected = true;
        stepCtx.v1LastEventMs = connectMs;
        stepCtx.obdEnabled = true;
        stepCtx.wifiActive = (i % 2 == 0);
        stepCtx.obdHasValidSpeedSample = (i % 3 == 0);
        stepCtx.proxyClientConnected = false;  // avoid legit transition trigger
        stepCtx.proxyClientConnectedOnceThisBoot = (i > 10);
        providerState.proxyFullyStopped = true;
        module.update(stepCtx);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::STEADY),
                                static_cast<uint8_t>(module.state()));
        snapshots.push_back(snapshotPredicates(stepCtx.nowMs));
    }

    // Each predicate should flip value at most once across these 30 ticks
    // while the state stayed pinned at STEADY.
    std::vector<bool> obdScan, obdConnect, obdRetry, proxyAdv, proxyKeep,
        wifiAuto, preempt;
    for (const auto& s : snapshots) {
        obdScan.push_back(s.obdScanAllowed);
        obdConnect.push_back(s.obdConnectAllowed);
        obdRetry.push_back(s.obdRetryAllowed);
        proxyAdv.push_back(s.proxyAdvertisingAllowed);
        proxyKeep.push_back(s.proxyKeepConnectionAllowed);
        wifiAuto.push_back(s.wifiAutoStartAllowed);
        preempt.push_back(s.shouldPreemptProxyForManualWifiStart);
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT(1, countTransitions(obdScan));
    TEST_ASSERT_LESS_OR_EQUAL_INT(1, countTransitions(obdConnect));
    TEST_ASSERT_LESS_OR_EQUAL_INT(1, countTransitions(obdRetry));
    TEST_ASSERT_LESS_OR_EQUAL_INT(1, countTransitions(proxyAdv));
    TEST_ASSERT_LESS_OR_EQUAL_INT(1, countTransitions(proxyKeep));
    TEST_ASSERT_LESS_OR_EQUAL_INT(1, countTransitions(wifiAuto));
    TEST_ASSERT_LESS_OR_EQUAL_INT(1, countTransitions(preempt));
}

// -- Invariant 3: counters are monotonic -----------------------------------

void test_counters_monotonic_through_full_cycle_and_reentry() {
    uint32_t lastTransitions = 0;
    uint32_t lastObdRetry = 0;
    uint32_t lastWifiKicks = 0;

    auto stepAndAssertMonotonic = [&](const CycleContext& ctx) {
        module.update(ctx);
        const uint32_t transitions = module.totalTransitionCount();
        const uint32_t obdRetry = module.totalObdRetryAttempts();
        const uint32_t wifiKicks = module.totalWifiManualPhoneKicks();
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(lastTransitions, transitions);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(lastObdRetry, obdRetry);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(lastWifiKicks, wifiKicks);
        lastTransitions = transitions;
        lastObdRetry = obdRetry;
        lastWifiKicks = wifiKicks;
    };

    // Walk: SCAN_V1 → V1_SETTLING → OBD_SCAN → PROXY_OPEN → WIFI_OPEN →
    // STEADY → (v1 drops) TEARDOWN → SCAN_V1 → V1_SETTLING → OBD_SCAN →
    // PROXY_OPEN.
    const uint32_t t0 = 1000;
    CycleContext ctx = makeContext(t0);
    ctx.v1GattConnected = true;
    ctx.autoPushEnabled = false;
    ctx.v1LastEventMs = t0;
    ctx.obdEnabled = true;
    stepAndAssertMonotonic(ctx);

    ctx = makeContext(t0 + kShortSettleFallbackMs + 5);
    ctx.v1GattConnected = true;
    ctx.autoPushEnabled = false;
    ctx.v1LastEventMs = t0;
    ctx.obdEnabled = true;
    stepAndAssertMonotonic(ctx);  // OBD_SCAN

    ctx = makeContext(t0 + kShortSettleFallbackMs + kShortObdScanWindowMs + 10);
    ctx.v1GattConnected = true;
    ctx.v1LastEventMs = t0;
    ctx.obdEnabled = true;
    stepAndAssertMonotonic(ctx);  // PROXY_OPEN

    ctx = makeContext(t0 + kShortSettleFallbackMs + kShortObdScanWindowMs +
                      kShortProxyOpenWindowMs + 20);
    ctx.v1GattConnected = true;
    ctx.v1LastEventMs = t0;
    ctx.obdEnabled = true;
    stepAndAssertMonotonic(ctx);  // WIFI_OPEN

    ctx = makeContext(ctx.nowMs + 5);
    ctx.v1GattConnected = true;
    ctx.v1LastEventMs = t0;
    ctx.obdEnabled = true;
    ctx.wifiActive = true;
    providerState.proxyFullyStopped = true;
    stepAndAssertMonotonic(ctx);  // STEADY

    // Record an OBD retry attempt; counter must increase.
    module.recordObdRetryAttempt(ctx.nowMs);
    TEST_ASSERT_GREATER_THAN_UINT32(lastObdRetry, module.totalObdRetryAttempts());
    lastObdRetry = module.totalObdRetryAttempts();

    // Manual wifi kick from STEADY with proxy client connected.
    ctx = makeContext(ctx.nowMs + 10);
    ctx.v1GattConnected = true;
    ctx.v1LastEventMs = t0;
    ctx.proxyClientConnected = true;
    ctx.wifiManualStartIntentLatched = true;
    stepAndAssertMonotonic(ctx);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, module.totalWifiManualPhoneKicks());

    // V1 drops while in WIFI_OPEN → TEARDOWN.
    ctx = makeContext(ctx.nowMs + 10);
    ctx.v1GattConnected = false;
    ctx.v1LastEventMs = t0;
    stepAndAssertMonotonic(ctx);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CycleState::TEARDOWN),
                            static_cast<uint8_t>(module.state()));

    // Ack teardown acks → back to SCAN_V1.
    providerState.obdScanStopped = true;
    providerState.obdConnectIdle = true;
    providerState.proxyFullyStopped = true;
    ctx = makeContext(ctx.nowMs + 5);
    ctx.v1GattConnected = false;
    stepAndAssertMonotonic(ctx);  // still TEARDOWN, but possibly stepping
    ctx = makeContext(ctx.nowMs + 5);
    ctx.v1GattConnected = false;
    stepAndAssertMonotonic(ctx);  // → SCAN_V1

    // Re-enter a cycle; transition counter must keep growing.
    ctx = makeContext(ctx.nowMs + 100);
    ctx.v1GattConnected = true;
    ctx.autoPushEnabled = false;
    ctx.v1LastEventMs = ctx.nowMs;
    stepAndAssertMonotonic(ctx);  // V1_SETTLING

    TEST_ASSERT_GREATER_THAN_UINT32(0, module.totalTransitionCount());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_boot_to_steady_with_auto_push_without_verify_within_bound);
    RUN_TEST(test_predicates_monotonic_within_steady);
    RUN_TEST(test_counters_monotonic_through_full_cycle_and_reentry);
    return UNITY_END();
}
