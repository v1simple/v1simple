// Real OBD transport/state machine and connection coordinator. The settings
// revision, BLE link and peripheral replies are controlled at their boundaries.
#define main original_obd_suite_main
#include "test/test_obd_runtime/test_obd_runtime.cpp"
#undef main

class ObdBleClient {};
void audio_set_volume(uint8_t) {}
enum class BLEState { DISCONNECTED, CONNECTED };

struct RecoverySettings {
    V1Settings value;
    uint32_t revision = 0;
    uint32_t recoveryRevision() const { return revision; }
    const V1Settings& get() const { return value; }
};
struct RecoveryGps {
    void setEnabled(bool) {}
    void begin(bool, uint32_t) {}
};
struct RecoverySync { void begin(RecoverySettings*, ObdRuntimeModule*) {} };
struct RecoverySpeed { void syncEnabledInputs(bool, bool) {} };
struct RecoveryAlp { void begin(bool) {} };
struct RecoveryDisplay {
    void updateColorTheme() {}
    void forceNextRedraw() {}
    void setBrightness(uint8_t) {}
};
struct RecoveryPower { bool ownsDisplayPresentation() const { return false; } };

// These methods model proxy policy only. There is deliberately no V1 reconnect,
// stable-connection callback, settings capture or AutoPush admission surface:
// the extracted recovery and dispatch owners must not invoke those operations.
struct RecoveryBle {
    bool connected = true;
    bool proxyEnabled = true;
    bool proxyAdvertising = false;
    bool phoneConnected = false;
    bool phoneConnectedBefore = false;
    bool verifyMatch = false;
    unsigned phoneDisconnects = 0;
    bool setProxyRuntimeEnabled(bool enabled, const char*) {
        proxyEnabled = enabled;
        if (!enabled) {
            proxyAdvertising = false;
            disconnectPhone();
        }
        return true;
    }
    void disconnectPhone() {
        if (phoneConnected) ++phoneDisconnects;
        phoneConnected = false;
    }
    BLEState getBLEState() const { return connected ? BLEState::CONNECTED : BLEState::DISCONNECTED; }
    bool consumeVerifyPushMatchEdge() {
        const bool result = verifyMatch;
        verifyMatch = false;
        return result;
    }
    uint32_t lastV1ConnectionEventMs() const { return 10; }
    bool isProxyEnabled() const { return proxyEnabled; }
    bool isProxyAdvertising() const { return proxyAdvertising; }
    bool isProxyClientConnected() const { return phoneConnected; }
    bool hasProxyClientConnectedThisBoot() const { return phoneConnectedBefore; }
    void setConnectionCycleProxyPolicy(bool advertising, bool keepConnection) {
        proxyAdvertising = proxyEnabled && advertising;
        if (!keepConnection) disconnectPhone();
    }
    void setObdBleArbitrationRequest(ObdBleArbitrationRequest) {}
    bool isScanning() const { return false; }
    bool isConnectBurstSettling() const { return false; }
    bool isConnectInProgress() const { return false; }
};

class DriveRuntime : public ConnectionCycleLifecycle {
  public:
    RecoverySettings settings_;
    ObdRuntimeModule obd_;
    ObdBleClient obdBle_;
    RecoveryGps gps_;
    RecoverySync obdSettingsSync_;
    RecoverySpeed speed_;
    RecoveryAlp alp_;
    RecoveryDisplay display_;
    RecoveryPower power_;
    RecoveryBle ble_;
    ConnectionCycleCoordinatorModule connectionCycle_;
    struct { bool bootReady = true; } state_;
    uint32_t observedSettingsRecoveryRevision_ = 0;
    bool proxyStopAcknowledged = true;

    DriveRuntime() {
        settings_.value.autoPushEnabled = true;
        obd_.begin(&obdBle_, false, "", 0, -80);
        connectionCycle_.begin(*this);
    }
    void syncRecoveredSettings();
    void processConnectionCycle(uint32_t nowMs, bool connected);
    void processObd(uint32_t nowMs, bool connected);
    void stopObdScan() override { obd_.stopActiveScan(); }
    void cancelObdConnect() override { obd_.cancelPendingConnect(); }
    void stopProxyAdvertising() override { ble_.proxyAdvertising = false; }
    void disconnectProxyPhones() override { ble_.disconnectPhone(); }
    bool isObdScanStopped() const override { return obd_.isScanStopped(); }
    bool isObdConnectIdle() const override { return obd_.isConnectIdle(); }
    bool isProxyFullyStopped() const override { return proxyStopAcknowledged; }
};

