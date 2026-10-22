
#include "test/mocks/Arduino.h"
#include "test/mocks/battery_manager.h"
#include "test/mocks/display.h"
#include "test/mocks/settings.h"
#include <cstdio>
#include <initializer_list>
#include <unity.h>
SerialClass Serial;
unsigned long mockMillis = 0, mockMicros = 0;
#include "include/main_runtime_state.h"
#include "src/modules/alp/alp_runtime_module.cpp"
#include "src/modules/power/power_module.cpp"
#include "src/modules/touch/touch_ui_module.cpp"
#include "src/runtime_coordinator.h"
#include "src/touch_handler.cpp"
void audio_set_volume(uint8_t) {}
void play_test_voice() {}
void setUp() {
    mockMillis = 0;
    mockMicros = 0;
}
void tearDown() {}

struct Connection {
    bool connected = false, backpressured = false, overloaded = false,
         requestShowInitialScanning = false;
};
struct Preview {
    bool isRunning() const { return false; }
    void cancel() {}
};
struct Edge {};
class DriveRuntime {
  public:
    BatteryManager battery_;
    V1Display display_;
    SettingsManager settings_;
    PowerModule power_;
    AlpRuntimeModule alp_;
    MainRuntimeState state_;
    Preview preview_;
    bool boot = false;
    uint32_t presentationElapsedMs = 0;
    TouchHandler touch_;
    TouchUiModule touchUi_;
    DriveRuntime() {
        battery_.setOnBattery(true);
        battery_.setBatteryPercent(90);
        settings_.settings.autoPowerOffMinutes = 1;
        power_.begin(&battery_, &display_, &settings_);
        alp_.begin(true);
        touch_.begin();
        touchUi_.begin(&display_, &touch_, &settings_, {});
    }
    bool active() { return true; }
    DriveLoopTiming beginDriveLoop() {
        return {static_cast<uint32_t>(mockMicros),
                static_cast<uint32_t>(mockMillis)};
    }
    Connection processConnectionRuntime(uint32_t) { return {}; }
    void acceptConnectionSnapshot(const Connection &) {}
    bool powerOwnsPresentation() { return power_.ownsDisplayPresentation(); }
    void showInitialScanningScreen() {}
    void markInitialScanningScreenHandled() {}
    void presentConnectionState(const Connection &) {
        mockMillis = static_cast<uint32_t>(mockMillis + presentationElapsedMs);
    }
    void processPower(uint32_t);
    void processAlp(uint32_t);
    void processAlpPresentationAndPower(uint32_t);
    bool processTouch(uint32_t now) { return touchUi_.process(now, boot); }
    void servicePowerDisplayOwnership(uint32_t) {}
    bool preemptSettingsForLiveAlert() {
        return alp_.ownsLaserDisplay() && alp_.currentEvent().active;
    }
    void openBootReadyGate(uint32_t) {}
    void processBleRuntime() {}
    void processGps(uint32_t) {}
    void processSpeed(uint32_t) {}
    void processSpeedAlert(uint32_t) {}
    void observeAlpProductState(uint32_t) {}
    void processPeriodicMaintenance(uint32_t, bool, bool, bool, bool) {}
    void finishDriveLoop(bool, uint32_t, bool) {}
    void processTapGesture(uint32_t) {}
    void processBleQueue() {}
    bool bleQueueBackpressured() { return false; }
    void processConnectionCycle(uint32_t, bool) {}
    void processObd(uint32_t, bool) {}
    bool hasLiveAlertPresentation() {
        return alp_.ownsLaserDisplay() && alp_.currentEvent().active;
    }
    void servicePowerAlertPresentation(uint32_t, bool) {}
    bool powerAlertPresentationNeedsRestore() { return false; }
    Edge consumeDisplayEdges() { return {}; }
    bool processParsedPolicies(const Edge &) { return false; }
    void presentDisplay(const Edge &, bool) {}
    DriveLoopDispatch processConnectionDispatch(bool) {
        return {static_cast<uint32_t>(mockMillis), false};
    }
    void tick(uint32_t now, bool heartbeat = false) {
        mockMillis = now;
        mockMicros = now * 1000u;
        if (heartbeat) {
            const uint8_t bytes[] = {0xB0, 0x03, 0, 0x33};
            alp_.testInjectBytes(bytes, sizeof(bytes));
            alp_.testSetLastUartByteMs(now);
        }
        DriveLoopCoordinator::tick(*this);
    }
};
#include "alp_power.inc"

