// The public painter fixture supplies the real ALP/runtime owner chain. Here
// input goes through DriveLoopCoordinator and the extracted production adapters,
// never the helper's unconditional feed()/handleParsed() shortcut.
#define ALP_DISPLAY_REAL_STATUS 1
#include "test/fixtures/alp_display_runtime.h"
#include "src/display_status_bar.cpp"
#include "include/display_ble_freshness.h"
#include "display_context.inc"
#include "src/runtime_coordinator.h"
#include "src/modules/ble/connection_runtime_module.h"
#include "src/modules/display/display_orchestration_module.h"
#include "test/mocks/modules/power/power_module.h"
#include "test/mocks/modules/ble/ble_queue_module.h"
#include "src/modules/ble/connection_state_module.cpp"
#include "src/modules/ble/connection_state_cadence_module.cpp"
#include "src/modules/ble/connection_state_dispatch_module.cpp"

// Preview ownership is scripted; its rendering is outside this boundary.
class DisplayPreviewModule {
  public:
    bool running = false;
    bool isRunning() const { return running; }
    bool ownsPresentation() const { return running; }
    void update() {}
};
class DisplayRestoreModule { public: bool process() { return false; } };
void DisplayOrchestrationModule::syncQuietPresentation() {}
bool DisplayOrchestrationModule::processSpeedVolume(uint32_t) { return false; }
void DisplayOrchestrationModule::executeVolumeFade(uint32_t) {}
void QuietCoordinatorModule::processInTheBox(uint32_t) { TEST_FAIL_MESSAGE("unexpected quiet owner"); }
#include "orchestration.inc"
#include "src/touch_handler.cpp"
#include "src/v1_settings_operation.h"
#include "src/modules/touch/tap_gesture_module.cpp"
class AutoPushModule {
  public: bool active = false; bool isActive() const { return active; }
};
SendResult QuietCoordinatorModule::sendMuteResult(QuietOwner, bool) {
    TEST_FAIL_MESSAGE("unexpected mute command"); return SendResult::FAILED;
}
struct SettingsOperationAdapter {
    bool busy = false;
    bool prepareOk = true;
    unsigned starts = 0;
    unsigned finishes = 0;
    bool requiresExclusiveDetectorAdmission() const { return busy; }
    V1SettingsOperationStore::StartResult startApply(int, const char*,
        V1SettingsOperationStore::Source, bool, bool) {
        ++starts;
        return {V1SettingsOperationStore::StartStatus::Started, 1};
    }
    bool markPreparing() { return prepareOk; }
    bool finish(V1SettingsOperationStore::State, V1SettingsOperationStore::Reason) {
        ++finishes; return true;
    }
};

class DriveRuntime : public Runtime {
  public:
    SystemEventBus systemEvents_;
    DisplayPreviewModule preview_;
    DisplayRestoreModule restore_;
    DisplayOrchestrationModule displayOrchestration_;
    BleQueueModule bleQueue_;
    ConnectionStateModule connection_;
    ConnectionStateCadenceModule cadence_;
    ConnectionStateDispatchModule connectionDispatch_;
    BatteryManager battery;
    WiFiManager wifi;
    GpsRuntimeModule gps;
    // References let the extracted production methods use their real names.
    V1BLEClient& ble_ = ble;
    DisplayPipelineModule& displayPipeline_ = pipeline;
    struct { bool bootSplashHoldActive = false; uint32_t activeScanScreenDwellMs = 0; } state_;
    struct { void setInTheBoxSuspended(bool) {} } quiet_;
    AutoPushModule autoPush_;
    SettingsOperationAdapter settingsOperations_;
    SettingsManager& settings_ = settings;
    V1Display& display_ = display;
    AlertPersistenceModule& alertPersistence_ = persistence;
    DisplayMode& displayMode_ = mode;
    AlpRuntimeModule& alp_ = alp;
    TouchHandler touch_;
    TapGestureModule tapGesture_;
    uint32_t observedSettingsOperationId_ = 0;
    bool beginTripleTapProfileCycle(int newSlot);
    bool connectedV1Address(String& address) {
        address = "AA:BB:CC:DD:EE:FF";
        return ble.isConnected();
    }
    bool overloaded = false;
    bool inSettings = false;

