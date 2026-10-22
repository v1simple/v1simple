#define main existing_fade_startup_main
#include "test/test_volume_fade_startup/test_volume_fade_startup.cpp"
#undef main
#include "src/modules/touch/touch_ui_module.cpp"
#include "src/runtime_coordinator.h"
#include "src/touch_handler.cpp"
void audio_set_volume(uint8_t) {}
void play_test_voice() {}
// Integration adapter: uninteresting services are inert; speed and presentation
// forward to the real owners through the production coordinator's branch/order.
struct SliderRuntime : SpeedFadeRuntime {
    struct Connection {
        bool connected = true, backpressured = false, overloaded = false,
             requestShowInitialScanning = false;
    };
    bool boot = false, speedValid = true, parsedReady = false;
    float mph = 0;
    uint32_t now = 10000;
    TouchHandler touch;
    TouchUiModule touchUi;
    SliderRuntime() {
        touch.begin();
        touchUi.begin(&display, &touch, &settings, {});
    }
    void openSliders() {
        now = 11000;
        boot = true;
        tick();
        now = 11400;
        boot = false;
        tick();
        TEST_ASSERT_EQUAL_INT(1, display.showSettingsSlidersCalls);
    }
    bool active() { return true; }
    DriveLoopTiming beginDriveLoop() {
        mockMillis = now;
        return {0, now};
    }
    Connection processConnectionRuntime(uint32_t) { return {}; }
    void acceptConnectionSnapshot(Connection) {}
    void showInitialScanningScreen() {}
    void markInitialScanningScreenHandled() {}
    bool powerOwnsPresentation() { return false; }
    void presentConnectionState(Connection) {}
    void processPower(uint32_t) {}
    bool processTouch(uint32_t t) { return touchUi.process(t, boot); }
    void servicePowerDisplayOwnership(uint32_t) {}
    void processAlp(uint32_t) {}
    bool preemptSettingsForLiveAlert() { return false; }
    void openBootReadyGate(uint32_t) {}
    void processBleRuntime() { quiet.syncProxyVolumeOwnership(&fade); }
    void processGps(uint32_t) {}
    void processSpeed(uint32_t) {}
    void processSpeedAlert(uint32_t t) { speed.update(mph, speedValid, t); }
    void observeAlpProductState(uint32_t) {}
    void processPeriodicMaintenance(uint32_t, bool, bool, bool, bool) {}
    void finishDriveLoop(bool, uint32_t, bool) {}
    void processTapGesture(uint32_t) {}
    void processBleQueue() {}
    bool bleQueueBackpressured() { return false; }
    void processConnectionCycle(uint32_t, bool) {}
    void processObd(uint32_t, bool) {}
    void processAlpPresentationAndPower(uint32_t) {}
    bool hasLiveAlertPresentation() { return false; }
    void servicePowerAlertPresentation(uint32_t, bool) {}
    bool powerAlertPresentationNeedsRestore() { return false; }
    struct DisplayEdges {
        uint32_t nowMs;
        bool parsedReady;
    };
    struct {
        bool bootSplashHoldActive = false;
    } state_;
    struct {
        bool isActive() const { return false; }
    } autoPush_;
    struct {
        bool requiresExclusiveDetectorAdmission() const { return false; }
    } settingsOperations_;
    QuietCoordinatorModule &quiet_ = quiet;
    DisplayOrchestrationModule &displayOrchestration_ = orchestration;
    // This fixture has no voice jobs; the tail fixture exercises real pending
    // receipts. Keep the extracted parsed-policy service observable here.
    struct {
        unsigned serviceCalls = 0;
        void servicePendingVoice() { ++serviceCalls; }
    } displayPipeline_;
    DisplayEdges consumeDisplayEdges() {
        const DisplayEdges edges{now, parsedReady};
        parsedReady = false;
        return edges;
    }
    bool processParsedPolicies(const DisplayEdges &);
    void presentDisplay(const DisplayEdges &edges, bool) {
        processParsedPolicies(edges);
    }
    DriveLoopDispatch processConnectionDispatch(bool) { return {now, true}; }
    void tick() { DriveLoopCoordinator::tick(*this); }
    void packetAndTick() {
        // Supply a real fresh canonical volume frame and its pending parsed
        // edge. BLE queue scheduling is outside this adapter; the production
        // parser is real.
        const auto state = parser.getDisplayState();
        feed(PACKET_ID_RESP_CURRENT_VOLUME,
             {state.mainVolume, state.muteVolume}, now);
        parsedReady = true;
        tick();
    }
};
#include "slider_policies.inc"