#include "connection_recovery.inc"

struct RecoveryFixture {
    DriveRuntime drive;
    uint32_t now = 10;

    void step() {
        mockMillis = ++now;
        auto& obd = drive.obd_;
        if (obd.getState() == ObdConnectionState::CONNECTING &&
            obd.getConnectCallCountForTest() > obd.snapshot(now).connectSuccesses) {
            obd.setTestBleConnected(true);
        }
        if (obd.activeCommand_.active && !obd.activeCommand_.writeResultPending &&
            !obd.bleDataReady_ && !obd.bleEventQueueCount_) {
            const char* reply = obd.activeCommand_.kind == ObdCommandKind::SPEED
                ? "41 0D 32\r>"
                : (strncmp(obd.getLastCommandForTest(), "ATZ", 3) == 0 ? "ELM327 v1.5\r>" : "OK\r>");
            obd.onBleData(reinterpret_cast<const uint8_t*>(reply), strlen(reply));
        }
        drive.processConnectionCycle(now, drive.ble_.connected);
        drive.processObd(now, drive.ble_.connected);
        drive.syncRecoveredSettings();
    }
    void runTo(uint32_t end) { while (now < end) step(); }
    void recover(bool obdEnabled = true, const char* address = kSavedAddress) {
        drive.settings_.value.obdEnabled = obdEnabled;
        drive.settings_.value.proxyBLE = !obdEnabled;
        drive.settings_.value.obdSavedAddress = address;
        ++drive.settings_.revision;
        drive.syncRecoveredSettings();
    }
    void assertConnectedOnce() {
        TEST_ASSERT_EQUAL_UINT32(1, drive.obd_.getConnectCallCountForTest());
        TEST_ASSERT_EQUAL(ObdConnectionState::POLLING, drive.obd_.getState());
        TEST_ASSERT_TRUE(drive.obd_.snapshot(now).speedValid);
        TEST_ASSERT_TRUE(drive.ble_.connected);
    }
};

void test_late_recovery_admits_saved_obd_without_restarting_v1() {
    RecoveryFixture fixture;
    fixture.runTo(20000);
    TEST_ASSERT_EQUAL(CycleState::PROXY_OPEN, fixture.drive.connectionCycle_.state());
    fixture.drive.ble_.phoneConnected = true;
    fixture.drive.ble_.phoneConnectedBefore = true;
    const uint32_t transitions = fixture.drive.connectionCycle_.totalTransitionCount();
    fixture.recover();
    TEST_ASSERT_EQUAL_UINT32(1, fixture.drive.ble_.phoneDisconnects);
    TEST_ASSERT_FALSE(fixture.drive.ble_.proxyEnabled);
    fixture.step();
    const CycleState recoveredPhase = fixture.drive.connectionCycle_.state();
    const uint32_t recoveredTransitions = fixture.drive.connectionCycle_.totalTransitionCount();
    fixture.runTo(30000);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.drive.obd_.getConnectCallCountForTest());
    fixture.runTo(35000);
    fixture.assertConnectedOnce();
    TEST_ASSERT_EQUAL(CycleState::V1_SETTLING, recoveredPhase);
    TEST_ASSERT_GREATER_THAN_UINT32(transitions, recoveredTransitions);
    fixture.runTo(50000); // The unchanged revision must not restart admission.
    fixture.assertConnectedOnce();
}

void test_recovery_after_steady_admits_saved_obd() {
    RecoveryFixture fixture;
    fixture.drive.settings_.value.proxyBLE = false;
    fixture.drive.ble_.proxyEnabled = false;
    fixture.runTo(20000);
    TEST_ASSERT_EQUAL(CycleState::STEADY, fixture.drive.connectionCycle_.state());
    fixture.recover();
    fixture.runTo(35000);
    fixture.assertConnectedOnce();
}

void test_early_recovery_keeps_original_settling_deadline() {
    RecoveryFixture fixture;
    fixture.runTo(1000);
    fixture.recover();
    fixture.runTo(10010);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.drive.obd_.getConnectCallCountForTest());
    fixture.runTo(10012);
    TEST_ASSERT_EQUAL_UINT32(1, fixture.drive.obd_.getConnectCallCountForTest());
    fixture.runTo(14000);
    fixture.assertConnectedOnce();
}

