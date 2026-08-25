#include <unity.h>

#include <string>

#include "../mocks/Arduino.h"
#include "../mocks/battery_manager.h"
#include "../mocks/display.h"
#include "../mocks/settings.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../include/battery_math.h"
#include "../../src/modules/power/power_module.cpp"
#include "../../src/modules/system/loop_connection_early_module.cpp"
#include "../../src/modules/system/loop_display_module.cpp"
#include "../../src/modules/system/loop_power_touch_module.cpp"

namespace {

BatteryManager battery;
V1Display display;
SettingsManager settings;
PowerModule power;

void setTime(unsigned long nowMs) {
    mockMillis = nowMs;
    mockMicros = nowMs * 1000UL;
}

struct PowerTouchProbe {
    bool suppressed = false;
    int powerCalls = 0;
    int touchCalls = 0;
};

struct ProbePowerLifecycle final : PowerLifecycle {
    int* preparationCalls = nullptr;
    int* abortCalls = nullptr;

    void prepareForShutdown() override {
        if (preparationCalls) {
            ++*preparationCalls;
        }
    }
    void resumeAfterAbortedShutdown() override {
        if (abortCalls) {
            ++*abortCalls;
        }
    }
};

void acquirePresentation(void* ctx, uint32_t) {
    auto* probe = static_cast<PowerTouchProbe*>(ctx);
    ++probe->powerCalls;
    probe->suppressed = true;
}

bool readPresentationSuppressed(void* ctx) {
    return static_cast<PowerTouchProbe*>(ctx)->suppressed;
}

bool runTouch(void* ctx, uint32_t, bool) {
    ++static_cast<PowerTouchProbe*>(ctx)->touchCalls;
    return true;
}

struct ConnectionEarlyProbe {
    int runtimeCalls = 0;
    int scanningCalls = 0;
    int displayEarlyCalls = 0;
};

ConnectionRuntimeSnapshot runConnectionRuntime(void* ctx, uint32_t, uint32_t, uint32_t, bool, uint32_t, bool) {
    auto* probe = static_cast<ConnectionEarlyProbe*>(ctx);
    ++probe->runtimeCalls;
    ConnectionRuntimeSnapshot snapshot;
    snapshot.requestShowInitialScanning = true;
    snapshot.connected = true;
    return snapshot;
}

void showInitialScanning(void* ctx) {
    ++static_cast<ConnectionEarlyProbe*>(ctx)->scanningCalls;
}

void runDisplayEarly(void* ctx, const DisplayOrchestrationEarlyContext&) {
    ++static_cast<ConnectionEarlyProbe*>(ctx)->displayEarlyCalls;
}

struct DisplayProbe {
    int collectCalls = 0;
    int parsedCalls = 0;
    int pipelineCalls = 0;
    int refreshCalls = 0;
    int blinkCalls = 0;
    int displayClockReads = 0;
    uint32_t pipelineNowMs = 0;
    uint32_t displayClockValue = 100;
    std::string order;
};

uint32_t readDisplayNow(void* ctx) {
    auto* probe = static_cast<DisplayProbe*>(ctx);
    probe->order.push_back('T');
    ++probe->displayClockReads;
    return probe->displayClockValue;
}

ParsedFrameSignal collectParsedSignal(void* ctx) {
    auto* probe = static_cast<DisplayProbe*>(ctx);
    ++probe->collectCalls;
    probe->order.push_back('S');
    ParsedFrameSignal signal;
    signal.parsedReady = true;
    return signal;
}

DisplayOrchestrationParsedResult runParsedFrame(void* ctx, const DisplayOrchestrationParsedContext&) {
    auto* probe = static_cast<DisplayProbe*>(ctx);
    ++probe->parsedCalls;
    probe->order.push_back('P');
    DisplayOrchestrationParsedResult result;
    result.runDisplayPipeline = true;
    return result;
}

void runDisplayPipeline(void* ctx, uint32_t nowMs) {
    auto* probe = static_cast<DisplayProbe*>(ctx);
    ++probe->pipelineCalls;
    probe->pipelineNowMs = nowMs;
    probe->order.push_back('D');
}

DisplayOrchestrationRefreshResult runRefresh(void* ctx, const DisplayOrchestrationRefreshContext&) {
    auto* probe = static_cast<DisplayProbe*>(ctx);
    ++probe->refreshCalls;
    probe->order.push_back('R');
    DisplayOrchestrationRefreshResult result;
    result.runBlinkRefresh = true;
    return result;
}

void runBlink(void* ctx, uint32_t) {
    ++static_cast<DisplayProbe*>(ctx)->blinkCalls;
}

} // namespace