    explicit DriveRuntime(bool connected = false, uint8_t persistSec = 0) : Runtime(connected, persistSec) {
        alp.setEventBus(&systemEvents_);
        displayOrchestration_.begin(&display, &ble, &preview_, &restore_, &parser, nullptr, nullptr, nullptr);
        connection_.begin(&ble, &parser, &display, nullptr, &bleQueue_, &persistence);
        connection_.setDisplayOwnerRestoreCallback([](void* context, uint32_t now) {
            return static_cast<DriveRuntime*>(context)->pipeline.restoreCurrentOwner(now);
        }, this);
        connectionDispatch_.begin(cadence_, connection_);
        display.setSystemStatusSources(battery, wifi, gps);
        settings.mutableSettings().showBatteryPercent = true;
        battery.setHasBattery(true);
        battery.setVoltage(3.9f);
        battery.setBatteryPercent(80);
        display.showScanning();
        Wire.resetMock();
        TEST_ASSERT_TRUE(touch_.begin());
        TapGestureModule::Callbacks callbacks{};
        callbacks.beginProfileCycle = [](int slot, void* context) {
            return static_cast<DriveRuntime*>(context)->beginTripleTapProfileCycle(slot);
        };
        callbacks.beginProfileCycleContext = this;
        tapGesture_.begin(&touch_, &settings, &display, &ble, &parser, &autoPush_,
                          &persistence, &mode, nullptr, callbacks);
    }

    struct DisplayEdges { uint32_t nowMs = 0; bool parsedReady = false; };
    DisplayEdges consumeDisplayEdges();
    void presentDisplay(const DisplayEdges&, bool overloadThisLoop);
    void presentConnectionState(const ConnectionRuntimeSnapshot&);
    DriveLoopDispatch processConnectionDispatch(bool powerPresentationOwned);

    bool active() const { return true; }
    DriveLoopTiming beginDriveLoop() { return {0, static_cast<uint32_t>(mockMillis)}; }
    ConnectionRuntimeSnapshot processConnectionRuntime(uint32_t) {
        ConnectionRuntimeSnapshot snapshot;
        snapshot.connected = ble.isConnected();
        snapshot.overloaded = overloaded;
        return snapshot;
    }
    void acceptConnectionSnapshot(const ConnectionRuntimeSnapshot&) {}
    void markInitialScanningScreenHandled() {}
    void showInitialScanningScreen() { display.showScanning(); }
    bool powerOwnsPresentation() const { return false; }
    void processPower(uint32_t) {}
    bool processTouch(uint32_t) { return inSettings; }
    void servicePowerDisplayOwnership(uint32_t) {}
    void processAlp(uint32_t now) { alp.process(now); }
    bool preemptSettingsForLiveAlert() {
        if (!alp.currentEvent().active) return false;
        inSettings = false;
        return true;
    }
    void openBootReadyGate(uint32_t) {}
    void processBleRuntime() {}
    void processBleQueue() {}
    bool bleQueueBackpressured() { return false; }
    void processConnectionCycle(uint32_t, bool) {}
    void processObd(uint32_t, bool) {}
    void observeAlpProductState(uint32_t) {}
    void processAlpPresentationAndPower(uint32_t) {}
    bool processParsedPolicies(const DisplayEdges&);
    void processGps(uint32_t) {}
    void processSpeed(uint32_t) {}
    void processSpeedAlert(uint32_t) {}
    bool hasLiveAlertPresentation() const { return alp.currentEvent().active; }
    void servicePowerAlertPresentation(uint32_t, bool) {}
    bool powerAlertPresentationNeedsRestore() const { return false; }
    void processTapGesture(uint32_t nowMs);
    void processPeriodicMaintenance(uint32_t, bool, bool, bool, bool) {}
    void finishDriveLoop(bool, uint32_t, bool) {}

