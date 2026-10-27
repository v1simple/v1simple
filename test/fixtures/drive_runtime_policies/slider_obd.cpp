// Keep the real OBD state machine, command parser and speed-source selector.
// The shared fixture completes connection/discovery/initialization through the
// runtime callbacks; the transport is the deterministic native adapter.
#define main existing_obd_main
#include "test/test_obd_runtime/test_obd_runtime.cpp"
#undef main

// OBD already includes the production settings types. Supply only the storage
// and display boundaries needed by the real local-input owner.
#define SETTINGS_H
class SettingsManager {
  public:
    V1Settings settings;
    const V1Settings& get() const { return settings; }
    SettingsPersistResult setStealthEnabled(bool enabled, SettingsPersistMode) {
        settings.stealthEnabled = enabled;
        SettingsPersistResult result;
        result.success = true;
        return result;
    }
    void updateBrightness(uint8_t value) { settings.brightness = value; }
    void updateVoiceVolume(uint8_t value) { settings.voiceVolume = value; }
    void requestDeferredPersist() {}
    bool saveDeferredBackup() { return true; }
    bool deferredPersistPending() const { return false; }
};
#include "test/mocks/display.h"
#include "src/touch_handler.cpp"
#include "src/modules/touch/touch_ui_module.cpp"
#include "src/modules/speed_mute/speed_mute_module.cpp"
#include "src/runtime_coordinator.h"

void audio_set_volume(uint8_t) {}
void play_test_voice() {}

// Unrelated services are inert. Local input, OBD polling, speed freshness and
// low-speed mute all run their production owners in the real drive-loop order.
struct ObdSliderRuntime : Fixture {
    SettingsManager settings;
    V1Display display;
    TouchHandler touch;
    TouchUiModule ui;
    SpeedSourceSelector selector;
    SpeedMuteModule mute;
    bool boot = false;
    bool liveAlert = false;
    bool settingsPreempted = false;
    bool replyEnabled = true;
    uint32_t lastReplySent = 0;
    struct Connection {
        bool connected = true, backpressured = false, overloaded = false,
             requestShowInitialScanning = false;
    };

    ObdSliderRuntime() {
        bootToPolling();
        touch.begin();
        ui.begin(&display, &touch, &settings, {});
        selector.begin(&runtime, true, nullptr, false);
        mute.begin(true, 25, 3, 0, true);
    }

    bool active() const { return true; }
    DriveLoopTiming beginDriveLoop() { return {nowMs * 1000, nowMs}; }
    Connection processConnectionRuntime(uint32_t) { return {}; }
    void acceptConnectionSnapshot(Connection) {}
    void showInitialScanningScreen() {}
    void markInitialScanningScreenHandled() {}
    bool powerOwnsPresentation() { return false; }
    void presentConnectionState(Connection) {}
    void processAlp(uint32_t) {}
    void processAlpPresentationAndPower(uint32_t) {}
    void processPower(uint32_t) {}
    bool processTouch(uint32_t at) { return ui.process(at, boot); }
    void servicePowerDisplayOwnership(uint32_t) {}
    bool preemptSettingsForLiveAlert() {
        settingsPreempted = liveAlert && ui.preemptForLiveAlert();
        return settingsPreempted;
    }
    void openBootReadyGate(uint32_t) {}
    void processBleRuntime() {}
    void processGps(uint32_t) {}
    void processSpeed(uint32_t at) { selector.update(at); }
    void processSpeedAlert(uint32_t at) {
        const auto speed = selector.selectedSpeed();
        mute.update(speed.speedMph, speed.valid, at);
    }
    void observeAlpProductState(uint32_t) {}
    int consumeDisplayEdges() { return 0; }
    bool processParsedPolicies(int) { return true; }
    void processPeriodicMaintenance(uint32_t, bool, bool, bool, bool) {}
    void finishDriveLoop(bool, uint32_t, bool) {}
    void processTapGesture(uint32_t) {}
    void processBleQueue() {}
    bool bleQueueBackpressured() { return false; }
    void processConnectionCycle(uint32_t, bool) {}
    void processObd(uint32_t at, bool) { runtime.update(at, context); }
    bool hasLiveAlertPresentation() { return liveAlert; }
    void servicePowerAlertPresentation(uint32_t, bool) {}
    bool powerAlertPresentationNeedsRestore() { return false; }
    void presentDisplay(int, bool) {}
    DriveLoopDispatch processConnectionDispatch(bool) { return {nowMs, true}; }

