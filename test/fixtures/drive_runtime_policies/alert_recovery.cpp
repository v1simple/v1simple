// The existing stream fixture supplies the production phone queue, command
// worker, notification queue/parser and connection owner. Its NimBLE, clock,
// panel and quiescence/disconnect boundary remain host substitutes.
#define main existing_proxy_alert_stream_main
#include "test/test_ble_proxy_alert_stream/test_ble_proxy_alert_stream.cpp"
#undef main
#include "src/modules/ble/connection_state_cadence_module.cpp"
#include "src/modules/ble/connection_state_dispatch_module.cpp"
#include "src/modules/touch/touch_ui_module.cpp"
#include "src/touch_handler.cpp"
#include "src/runtime_coordinator.h"

void audio_set_volume(uint8_t) {}
void play_test_voice() {}

// Exercise the real transport deadline on each loop. Radio scan/disconnect
// dispatch and deferred persistence are outside this fixture's boundary.
void V1BLEClient::process(SettingsManager&) { serviceCommandWrite(); }

enum class Presentation { Normal, Sliders, Preview, Power };

struct RecoveryRuntime {
    StreamFixture& fixture;
    V1BLEClient& ble_;
    SettingsManager& settings_;
    ConnectionStateModule& connectionState_;
    TouchHandler touch_;
    TouchUiModule touchUi_;
    ConnectionStateCadenceModule cadence_;
    ConnectionStateDispatchModule connectionDispatch_;
    struct { bool running = false; bool ownsPresentation() const { return running; } } preview_;
    struct { bool bootSplashHoldActive = false; unsigned long activeScanScreenDwellMs = 0; } state_;
    struct { template <typename T> void syncProxyVolumeOwnership(T*) {} } quiet_;
    int volumeFade_ = 0;
    bool powerOwned = false;
    unsigned presentationCalls = 0;

    explicit RecoveryRuntime(StreamFixture& f)
        : fixture(f), ble_(f.client), settings_(f.settings), connectionState_(f.connection) {
        touch_.begin();
        touchUi_.begin(&f.display, &touch_, &settings_, {});
        connectionDispatch_.begin(cadence_, connectionState_);
    }
    void selectPresentation(Presentation mode) {
        powerOwned = mode == Presentation::Power;
        preview_.running = mode == Presentation::Preview;
        if (mode == Presentation::Sliders) {
            // Begin a supported short BOOT press before recovery admission;
            // its release in the next drive tick opens the real sliders.
            TEST_ASSERT_FALSE(touchUi_.process(mockMillis - 400, true));
        }
    }
    struct Connection {
        bool connected = true, backpressured = false, overloaded = false, requestShowInitialScanning = false;
    };
    bool active() const { return true; }
    DriveLoopTiming beginDriveLoop() { return {0, static_cast<uint32_t>(mockMillis)}; }
    Connection processConnectionRuntime(uint32_t) { fixture.queue.process(); return {ble_.isConnected()}; }
    void acceptConnectionSnapshot(Connection) {}
    void showInitialScanningScreen() {}
    void markInitialScanningScreenHandled() {}
    bool powerOwnsPresentation() const { return powerOwned; }
    void presentConnectionState(Connection) {}
    void processPower(uint32_t) {}
    bool processTouch(uint32_t nowMs) { return touchUi_.process(nowMs, false); }
    void servicePowerDisplayOwnership(uint32_t) {}
    void processAlp(uint32_t) {}
    bool preemptSettingsForLiveAlert() {
        return fixture.parser.hasAlerts() && touchUi_.preemptForLiveAlert();
    }
    void openBootReadyGate(uint32_t) {}
    void processBleRuntime();
    void processGps(uint32_t) {}
    void processSpeed(uint32_t) {}
    void processSpeedAlert(uint32_t) {}
    void observeAlpProductState(uint32_t) {}
    void processPeriodicMaintenance(uint32_t, bool, bool, bool, bool) {}
    void finishDriveLoop(bool, uint32_t, bool) { fixture.queue.process(); }
    void processTapGesture(uint32_t) {}
    void processBleQueue() { fixture.queue.process(); }
    bool bleQueueBackpressured() const { return false; }
    void processConnectionCycle(uint32_t, bool) {}
    void processObd(uint32_t, bool) {}
    void processAlpPresentationAndPower(uint32_t) {}
    bool hasLiveAlertPresentation() const { return fixture.parser.hasAlerts(); }
    void servicePowerAlertPresentation(uint32_t, bool) {}
    bool powerAlertPresentationNeedsRestore() const { return false; }
    int consumeDisplayEdges() { return 0; }
    void processParsedPolicies(int) {}
    void presentDisplay(int, bool) { ++presentationCalls; }
    DriveLoopDispatch processConnectionDispatch(bool);
    void tick() { DriveLoopCoordinator::tick(*this); }
};