    void loop(uint32_t now) { mockMillis = now; DriveLoopCoordinator::tick(*this); }
    void queueTouchReport(uint16_t x, uint16_t y) {
        std::vector<uint8_t> frame(32, 0);
        frame[1] = 1;
        frame[2] = x >> 8;
        frame[3] = x;
        frame[4] = y >> 8;
        frame[5] = y;
        Wire.queueRequestFrom(frame.size(), frame);
    }
    void queueReleaseReport(uint8_t fill) {
        Wire.queueRequestFrom(32, std::vector<uint8_t>(32, fill));
    }
    void uart(uint32_t now, uint8_t a, uint8_t b, uint8_t c) {
        const uint8_t bytes[]{a, b, c, alpChecksum(a, b, c)};
        alp.testInjectBytes(bytes, sizeof(bytes));
        alp.testSetLastUartByteMs(now);
        loop(now);
    }
    void heartbeat(uint32_t now, uint8_t mode) { uart(now, 0xB0, mode, 0); }
    void laser() {
        heartbeat(100, 3);
        uart(200, 0x98, 0, 0xE3);
        uart(210, 0xC8, 0, 0xD5);
        TEST_ASSERT_TRUE(display.ut_alpHasLaserEvent());
        TEST_ASSERT_EQUAL_STRING("PL3", display.ut_alpFreqText());
    }
    void assertNoTransfer() {
        TEST_ASSERT_EQUAL_INT(0, display.testCanvas()->getFlushCount());
        TEST_ASSERT_TRUE(regionTransfers.empty());
    }
};
#include "drive_display.inc"
#include "profile_cycle.inc"

void test_disconnected_dli_mode_reaches_panel_without_an_event_or_v1_packet() {
    DriveRuntime r;
    r.heartbeat(100, 2);
    TEST_ASSERT_EQUAL(2, r.display.ut_elementCaches().alp.lastHbByte1);
    r.clearPaint();
    r.heartbeat(200, 3);
    TEST_ASSERT_EQUAL(3, r.alp.lastHeartbeatByte1());
    TEST_ASSERT_EQUAL(2, r.display.ut_elementCaches().alp.lastHbByte1);
    r.assertNoTransfer();
    r.loop(249);
    r.assertNoTransfer();
    r.loop(250);
    TEST_ASSERT_EQUAL(3, r.display.ut_elementCaches().alp.lastHbByte1);
    TEST_ASSERT_EQUAL_INT(1, r.display.testCanvas()->getFlushCount());
    TEST_ASSERT_TRUE(regionTransfers.empty());
    TEST_ASSERT_EQUAL_INT(0, r.display.testCanvas()->getFillScreenCount());
    r.clearPaint();
    for (uint32_t now = 251; now < 10000; ++now) {
        if (now % 100 == 0) r.heartbeat(now, 3); else r.loop(now);
    }
    r.assertNoTransfer();
}

void test_lid_mode_event_still_updates_immediately() {
    DriveRuntime r;
    r.heartbeat(100, 2);
    r.clearPaint();
    r.heartbeat(200, 4);
    TEST_ASSERT_EQUAL(4, r.display.ut_elementCaches().alp.lastHbByte1);
    TEST_ASSERT_EQUAL_INT(1, r.display.testCanvas()->getFlushCount());
    TEST_ASSERT_TRUE(regionTransfers.empty());
}