void setUp() {
    setTime(0);
    battery.reset();
    battery.setOnBattery(true);
    battery.setHasBattery(true);
    battery.setBatteryPercent(60);
    display.reset();
    settings = SettingsManager{};
    settings.settings.autoPowerOffMinutes = 1;
    power = PowerModule{};
    power.begin(&battery, &display, &settings);
}

void tearDown() {}

void test_critical_protection_keeps_zero_invalid_but_accepts_deep_discharge() {
    TEST_ASSERT_TRUE(battery_math::criticalProtectionRequired(true, 3249));
    TEST_ASSERT_TRUE(battery_math::criticalProtectionRequired(true, 3200));
    TEST_ASSERT_TRUE(battery_math::criticalProtectionRequired(true, 3199));
    TEST_ASSERT_TRUE(battery_math::criticalProtectionRequired(true, 1));
    TEST_ASSERT_FALSE(battery_math::criticalProtectionRequired(true, 0));
    TEST_ASSERT_FALSE(battery_math::criticalProtectionRequired(false, 3199));
    TEST_ASSERT_FALSE(battery_math::criticalProtectionRequired(false, 1));
}

void test_critical_warning_does_not_depend_on_icon_presence_floor() {
    battery.setHasBattery(false);
    battery.setCritical(true);

    power.process(100);

    TEST_ASSERT_TRUE(power.ownsDisplayPresentation());
    TEST_ASSERT_EQUAL(1, display.showLowBatteryCalls);
    TEST_ASSERT_FALSE(power.consumeDisplayRestoreRequest());
}

void test_usb_transition_releases_warning_and_requests_one_authoritative_restore() {
    battery.setCritical(true);
    power.process(100);
    TEST_ASSERT_TRUE(power.ownsDisplayPresentation());

    battery.setOnBattery(false);
    power.process(200);

    TEST_ASSERT_FALSE(power.ownsDisplayPresentation());
    TEST_ASSERT_TRUE(power.consumeDisplayRestoreRequest());
    TEST_ASSERT_FALSE(power.consumeDisplayRestoreRequest());
}

void test_critical_shutdown_abort_releases_warning_for_authoritative_recovery() {
    int abortCalls = 0;
    ProbePowerLifecycle lifecycle;
    lifecycle.abortCalls = &abortCalls;
    power.setLifecycle(lifecycle);
    battery.setCritical(true);
    battery.powerOffResult = false;
    power.process(100);

    setTime(5101);
    power.process(5101);

    TEST_ASSERT_EQUAL(1, battery.refreshVoltageCalls);
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);
    TEST_ASSERT_EQUAL(1, abortCalls);
    TEST_ASSERT_FALSE(power.ownsDisplayPresentation());
    TEST_ASSERT_TRUE(power.consumeDisplayRestoreRequest());
    TEST_ASSERT_TRUE(power.consumeDisplayBrightnessRestoreRequest());
    TEST_ASSERT_EQUAL(0, display.setBrightnessCalls);
}

void test_critical_shutdown_is_cancelled_when_fresh_read_is_unavailable() {
    battery.setCritical(true);
    battery.setRefreshVoltageResult(false);
    power.process(100);

    power.process(5101);

    TEST_ASSERT_EQUAL(1, battery.refreshVoltageCalls);
    TEST_ASSERT_EQUAL(0, battery.powerOffCalls);
    TEST_ASSERT_FALSE(power.ownsDisplayPresentation());
    TEST_ASSERT_TRUE(power.consumeDisplayRestoreRequest());
}