// Exact production adapters bind per-loop BLE service and presentation dispatch.
#include "alert_recovery.inc"

void prepareRecovery(StreamFixture& f) {
    mock_reset_task_create_state();
    f.phoneStop();
    f.disconnectPhone();
    f.remote.setCanWrite(true);
    f.remote.setCanWriteNoResponse(false);
    f.client.acceptClientCallbacks_.store(true);
    mockMillis += 1500;
    f.idle();
}

void completeRecoveryWorker(StreamFixture& f) {
    const auto worker = g_mock_task_create_state.lastTaskFunction;
    TEST_ASSERT_NOT_NULL(worker);
    worker(g_mock_task_create_state.lastTaskParameter);
    TEST_ASSERT_TRUE(f.client.commandWorkerResult_.load());
    TEST_ASSERT_FALSE(f.client.discoveryTaskRunning_.load());
}

void tickIdle(RecoveryRuntime& runtime) {
    mockMillis += 100;
    runtime.fixture.enqueue(StreamFixture::frame(
        PACKET_ID_DISPLAY_DATA, {0x77, 0x77, 0, 0, 0, 0x0C, 0x04, 0x62}));
    runtime.tick();
}

void acknowledgedRecovery(Presentation mode) {
    StreamFixture f;
    prepareRecovery(f);
    RecoveryRuntime runtime(f);
    runtime.selectPresentation(mode);
    TEST_ASSERT_TRUE(f.connection.process(mockMillis));
    TEST_ASSERT_TRUE(f.client.commandInFlight(V1CommandOwner::AlertStreamRecovery));
    completeRecoveryWorker(f);
    const auto writes = f.remote.writeValueCalls();
    for (unsigned i = 0; i < 15; ++i) tickIdle(runtime);
    TEST_ASSERT_TRUE_MESSAGE(f.client.isConnected(), "Presentation abandoned an acknowledged recovery");
    TEST_ASSERT_FALSE(f.client.commandInFlight(V1CommandOwner::AlertStreamRecovery));
    TEST_ASSERT_TRUE(f.client.alertDataRequestGate_.hasSuccessfulSend(1));
    TEST_ASSERT_EQUAL_UINT32(writes, f.remote.writeValueCalls());
    if (mode == Presentation::Sliders) {
        TEST_ASSERT_EQUAL_INT(1, f.display.showSettingsSlidersCalls);
        TEST_ASSERT_EQUAL_UINT32(0, runtime.presentationCalls);
        TEST_ASSERT_EQUAL_INT(0, f.display.hideBrightnessSliderCalls);
    }
    // Completion must release the transport slot for another owner.
    mockMillis += 10;
    V1BLEClient::CommandScope next(f.client, V1CommandOwner::AutoPush);
    TEST_ASSERT_EQUAL(SendResult::IN_FLIGHT, f.client.requestAllVolumeResult());
    completeRecoveryWorker(f);
    TEST_ASSERT_EQUAL(SendResult::SENT, f.client.requestAllVolumeResult());
}

void test_acknowledged_recovery_completes_in_normal_view() { acknowledgedRecovery(Presentation::Normal); }
void test_acknowledged_recovery_completes_under_real_sliders() { acknowledgedRecovery(Presentation::Sliders); }
void test_acknowledged_recovery_completes_under_preview() { acknowledgedRecovery(Presentation::Preview); }
void test_acknowledged_recovery_completes_under_power_warning() { acknowledgedRecovery(Presentation::Power); }