void test_battery_canvas_change_is_transferred_by_the_scheduled_owner_once() {
    DriveRuntime r;
    r.loop(250); // Establish the first scheduled owner and ordinary BLE/context cache.
    r.clearPaint();
    r.battery.setBatteryPercent(79);
    r.loop(300); // Real 50 ms connection cadence paints after presentation.
    TEST_ASSERT_EQUAL(79, r.display.ut_elementCaches().battery.lastPctDrawn);
    TEST_ASSERT_FALSE(r.display.ut_drawnRegionEmpty());
    r.assertNoTransfer();
    r.loop(500);
    TEST_ASSERT_TRUE(r.display.ut_drawnRegionEmpty());
    TEST_ASSERT_EQUAL_INT(1, r.display.testCanvas()->getFlushCount());
    TEST_ASSERT_TRUE(regionTransfers.empty());
    r.clearPaint();
    for (uint32_t now = 501; now < 3000; ++now) r.loop(now);
    r.assertNoTransfer();
}

void test_unchanged_disconnected_idle_never_transfers() {
    DriveRuntime r;
    r.loop(250); // Settle initial status context before counting.
    r.clearPaint();
    for (uint32_t now = 251; now < 10000; ++now) r.loop(now);
    r.assertNoTransfer();
    TEST_ASSERT_EQUAL(DisplayMode::IDLE, r.mode);
    TEST_ASSERT_EQUAL_INT(0, r.display.testCanvas()->getFillScreenCount());
}

void test_preview_boot_overload_and_settings_gates_defer_status_refresh() {
    for (int gate = 0; gate < 4; ++gate) {
        DriveRuntime r;
        r.heartbeat(100, 2);
        r.clearPaint();
        r.preview_.running = gate == 0;
        r.state_.bootSplashHoldActive = gate == 1;
        r.overloaded = gate == 2;
        r.inSettings = gate == 3;
        for (uint32_t now = 200; now <= 1000; now += 100) r.heartbeat(now, 3);
        r.assertNoTransfer();
        TEST_ASSERT_EQUAL(2, r.display.ut_elementCaches().alp.lastHbByte1);
        r.preview_.running = false;
        r.state_.bootSplashHoldActive = false;
        r.overloaded = false;
        r.inSettings = false;
        r.loop(1001);
        TEST_ASSERT_EQUAL(3, r.display.ut_elementCaches().alp.lastHbByte1);
        TEST_ASSERT_EQUAL_INT(1, r.display.testCanvas()->getFlushCount());
        TEST_ASSERT_TRUE(regionTransfers.empty());
    }
}

void test_live_and_persisted_alp_keep_ownership_until_the_scheduled_expiry() {
    DriveRuntime r(false, 3);
    r.settings.mutableSettings().stealthEnabled = true;
    r.laser();
    r.loop(250);
    TEST_ASSERT_TRUE(r.display.ut_alpHasLaserEvent());
    TEST_ASSERT_EQUAL_STRING("PL3", r.display.ut_alpFreqText());
    TEST_ASSERT_EQUAL(DisplayMode::LIVE, r.mode);
    r.loop(3211);
    TEST_ASSERT_FALSE(r.display.ut_alpHasLaserEvent());
    TEST_ASSERT_EQUAL_STRING("PL3", r.display.ut_alpFreqText());
    r.loop(4000);
    TEST_ASSERT_EQUAL(DisplayMode::LIVE, r.mode);
    r.clearPaint();
    r.loop(6210);
    TEST_ASSERT_EQUAL(DisplayMode::LIVE, r.mode);
    r.clearPaint();
    r.loop(6211);
    r.scan();
    TEST_ASSERT_FALSE(r.display.isStealthScreen());
    r.clearPaint();
    r.loop(6500);
    r.assertNoTransfer();
}

void test_abnormal_listening_hold_and_tail_use_real_deadline_wakes() {
    DriveRuntime r(false, 3);
    r.laser();
    r.uart(250, 0xD0, 0, 0xFD);
    for (uint32_t now = 1000; now <= 5000; now += 1000) r.heartbeat(now, 6);
    r.heartbeat(5251, 6);
    TEST_ASSERT_FALSE(r.alp.currentEvent().active);
    TEST_ASSERT_TRUE(r.display.ut_alpHasLaserEvent());
    r.loop(5501); // Periodic status refresh must preserve the bounded hold.
    TEST_ASSERT_TRUE(r.display.ut_alpHasLaserEvent());
    r.loop(6251);
    TEST_ASSERT_FALSE(r.display.ut_alpHasLaserEvent());
    TEST_ASSERT_EQUAL_STRING("PL3", r.display.ut_alpFreqText());
    r.clearPaint();
    r.loop(9251);
    r.scan();
}