void enterSpeedZero(SliderRuntime &r) {
    volume(6, 2, 9900);
    r.packetAndTick();
    TEST_ASSERT_TRUE(r.speed.getState().muteActive);
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastVolume);
    feed(PACKET_ID_RESP_CURRENT_VOLUME, {0, 2}, 10100);
    TEST_ASSERT_FALSE(parser.hasAlerts());
}
void test_normal_view_restores_original_pair_on_invalid_speed() {
    SliderRuntime r;
    enterSpeedZero(r);
    const int before = ble.setVolumeCalls;
    r.now = 12000;
    r.speedValid = false;
    r.packetAndTick();
    TEST_ASSERT_FALSE(r.speed.getState().muteActive);
    TEST_ASSERT_EQUAL_INT(before + 1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}
void test_slider_view_should_restore_original_pair_on_invalid_speed() {
    SliderRuntime r;
    enterSpeedZero(r);
    const int before = ble.setVolumeCalls;
    r.openSliders();
    r.now = 12000;
    r.speedValid = false;
    r.packetAndTick();
    TEST_ASSERT_FALSE(r.speed.getState().muteActive);
    TEST_ASSERT_EQUAL_INT(before + 1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
    TEST_ASSERT_EQUAL_INT(0, display.hideBrightnessSliderCalls);
    TEST_ASSERT_GREATER_THAN_UINT(0, r.displayPipeline_.serviceCalls);
}
void test_slider_without_parsed_edge_waits_then_restores() {
    SliderRuntime r;
    enterSpeedZero(r);
    const int before = ble.setVolumeCalls;
    r.openSliders();
    r.now = 12000;
    r.speedValid = false;
    r.parsedReady = false;
    r.tick();
    TEST_ASSERT_FALSE(r.speed.getState().muteActive);
    TEST_ASSERT_EQUAL_INT(before, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(0, ble.lastVolume);
    r.now = 13000;
    r.packetAndTick();
    TEST_ASSERT_EQUAL_INT(before + 1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
    TEST_ASSERT_EQUAL_UINT8(2, ble.lastMuteVolume);
}
void test_normal_view_without_parsed_edge_waits_for_presentation() {
    SliderRuntime r;
    enterSpeedZero(r);
    const int before = ble.setVolumeCalls;
    r.now = 12000;
    r.speedValid = false;
    r.parsedReady = false;
    r.tick();
    TEST_ASSERT_FALSE(r.speed.getState().muteActive);
    TEST_ASSERT_EQUAL_INT(before, ble.setVolumeCalls);
}
void test_slider_restore_respects_boot_hold_until_next_parsed_edge() {
    SliderRuntime r;
    enterSpeedZero(r);
    r.openSliders();
    const int before = ble.setVolumeCalls;
    r.state_.bootSplashHoldActive = true;
    r.now = 12000;
    r.speedValid = false;
    r.packetAndTick();
    TEST_ASSERT_EQUAL_INT(before, ble.setVolumeCalls);
    r.state_.bootSplashHoldActive = false;
    r.now = 13000;
    r.packetAndTick();
    TEST_ASSERT_EQUAL_INT(before + 1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_UINT8(6, ble.lastVolume);
}
void test_slider_restore_respects_phone_volume_ownership() {
    SliderRuntime r;
    enterSpeedZero(r);
    r.openSliders();
    const int before = ble.setVolumeCalls;
    ble.setProxyConnected(true);
    r.now = 12000;
    r.speedValid = false;
    r.packetAndTick();
    TEST_ASSERT_FALSE(r.speed.getState().muteActive);
    TEST_ASSERT_EQUAL_INT(before, ble.setVolumeCalls);
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_slider_restore_respects_boot_hold_until_next_parsed_edge);
    RUN_TEST(test_slider_restore_respects_phone_volume_ownership);
    RUN_TEST(test_normal_view_restores_original_pair_on_invalid_speed);
    RUN_TEST(test_normal_view_without_parsed_edge_waits_for_presentation);
    RUN_TEST(test_slider_view_should_restore_original_pair_on_invalid_speed);
    RUN_TEST(test_slider_without_parsed_edge_waits_then_restores);
    return UNITY_END();
}