    void tick(uint32_t at) {
        nowMs = at;
        mockMillis = at;
        // A healthy adapter returns a stopped-vehicle response to each actual
        // speed request, after its transport reports write completion. Extra
        // unsolicited speed replies would hide a stopped polling owner.
        const auto& command = runtime.activeCommand_;
        if (replyEnabled && command.active && command.kind == ObdCommandKind::SPEED &&
            !command.writeResultPending && command.sentMs != lastReplySent) {
            lastReplySent = command.sentMs;
            const char* response = "41 0D 00\r>";
            runtime.onBleData(reinterpret_cast<const uint8_t*>(response), strlen(response));
        }
        DriveLoopCoordinator::tick(*this);
    }

    void run(uint32_t durationMs) {
        const uint32_t end = nowMs + durationMs;
        while (nowMs < end) tick(nowMs + 10);
    }

    void enterSliders() {
        boot = true;
        run(400);
        boot = false;
        tick(nowMs + 10);
        TEST_ASSERT_EQUAL_INT(1, display.showSettingsSlidersCalls);
    }
};

void verifyHealthySpeed(bool sliders, bool alertAtEnd) {
    ObdSliderRuntime runtime;
    runtime.run(1000);
    TEST_ASSERT_TRUE(runtime.selector.selectedSpeed().valid);
    TEST_ASSERT_TRUE(runtime.mute.getState().muteActive);
    if (sliders) runtime.enterSliders();
    const uint32_t polls = runtime.runtime.snapshot(runtime.nowMs).pollCount;
    runtime.run(obd::SPEED_MAX_AGE_MS + 1000);
    if (alertAtEnd) {
        runtime.liveAlert = true;
        runtime.tick(runtime.nowMs + 10);
        TEST_ASSERT_TRUE(runtime.settingsPreempted);
    }
    TEST_ASSERT_TRUE_MESSAGE(runtime.selector.selectedSpeed().valid,
                             "Healthy OBD speed became unavailable during local adjustment");
    TEST_ASSERT_TRUE_MESSAGE(runtime.mute.getState().muteActive,
                             "Local adjustment released configured low-speed mute while stopped");
    TEST_ASSERT_GREATER_THAN_UINT32(polls, runtime.runtime.snapshot(runtime.nowMs).pollCount);
}

void test_normal_driving_keeps_healthy_obd_speed() { verifyHealthySpeed(false, false); }
void test_sliders_keep_healthy_obd_speed() { verifyHealthySpeed(true, false); }
void test_live_alert_preempting_sliders_has_healthy_obd_speed() { verifyHealthySpeed(true, true); }

void test_sliders_still_expire_speed_when_adapter_stops_replying() {
    ObdSliderRuntime runtime;
    runtime.run(1000);
    TEST_ASSERT_TRUE(runtime.selector.selectedSpeed().valid);
    TEST_ASSERT_TRUE(runtime.mute.getState().muteActive);
    runtime.enterSliders();
    runtime.replyEnabled = false;
    runtime.run(obd::SPEED_MAX_AGE_MS + 1000);
    TEST_ASSERT_FALSE(runtime.selector.selectedSpeed().valid);
    TEST_ASSERT_FALSE(runtime.mute.getState().muteActive);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_normal_driving_keeps_healthy_obd_speed);
    RUN_TEST(test_sliders_keep_healthy_obd_speed);
    RUN_TEST(test_live_alert_preempting_sliders_has_healthy_obd_speed);
    RUN_TEST(test_sliders_still_expire_speed_when_adapter_stops_replying);
    return UNITY_END();
}