void test_sliders_do_not_start_unadmitted_recovery() {
    StreamFixture f;
    prepareRecovery(f);
    RecoveryRuntime runtime(f);
    runtime.selectPresentation(Presentation::Sliders);
    const auto writes = f.remote.writeValueCalls();
    for (unsigned i = 0; i < 15; ++i) tickIdle(runtime);
    TEST_ASSERT_TRUE(f.client.isConnected());
    TEST_ASSERT_FALSE(f.client.commandInFlight(V1CommandOwner::AlertStreamRecovery));
    TEST_ASSERT_EQUAL_UINT32(writes, f.remote.writeValueCalls());
    TEST_ASSERT_FALSE(f.detailedStream);
}

void test_sliders_preserve_connected_phone_stop() {
    StreamFixture f;
    f.phoneStop();
    RecoveryRuntime runtime(f);
    runtime.selectPresentation(Presentation::Sliders);
    const auto writes = f.remote.writeValueCalls();
    for (unsigned i = 0; i < 15; ++i) tickIdle(runtime);
    TEST_ASSERT_TRUE(f.client.phoneOwnsStoppedAlertStream());
    TEST_ASSERT_FALSE(f.detailedStream);
    TEST_ASSERT_EQUAL_UINT32(writes, f.remote.writeValueCalls());
}

void test_unacknowledged_recovery_still_times_out_under_sliders() {
    StreamFixture f;
    prepareRecovery(f);
    RecoveryRuntime runtime(f);
    runtime.selectPresentation(Presentation::Sliders);
    f.connection.process(mockMillis);
    TEST_ASSERT_TRUE(f.client.commandInFlight(V1CommandOwner::AlertStreamRecovery));
    for (unsigned i = 0; i < 9; ++i) tickIdle(runtime);
    TEST_ASSERT_TRUE(f.client.isConnected());
    tickIdle(runtime);
    TEST_ASSERT_FALSE(f.client.isConnected());
    TEST_ASSERT_EQUAL(BLEState::QUIESCING, f.client.bleState_);
    TEST_ASSERT_FALSE(f.client.alertDataRequestGate_.hasSuccessfulSend(1));
}

void test_retired_session_cannot_publish_completed_recovery_under_sliders() {
    StreamFixture f;
    prepareRecovery(f);
    RecoveryRuntime runtime(f);
    runtime.selectPresentation(Presentation::Sliders);
    f.connection.process(mockMillis);
    completeRecoveryWorker(f);
    f.client.sessionPublicationGate_.close();
    mockMillis += 100;
    runtime.tick();
    TEST_ASSERT_FALSE(f.client.isConnected());
    TEST_ASSERT_FALSE(f.client.alertDataRequestGate_.hasSuccessfulSend(1));
}

void test_live_alert_still_preempts_sliders_after_recovery() {
    StreamFixture f;
    prepareRecovery(f);
    RecoveryRuntime runtime(f);
    runtime.selectPresentation(Presentation::Sliders);
    f.connection.process(mockMillis);
    completeRecoveryWorker(f);
    tickIdle(runtime);
    TEST_ASSERT_EQUAL_INT(1, f.display.showSettingsSlidersCalls);
    f.enqueue(StreamFixture::frame(PACKET_ID_ALERT_DATA, {0x11, 0x87, 0x8C, 0xAC, 0, 0x22, 0x80}));
    mockMillis += 100;
    runtime.tick();
    TEST_ASSERT_TRUE(f.client.isConnected());
    TEST_ASSERT_TRUE(f.parser.hasAlerts());
    TEST_ASSERT_EQUAL_INT(1, f.display.hideBrightnessSliderCalls);
    TEST_ASSERT_GREATER_THAN_UINT32(0, runtime.presentationCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_acknowledged_recovery_completes_in_normal_view);
    RUN_TEST(test_acknowledged_recovery_completes_under_real_sliders);
    RUN_TEST(test_acknowledged_recovery_completes_under_preview);
    RUN_TEST(test_acknowledged_recovery_completes_under_power_warning);
    RUN_TEST(test_sliders_do_not_start_unadmitted_recovery);
    RUN_TEST(test_sliders_preserve_connected_phone_stop);
    RUN_TEST(test_unacknowledged_recovery_still_times_out_under_sliders);
    RUN_TEST(test_retired_session_cannot_publish_completed_recovery_under_sliders);
    RUN_TEST(test_live_alert_still_preempts_sliders_after_recovery);
    return UNITY_END();
}