void test_disconnected_recovery_waits_for_v1_before_obd() {
    RecoveryFixture fixture;
    fixture.drive.ble_.connected = false;
    fixture.runTo(20000);
    fixture.recover();
    fixture.runTo(45000);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.drive.obd_.getConnectCallCountForTest());
    TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, fixture.drive.obd_.getState());
    fixture.drive.ble_.connected = true;
    fixture.runTo(60000);
    fixture.assertConnectedOnce();
}

void check_recovery_preserves_teardown(bool alreadyInTeardown) {
    RecoveryFixture fixture;
    fixture.runTo(20000);
    fixture.drive.ble_.connected = false;
    fixture.drive.proxyStopAcknowledged = false;
    if (alreadyInTeardown) fixture.step();
    fixture.recover();
    fixture.step();
    TEST_ASSERT_EQUAL(CycleState::TEARDOWN, fixture.drive.connectionCycle_.state());
    fixture.drive.ble_.connected = true;
    fixture.step();
    fixture.step();
    TEST_ASSERT_EQUAL(CycleState::TEARDOWN, fixture.drive.connectionCycle_.state());
    TEST_ASSERT_EQUAL_UINT32(0, fixture.drive.obd_.getConnectCallCountForTest());
    fixture.drive.proxyStopAcknowledged = true;
    fixture.runTo(35000);
    fixture.assertConnectedOnce();
}

void test_recovery_during_v1_drop_does_not_bypass_teardown() {
    check_recovery_preserves_teardown(false);
}

void test_recovery_during_existing_teardown_keeps_acknowledgement_gate() {
    check_recovery_preserves_teardown(true);
}

void test_proxy_only_recovery_preserves_active_phone_and_does_not_arm_later_toggle() {
    RecoveryFixture fixture;
    fixture.runTo(20000);
    fixture.drive.ble_.phoneConnected = true;
    fixture.drive.ble_.phoneConnectedBefore = true;
    const uint32_t transitions = fixture.drive.connectionCycle_.totalTransitionCount();
    fixture.recover(false);
    fixture.runTo(30000);
    TEST_ASSERT_TRUE(fixture.drive.ble_.phoneConnected);
    TEST_ASSERT_EQUAL_UINT32(0, fixture.drive.ble_.phoneDisconnects);
    TEST_ASSERT_EQUAL_UINT32(transitions, fixture.drive.connectionCycle_.totalTransitionCount());
    TEST_ASSERT_EQUAL_UINT32(0, fixture.drive.obd_.getConnectCallCountForTest());

    // An ordinary user toggle has no recovery revision and retains the existing
    // next-V1-cycle policy, even after a prior recovery request was consumed.
    fixture.drive.settings_.value.obdEnabled = true;
    fixture.drive.settings_.value.proxyBLE = false;
    fixture.drive.obd_.setEnabled(true);
    fixture.drive.ble_.setProxyRuntimeEnabled(false, "V1-Proxy");
    fixture.runTo(180000);
    TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, fixture.drive.obd_.getState());
    TEST_ASSERT_EQUAL_UINT32(0, fixture.drive.obd_.getConnectCallCountForTest());
}

void test_unpaired_recovery_does_not_start_automatic_scan_or_connect() {
    RecoveryFixture fixture;
    fixture.runTo(20000);
    fixture.recover(true, "");
    fixture.runTo(180000);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, fixture.drive.obd_.getState());
    TEST_ASSERT_EQUAL_UINT32(0, fixture.drive.obd_.getConnectCallCountForTest());
    TEST_ASSERT_EQUAL_UINT32(0, fixture.drive.obd_.getStartScanCallCountForTest());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_late_recovery_admits_saved_obd_without_restarting_v1);
    RUN_TEST(test_recovery_after_steady_admits_saved_obd);
    RUN_TEST(test_early_recovery_keeps_original_settling_deadline);
    RUN_TEST(test_disconnected_recovery_waits_for_v1_before_obd);
    RUN_TEST(test_recovery_during_v1_drop_does_not_bypass_teardown);
    RUN_TEST(test_recovery_during_existing_teardown_keeps_acknowledgement_gate);
    RUN_TEST(test_proxy_only_recovery_preserves_active_phone_and_does_not_arm_later_toggle);
    RUN_TEST(test_unpaired_recovery_does_not_start_automatic_scan_or_connect);
    return UNITY_END();
}