void test_critical_shutdown_is_cancelled_when_fresh_read_recovers() {
    battery.setCritical(true);
    battery.setCriticalAfterRefresh(false);
    power.process(100);

    power.process(5101);

    TEST_ASSERT_EQUAL(1, battery.refreshVoltageCalls);
    TEST_ASSERT_EQUAL(0, battery.powerOffCalls);
    TEST_ASSERT_FALSE(power.ownsDisplayPresentation());
    TEST_ASSERT_TRUE(power.consumeDisplayRestoreRequest());
}

void test_critical_shutdown_reuses_successful_warning_window_read() {
    battery.setCritical(true);
    power.process(100);
    battery.setVoltageReadingAt(200);

    power.process(5101);

    TEST_ASSERT_EQUAL(0, battery.refreshVoltageCalls);
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);
}

void test_critical_shutdown_does_not_accept_pre_warning_read() {
    battery.setCritical(true);
    battery.setVoltageReadingAt(99);
    battery.setRefreshVoltageResult(false);
    power.process(100);

    power.process(5101);

    TEST_ASSERT_EQUAL(1, battery.refreshVoltageCalls);
    TEST_ASSERT_EQUAL(0, battery.powerOffCalls);
    TEST_ASSERT_FALSE(power.ownsDisplayPresentation());
}

void test_critical_shutdown_does_not_treat_trigger_sample_as_confirmation() {
    battery.setCritical(true);
    battery.setVoltageReadingAt(100);
    battery.setRefreshVoltageResult(false);
    power.process(100);

    power.process(5101);

    TEST_ASSERT_EQUAL(1, battery.refreshVoltageCalls);
    TEST_ASSERT_EQUAL(0, battery.powerOffCalls);
}

void test_auto_power_abort_retries_only_after_a_full_interval() {
    power.onV1DataReceived();
    setTime(1000);
    power.onV1ConnectionChange(false);
    TEST_ASSERT_EQUAL_UINT32(1000, power.autoPowerOffTimerStartForTest());
    battery.powerOffResult = false;

    setTime(61000);
    power.process(61000);
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);
    TEST_ASSERT_EQUAL_UINT32(61000, power.autoPowerOffTimerStartForTest());

    setTime(120999);
    power.process(120999);
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);

    setTime(121000);
    power.process(121000);
    TEST_ASSERT_EQUAL(2, battery.powerOffCalls);
    TEST_ASSERT_EQUAL_UINT32(121000, power.autoPowerOffTimerStartForTest());
}

void test_shutdown_preparation_cannot_veto_physical_poweroff() {
    int preparationCalls = 0;
    int abortCalls = 0;
    ProbePowerLifecycle lifecycle;
    lifecycle.preparationCalls = &preparationCalls;
    lifecycle.abortCalls = &abortCalls;
    power.setLifecycle(lifecycle);

    power.performShutdown();

    TEST_ASSERT_EQUAL(1, preparationCalls);
    TEST_ASSERT_EQUAL(0, abortCalls);
    TEST_ASSERT_EQUAL(1, display.showShutdownCalls);
    TEST_ASSERT_EQUAL(1, battery.powerOffCalls);
}

void test_power_touch_phase_suppresses_touch_after_warning_acquires_owner() {
    PowerTouchProbe probe;
    LoopPowerTouchModule module;
    LoopPowerTouchModule::Providers providers;
    providers.runPowerProcess = acquirePresentation;
    providers.powerContext = &probe;
    providers.readPresentationSuppressed = readPresentationSuppressed;
    providers.presentationContext = &probe;
    providers.runTouchUiProcess = runTouch;
    providers.touchUiContext = &probe;
    module.begin(providers);

    const LoopPowerTouchResult result = module.process(LoopPowerTouchContext{});

    TEST_ASSERT_EQUAL(1, probe.powerCalls);
    TEST_ASSERT_EQUAL(0, probe.touchCalls);
    TEST_ASSERT_TRUE(result.presentationSuppressed);
    TEST_ASSERT_FALSE(result.inSettings);
}