void test_connected_idle_is_not_repainted_by_the_disconnected_cadence() {
    DriveRuntime r(true);
    r.loop(100); // Real connection cadence restores the connected owner.
    TEST_ASSERT_EQUAL_STRING("0", r.display.ut_elementCaches().topCounter.lastText);
    r.clearPaint();
    for (uint32_t now = 101; now < 2000; ++now) r.loop(now);
    r.assertNoTransfer();
    r.ble.setConnected(false);
    r.loop(2000);
    TEST_ASSERT_EQUAL(DisplayMode::IDLE, r.mode);
    TEST_ASSERT_EQUAL_STRING("SCAN", r.display.ut_fontMgr().segment7.lastPrinted);
    TEST_ASSERT_FALSE(r.display.ut_alpHasLaserEvent());
    TEST_ASSERT_TRUE(r.display.testCanvas()->getFlushCount() > 0);
    r.clearPaint();
    r.loop(2250);
    r.assertNoTransfer();
    r.ble.setConnected(true);
    r.loop(2300);
    TEST_ASSERT_EQUAL_STRING("0", r.display.ut_elementCaches().topCounter.lastText);
    TEST_ASSERT_EQUAL_INT(1, r.display.testCanvas()->getFlushCount());
}

void test_connection_changes_preserve_live_alp_and_release_to_current_owner() {
    DriveRuntime r(false, 3);
    r.laser();
    r.ble.setConnected(true);
    r.loop(300);
    TEST_ASSERT_TRUE(r.display.ut_alpHasLaserEvent());
    TEST_ASSERT_EQUAL_STRING("PL3", r.display.ut_alpFreqText());
    r.ble.setConnected(false);
    r.loop(350);
    TEST_ASSERT_TRUE(r.display.ut_alpHasLaserEvent());
    r.loop(3211);
    TEST_ASSERT_FALSE(r.display.ut_alpHasLaserEvent());
    TEST_ASSERT_EQUAL_STRING("PL3", r.display.ut_alpFreqText());
    r.ble.setConnected(true);
    r.loop(3300);
    TEST_ASSERT_EQUAL_STRING("PL3", r.display.ut_alpFreqText());
    r.loop(6211);
    TEST_ASSERT_EQUAL(DisplayMode::IDLE, r.mode);
    TEST_ASSERT_EQUAL_STRING("0", r.display.ut_elementCaches().topCounter.lastText);
    TEST_ASSERT_FALSE(r.display.ut_alpFreqOverride());
}

void test_connected_v1_live_owner_survives_idle_ticks_then_disconnect_clears_it() {
    DriveRuntime r(true);
    r.loop(100);
    const uint8_t row[]{0xAA, 0xD8, 0xEA, 0x43, 8, 0x11, 0x5E, 0x56, 0xA0, 0, 0x24, 0x80};
    std::vector<uint8_t> packet(row, row + sizeof(row));
    uint8_t checksum = 0;
    for (uint8_t byte : packet) checksum += byte;
    packet.push_back(checksum);
    packet.push_back(0xAB);
    TEST_ASSERT_TRUE(r.parser.parse(packet.data(), packet.size(), 110));
    r.bleQueue_.setParsedFlag(true);
    r.loop(110);
    TEST_ASSERT_EQUAL(DisplayMode::LIVE, r.mode);
    TEST_ASSERT_EQUAL(BAND_K, frequencyBand);
    r.loop(500);
    TEST_ASSERT_EQUAL(DisplayMode::LIVE, r.mode);
    TEST_ASSERT_EQUAL(BAND_K, frequencyBand);
    r.ble.setConnected(false);
    r.clearPaint();
    r.loop(550);
    TEST_ASSERT_FALSE(r.parser.hasAlerts());
    TEST_ASSERT_EQUAL(DisplayMode::IDLE, r.mode);
    TEST_ASSERT_EQUAL_STRING("SCAN", r.display.ut_fontMgr().segment7.lastPrinted);
    TEST_ASSERT_EQUAL(BAND_NONE, r.display.ut_elementCaches().bands.lastMask);
}