void exercise_alp_presence(bool sliders, uint32_t heartbeatAt, uint32_t phaseElapsedMs = 0) {
    DriveRuntime r;
    r.power_.onV1DataReceived();
    mockMillis = 1000;
    r.power_.onV1ConnectionChange(false);
    if (sliders) {
        r.boot = true;
        r.tick(2000);
        r.boot = false;
        r.tick(2400);
        TEST_ASSERT_EQUAL_INT(1, r.display_.showSettingsSlidersCalls);
    }
    r.tick(heartbeatAt, true);
    TEST_ASSERT_TRUE(r.state_.alpSignalActive);
    TEST_ASSERT_FALSE(r.power_.autoPowerOffTimerRunningForTest());
    TEST_ASSERT_EQUAL_INT(0, r.battery_.powerOffCalls);
    if (sliders)
        TEST_ASSERT_EQUAL_INT(0, r.display_.hideBrightnessSliderCalls);
    const uint32_t silenceAt = heartbeatAt + 4001;
    r.presentationElapsedMs = phaseElapsedMs;
    r.tick(silenceAt);
    r.presentationElapsedMs = 0;
    const uint32_t timerStartedAt = silenceAt + phaseElapsedMs;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.battery_.powerOffCalls,
                                 "ALP loss must start a full interval, even after same-pass work");
    TEST_ASSERT_FALSE(r.state_.alpSignalActive);
    TEST_ASSERT_TRUE(r.power_.autoPowerOffTimerRunningForTest());
    TEST_ASSERT_EQUAL_UINT32(timerStartedAt,
                             r.power_.autoPowerOffTimerStartForTest());
    r.tick(timerStartedAt + 59999);
    TEST_ASSERT_EQUAL_INT(0, r.battery_.powerOffCalls);
    r.tick(timerStartedAt + 60000);
    TEST_ASSERT_EQUAL_INT(1, r.battery_.powerOffCalls);
}
void test_normal_heartbeat_and_silence_reconcile_power() {
    exercise_alp_presence(false, 59000);
}
void test_slider_heartbeat_and_silence_reconcile_power() {
    exercise_alp_presence(true, 59000);
}
void test_normal_heartbeat_at_expiry_cancels_before_shutdown() {
    exercise_alp_presence(false, 61000);
}
void test_slider_heartbeat_at_expiry_cancels_before_shutdown() {
    exercise_alp_presence(true, 61000);
}
void test_normal_alp_loss_after_phase_work_waits_full_interval() {
    exercise_alp_presence(false, 59000, 1);
}
void test_slider_alp_loss_after_phase_work_waits_full_interval() {
    exercise_alp_presence(true, 59000, 1);
}
void test_normal_alp_loss_at_clock_wrap_waits_full_interval() {
    exercise_alp_presence(false, UINT32_MAX - 4001, 1);
}
void test_slider_alp_loss_at_clock_wrap_waits_full_interval() {
    exercise_alp_presence(true, UINT32_MAX - 4001, 1);
}
void exercise_advancing_clock_cancellation(bool sliders) {
    DriveRuntime r;
    if (sliders) {
        r.boot = true;
        r.tick(1000);
        r.boot = false;
        r.tick(1400);
        TEST_ASSERT_EQUAL_INT(1, r.display_.showSettingsSlidersCalls);
    }
    r.tick(10000, true);
    r.presentationElapsedMs = 1;
    r.tick(14001);
    TEST_ASSERT_TRUE(r.power_.autoPowerOffTimerRunningForTest());
    TEST_ASSERT_EQUAL_INT(0, r.battery_.powerOffCalls);
    r.tick(74002, true);
    TEST_ASSERT_TRUE(r.state_.alpSignalActive);
    TEST_ASSERT_FALSE(r.power_.autoPowerOffTimerRunningForTest());
    TEST_ASSERT_EQUAL_INT(0, r.battery_.powerOffCalls);
}
void test_normal_new_heartbeat_cancels_with_advancing_clock() {
    exercise_advancing_clock_cancellation(false);
}
void test_slider_new_heartbeat_cancels_with_advancing_clock() {
    exercise_advancing_clock_cancellation(true);
}
void test_disabled_auto_off_stays_disabled_after_alp_loss() {
    DriveRuntime r;
    r.settings_.settings.autoPowerOffMinutes = 0;
    r.tick(10000, true);
    r.presentationElapsedMs = 1;
    r.tick(14001);
    r.tick(100000);
    TEST_ASSERT_TRUE(r.power_.autoPowerOffArmedForTest());
    TEST_ASSERT_FALSE(r.power_.autoPowerOffTimerRunningForTest());
    TEST_ASSERT_EQUAL_INT(0, r.battery_.powerOffCalls);
}
void test_absent_unarmed_alp_stays_awake_with_advancing_clock() {
    DriveRuntime r;
    r.presentationElapsedMs = 1;
    r.tick(10000);
    r.tick(100000);
    TEST_ASSERT_FALSE(r.power_.autoPowerOffArmedForTest());
    TEST_ASSERT_FALSE(r.power_.autoPowerOffTimerRunningForTest());
    TEST_ASSERT_EQUAL_INT(0, r.battery_.powerOffCalls);
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_normal_heartbeat_and_silence_reconcile_power);
    RUN_TEST(test_slider_heartbeat_and_silence_reconcile_power);
    RUN_TEST(test_normal_heartbeat_at_expiry_cancels_before_shutdown);
    RUN_TEST(test_slider_heartbeat_at_expiry_cancels_before_shutdown);
    RUN_TEST(test_normal_alp_loss_after_phase_work_waits_full_interval);
    RUN_TEST(test_slider_alp_loss_after_phase_work_waits_full_interval);
    RUN_TEST(test_normal_alp_loss_at_clock_wrap_waits_full_interval);
    RUN_TEST(test_slider_alp_loss_at_clock_wrap_waits_full_interval);
    RUN_TEST(test_normal_new_heartbeat_cancels_with_advancing_clock);
    RUN_TEST(test_slider_new_heartbeat_cancels_with_advancing_clock);
    RUN_TEST(test_disabled_auto_off_stays_disabled_after_alp_loss);
    RUN_TEST(test_absent_unarmed_alp_stays_awake_with_advancing_clock);
    return UNITY_END();
}