void test_connection_early_keeps_runtime_live_but_suppresses_presentations() {
    ConnectionEarlyProbe probe;
    LoopConnectionEarlyModule module;
    LoopConnectionEarlyModule::Providers providers;
    providers.runConnectionRuntime = runConnectionRuntime;
    providers.connectionRuntimeContext = &probe;
    providers.showInitialScanning = showInitialScanning;
    providers.scanningContext = &probe;
    providers.runDisplayEarly = runDisplayEarly;
    providers.displayEarlyContext = &probe;
    module.begin(providers);

    LoopConnectionEarlyContext ctx;
    ctx.presentationSuppressed = true;
    const LoopConnectionEarlyResult result = module.process(ctx);

    TEST_ASSERT_TRUE(result.bleConnectedNow);
    TEST_ASSERT_TRUE(result.initialScanningScreenShown);
    TEST_ASSERT_EQUAL(1, probe.runtimeCalls);
    TEST_ASSERT_EQUAL(0, probe.scanningCalls);
    TEST_ASSERT_EQUAL(0, probe.displayEarlyCalls);
}

void test_display_phase_consumes_event_but_suppresses_pipeline_and_blink() {
    DisplayProbe probe;
    LoopDisplayModule module;
    LoopDisplayModule::Providers providers;
    providers.readDisplayNowMs = readDisplayNow;
    providers.displayNowContext = &probe;
    providers.collectParsedSignal = collectParsedSignal;
    providers.parsedSignalContext = &probe;
    providers.runParsedFrame = runParsedFrame;
    providers.parsedFrameContext = &probe;
    providers.runDisplayPipeline = runDisplayPipeline;
    providers.displayPipelineContext = &probe;
    providers.runLightweightRefresh = runRefresh;
    providers.lightweightRefreshContext = &probe;
    providers.runBlinkRefresh = runBlink;
    providers.blinkRefreshContext = &probe;
    module.begin(providers);

    LoopDisplayContext ctx;
    ctx.presentationSuppressed = true;
    module.process(ctx);

    TEST_ASSERT_EQUAL(1, probe.collectCalls);
    TEST_ASSERT_EQUAL(0, probe.parsedCalls);
    TEST_ASSERT_EQUAL(0, probe.pipelineCalls);
    TEST_ASSERT_EQUAL(0, probe.refreshCalls);
    TEST_ASSERT_EQUAL(0, probe.blinkCalls);
    TEST_ASSERT_EQUAL_STRING("TS", probe.order.c_str());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_critical_protection_keeps_zero_invalid_but_accepts_deep_discharge);
    RUN_TEST(test_critical_warning_does_not_depend_on_icon_presence_floor);
    RUN_TEST(test_usb_transition_releases_warning_and_requests_one_authoritative_restore);
    RUN_TEST(test_critical_shutdown_abort_releases_warning_for_authoritative_recovery);
    RUN_TEST(test_critical_shutdown_is_cancelled_when_fresh_read_is_unavailable);
    RUN_TEST(test_critical_shutdown_is_cancelled_when_fresh_read_recovers);
    RUN_TEST(test_critical_shutdown_reuses_successful_warning_window_read);
    RUN_TEST(test_critical_shutdown_does_not_accept_pre_warning_read);
    RUN_TEST(test_critical_shutdown_does_not_treat_trigger_sample_as_confirmation);
    RUN_TEST(test_auto_power_abort_retries_only_after_a_full_interval);
    RUN_TEST(test_shutdown_preparation_cannot_veto_physical_poweroff);
    RUN_TEST(test_power_touch_phase_suppresses_touch_after_warning_acquires_owner);
    RUN_TEST(test_connection_early_keeps_runtime_live_but_suppresses_presentations);
    RUN_TEST(test_display_phase_consumes_event_but_suppresses_pipeline_and_blink);
    return UNITY_END();
}