void test_disconnected_refresh_cadence_survives_millis_rollover() {
    DriveRuntime r;
    r.heartbeat(UINT32_MAX - 100, 2);
    r.loop(UINT32_MAX - 99);
    r.clearPaint();
    r.heartbeat(UINT32_MAX - 50, 3);
    r.loop(149); // 249 ms since the last scheduled refresh.
    r.assertNoTransfer();
    r.loop(150);
    TEST_ASSERT_EQUAL(3, r.display.ut_elementCaches().alp.lastHbByte1);
    TEST_ASSERT_EQUAL_INT(1, r.display.testCanvas()->getFlushCount());
    TEST_ASSERT_TRUE(regionTransfers.empty());
}



// Raw I2C reports pass through the production touch reader, gesture recognizer,
// profile admission, coordinator, owner pipeline and painter. Storage admission
// is scripted; panel transfer requests are observed, not physical panel pixels.
void cycleProfileWithRawReleases(DriveRuntime& r, uint32_t start, uint8_t idleFill) {
    r.queueTouchReport(300, 80); r.loop(start);
    r.queueReleaseReport(idleFill); r.loop(start + 50);
    r.queueTouchReport(300, 80); r.loop(start + 250);
    r.queueReleaseReport(idleFill); r.loop(start + 300);
    TEST_ASSERT_EQUAL_INT(0, r.settings.setActiveSlotCalls);
    r.clearPaint();
    r.queueTouchReport(300, 80); r.loop(start + 500);
}
void assertSelectedProfileReachesPanel(DriveRuntime& r) {
    TEST_ASSERT_EQUAL_INT(1, r.settings.setActiveSlotCalls);
    TEST_ASSERT_EQUAL_INT(1, r.settings.get().activeSlot);
    TEST_ASSERT_EQUAL_INT(1, r.display.ut_elementCaches().profile.lastSlot);
    TEST_ASSERT_EQUAL_STRING("HIGHWAY", r.display.ut_elementCaches().profile.lastName);
    const auto profile = DisplayLayout::profileRect();
    bool transferred = r.display.testCanvas()->getFlushCount() > 0;
    for (const auto& region : regionTransfers) {
        if (region.x <= profile.x && region.y <= profile.y &&
            region.x + region.w >= profile.x + profile.w &&
            region.y + region.h >= profile.y + profile.h) transferred = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(transferred, "selected profile never reached the panel transfer boundary");
}
void test_raw_idle_reports_allow_three_taps_to_update_disconnected_scan() {
    for (uint8_t idleFill : {uint8_t{0}, uint8_t{0x38}}) {
        DriveRuntime r;
        r.loop(250);
        r.settings.mutableSettings().autoPushEnabled = true;
        TEST_ASSERT_EQUAL_STRING("SCAN", r.display.ut_fontMgr().segment7.lastPrinted);
        cycleProfileWithRawReleases(r, 1000, idleFill);
        assertSelectedProfileReachesPanel(r);
        TEST_ASSERT_FALSE(r.ble.isConnected());
        TEST_ASSERT_EQUAL_INT(0, r.settingsOperations_.starts);
        TEST_ASSERT_EQUAL(DisplayMode::IDLE, r.mode);
        // The SCAN canvas is retained; changed status must use the panel's
        // reliable full-frame transfer, not redraw or partially blit SCAN.
        TEST_ASSERT_EQUAL_INT(1, r.display.testCanvas()->getFlushCount());
        TEST_ASSERT_EQUAL_INT(0, r.display.testCanvas()->getFillScreenCount());
        TEST_ASSERT_TRUE(regionTransfers.empty());
        r.queueTouchReport(300, 80); r.loop(1750); // A held third contact is not another tap.
        TEST_ASSERT_EQUAL_INT(1, r.settings.setActiveSlotCalls);
    }
}
void test_raw_idle_reports_do_not_bypass_rejected_profile_persistence() {
    DriveRuntime r;
    r.loop(250);
    r.settings.setActiveSlotSuccess = false;
    cycleProfileWithRawReleases(r, 1000, 0x38);
    TEST_ASSERT_EQUAL_INT(1, r.settings.setActiveSlotCalls);
    TEST_ASSERT_EQUAL_INT(0, r.settings.saveDeferredBackupCalls);
    TEST_ASSERT_EQUAL_INT(0, r.settings.get().activeSlot);
    TEST_ASSERT_EQUAL_INT(0, r.display.ut_elementCaches().profile.lastSlot);
    r.assertNoTransfer();
}
void test_raw_idle_reports_keep_persisted_alp_owner_during_profile_cycle() {
    DriveRuntime r(false, 3);
    r.laser();
    r.loop(3211);
    TEST_ASSERT_FALSE(r.alp.currentEvent().active);
    TEST_ASSERT_EQUAL_STRING("PL3", r.display.ut_alpFreqText());
    cycleProfileWithRawReleases(r, 3500, 0x38);
    assertSelectedProfileReachesPanel(r);
    TEST_ASSERT_EQUAL(DisplayMode::LIVE, r.mode);
    TEST_ASSERT_FALSE(r.display.ut_alpHasLaserEvent());
    TEST_ASSERT_TRUE(r.display.ut_alpFreqOverride());
    TEST_ASSERT_EQUAL_STRING("PL3", r.display.ut_alpFreqText());
}
void test_raw_idle_reports_reach_connected_profile_admission_without_early_feedback() {
    DriveRuntime r(true);
    r.loop(250);
    r.settings.mutableSettings().autoPushEnabled = true;
    cycleProfileWithRawReleases(r, 1000, 0x38);
    TEST_ASSERT_EQUAL_INT(1, r.settingsOperations_.starts);
    TEST_ASSERT_EQUAL_INT(0, r.settings.setActiveSlotCalls);
    TEST_ASSERT_EQUAL_INT(0, r.settings.get().activeSlot);
    TEST_ASSERT_EQUAL_INT(0, r.display.ut_elementCaches().profile.lastSlot);
    r.assertNoTransfer();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_disconnected_dli_mode_reaches_panel_without_an_event_or_v1_packet);
    RUN_TEST(test_lid_mode_event_still_updates_immediately);
    RUN_TEST(test_battery_canvas_change_is_transferred_by_the_scheduled_owner_once);
    RUN_TEST(test_unchanged_disconnected_idle_never_transfers);
    RUN_TEST(test_preview_boot_overload_and_settings_gates_defer_status_refresh);
    RUN_TEST(test_live_and_persisted_alp_keep_ownership_until_the_scheduled_expiry);
    RUN_TEST(test_abnormal_listening_hold_and_tail_use_real_deadline_wakes);
    RUN_TEST(test_connected_idle_is_not_repainted_by_the_disconnected_cadence);
    RUN_TEST(test_connection_changes_preserve_live_alp_and_release_to_current_owner);
    RUN_TEST(test_connected_v1_live_owner_survives_idle_ticks_then_disconnect_clears_it);
    RUN_TEST(test_disconnected_refresh_cadence_survives_millis_rollover);
    RUN_TEST(test_raw_idle_reports_allow_three_taps_to_update_disconnected_scan);
    RUN_TEST(test_raw_idle_reports_do_not_bypass_rejected_profile_persistence);
    RUN_TEST(test_raw_idle_reports_keep_persisted_alp_owner_during_profile_cycle);
    RUN_TEST(test_raw_idle_reports_reach_connected_profile_admission_without_early_feedback);
    return UNITY_END();
}
