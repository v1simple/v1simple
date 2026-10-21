#define main original_suite_main
#include "test/test_auto_push_module/test_auto_push_module.cpp"
#undef main
#include "src/runtime_coordinator.h"
#include "src/v1_settings_operation.cpp"

// Only collaborators outside the path under test are fake. The entire
// processSettingsOperation method below is extracted unchanged at build time.
class DriveRuntime {
  public:
    struct BleAdapter {
        unsigned resetCalls = 0;
        bool isConnected() const { return ble.isConnected(); }
        SendResult factoryResetDetector() { ++resetCalls; return SendResult::SENT; }
        void disconnect() { ble.setConnected(false); }
        uint32_t sessionGeneration() const { return ble.sessionGeneration(); }
        uint32_t latestV1NotificationIngressSequence() const {
            return ble.latestV1NotificationIngressSequence();
        }
        bool beginSettingsRecapture() { return true; }
    } ble_;
    AutoPushModule &autoPush_ = module;
    bool active_ = false;
    struct { bool maintenanceBootActive = true; } state_;
    void start(uint32_t, uint32_t, int);
    V1SettingsOperationStore settingsOperations_;
    bool settingsOperationBootRecoveryPending_ = false;
    uint32_t settingsOperationBootRecoveryAttemptMs_ = 0;
    bool settingsWrongDetectorDisconnectPending_ = false;
    uint32_t observedSettingsOperationId_ = 0;
    V1SettingsOperationStore::State observedSettingsOperationState_ = V1SettingsOperationStore::State::None;
    uint32_t settingsOperationStartedMs_ = 0, settingsOperationNextActionMs_ = 0,
             settingsRecaptureIngressBoundary_ = 0;
    bool settingsRecaptureStarted_ = false;
    V1FreshObservationGate settingsFreshObservationGate_;
    V1DestructiveSendLatch factoryResetSendLatch_;
    bool factoryResetSummaryPersistedThisBoot_ = false;
    std::array<V1SettingsOperationStore::ComponentSummary, V1SettingsOperationStore::kComponentCount>
        factoryResetSentComponents_{};
    V1SettingsOperationPolicy::ReturnToMaintenanceRetry settingsReturnRetry_;
    static constexpr uint32_t kFreshObservationWaitMs = 1500;
    bool requestMaintenanceBootRestart() { return false; }
    bool connectedV1Address(String &address) {
        address = "AA:BB:CC:DD:EE:FF";
        return ble.isConnected();
    }
    V1DetectorSnapshot captureDetectorSnapshot(uint32_t) { return makeSnapshot(); }
    bool settingsSnapshotComplete(const V1DetectorSnapshot &) { return true; }
    bool persistDetectorSnapshot(const String &, const V1DetectorSnapshot &, bool) { return true; }
    void attemptFactoryReset(const V1SettingsOperationStore::Snapshot &);
    void queueSettingsApply(const V1SettingsOperationStore::Snapshot &, const V1DetectorSnapshot &) {}
    void finishSettingsRecapture(const V1SettingsOperationStore::Snapshot &, uint32_t) {}
    void processSettingsOperation(uint32_t nowMs);
    void syncRecoveredSettings() {}
    void processPeriodicMaintenance(uint32_t nowMs, bool, bool, bool, bool);
};
#include "outer_operation.inc"

// Production coordinator owns whether the two production owners are serviced.
struct Loop {
    DriveRuntime &outer;
    bool modal = false;
    bool active() const { return true; }
    DriveLoopTiming beginDriveLoop() { return {0, static_cast<uint32_t>(mockMillis)}; }
    struct Connection {
        bool requestShowInitialScanning = false, connected = true, backpressured = false, overloaded = false;
    };
    Connection processConnectionRuntime(uint32_t) { return {}; }
    void acceptConnectionSnapshot(Connection) {}
    bool powerOwnsPresentation() { return false; }
    void showInitialScanningScreen() {}
    void markInitialScanningScreenHandled() {}
    void presentConnectionState(Connection) {}
    void processPower(uint32_t) {}
    bool processTouch(uint32_t) { return modal; }
    void servicePowerDisplayOwnership(uint32_t) {}
    void processAlp(uint32_t) {}
    bool preemptSettingsForLiveAlert() { return false; }
    void openBootReadyGate(uint32_t) {}
    void processBleRuntime() {}
    void observeAlpProductState(uint32_t) {}
    void processPeriodicMaintenance(uint32_t now, bool connected, bool pressure, bool overloaded,
                                    bool drain) {
        outer.processPeriodicMaintenance(now, connected, pressure, overloaded, drain);
    }
    void finishDriveLoop(bool, uint32_t, bool) {}
    void processTapGesture(uint32_t) {}
    void processBleQueue() {}
    bool bleQueueBackpressured() { return false; }
    void processConnectionCycle(uint32_t, bool) {}
    void processObd(uint32_t, bool) {}
    void processAlpPresentationAndPower(uint32_t) {}
    template <typename Edges> bool processParsedPolicies(const Edges&) { return false; }
    void processGps(uint32_t) {}
    void processSpeed(uint32_t) {}
    void processSpeedAlert(uint32_t) {}
    bool hasLiveAlertPresentation() { return false; }
    void servicePowerAlertPresentation(uint32_t, bool) {}
    bool powerAlertPresentationNeedsRestore() { return false; }
    int consumeDisplayEdges() { return 0; }
    void presentDisplay(int, bool) {}
    DriveLoopDispatch processConnectionDispatch(bool) {
        return {static_cast<uint32_t>(mockMillis), ble.isConnected()};
    }
    void tick(unsigned long now) {
        mockMillis = now;
        DriveLoopCoordinator::tick(*this);
    }
};

void stageVolumeApply(DriveRuntime &outer) {
    mock_preferences::reset();
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, outer.settingsOperations_.begin());
    configureProfile();
    auto &detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    queueAndPreflight();
    TEST_ASSERT_TRUE(statusContains("\"step\":\"VolumeWrite\""));
    TEST_ASSERT_EQUAL_INT(
        V1SettingsOperationStore::StartStatus::Started,
        outer.settingsOperations_
            .startApply(0, "AA:BB:CC:DD:EE:FF", V1SettingsOperationStore::Source::TripleTap, false, false)
            .status);
    TEST_ASSERT_TRUE(outer.settingsOperations_.markRunning(module.executionSummary().operationId));
    outer.processSettingsOperation(100);
}

using Store = V1SettingsOperationStore;
const Store::ComponentSummary &volume(const Store::Snapshot &snapshot) {
    return snapshot.components[static_cast<size_t>(Store::Component::Volume)];
}
void assertDeadlineStopped(DriveRuntime &outer, bool sent) {
    const auto state = sent ? Store::State::Partial : Store::State::Failed;
    TEST_ASSERT_EQUAL_INT(state, outer.settingsOperations_.snapshot().state);
    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_EQUAL(sent, volume(outer.settingsOperations_.snapshot()).sent);
    TEST_ASSERT_EQUAL_INT(Store::ComponentReason::OperationTimeout,
                          volume(outer.settingsOperations_.snapshot()).reason);
    Store reloaded;
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, reloaded.begin());
    TEST_ASSERT_EQUAL_INT(state, reloaded.snapshot().state);
    TEST_ASSERT_EQUAL_INT(Store::ComponentReason::OperationTimeout, volume(reloaded.snapshot()).reason);
}
void test_overdue_modal_cancels_before_any_detector_write() {
    DriveRuntime outer;
    stageVolumeApply(outer);
    Loop loop{outer};
    loop.modal = true;
    loop.tick(90100);
    assertDeadlineStopped(outer, false);
    TEST_ASSERT_EQUAL_UINT32(0, ble.setVolumeCalls);
    loop.modal = false;
    loop.tick(90101);
    loop.tick(90200);
    TEST_ASSERT_EQUAL_UINT32(0, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_INT(Store::State::Failed, outer.settingsOperations_.snapshot().state);
}
void test_modal_services_apply_and_completes_verification() {
    DriveRuntime outer;
    stageVolumeApply(outer);
    Loop loop{outer};
    loop.modal = true;
    loop.tick(1000);
    TEST_ASSERT_EQUAL_UINT32(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_INT(Store::State::Running, outer.settingsOperations_.snapshot().state);
    loop.tick(1030);
    observeAllVolume(7, 3, 4, 1);
    loop.tick(1031);
    TEST_ASSERT_FALSE(module.isActive());
    loop.tick(1032);
    TEST_ASSERT_EQUAL_INT(Store::State::Recapturing, outer.settingsOperations_.snapshot().state);
}
void test_timeout_after_sent_write_preserves_partial_and_releases_volume_owner() {
    DriveRuntime outer;
    stageVolumeApply(outer);
    Loop loop{outer};
    loop.tick(1000);
    TEST_ASSERT_EQUAL_UINT32(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_INT(SendResult::NOT_YET, quiet.sendVolumeResult(QuietOwner::VolumeFade, 1, 1));
    loop.modal = true;
    loop.tick(90100);
    assertDeadlineStopped(outer, true);
    loop.modal = false;
    loop.tick(90200);
    TEST_ASSERT_EQUAL_UINT32(1, ble.setVolumeCalls);
    TEST_ASSERT_EQUAL_INT(SendResult::SENT, quiet.sendVolumeResult(QuietOwner::VolumeFade, 1, 1));
}
void test_persist_failure_cannot_leave_timed_out_executor_running() {
    DriveRuntime outer;
    stageVolumeApply(outer);
    Loop loop{outer};
    loop.modal = true;
    mock_preferences::set_fail_writes(true);
    loop.tick(90100);
    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_EQUAL_INT(Store::State::Running, outer.settingsOperations_.snapshot().state);
    loop.modal = false;
    loop.tick(90200);
    TEST_ASSERT_EQUAL_UINT32(0, ble.setVolumeCalls);
    mock_preferences::set_fail_writes(false);
    loop.tick(90300);
    assertDeadlineStopped(outer, false);
}
void test_disconnect_cancels_without_detector_write() {
    DriveRuntime outer;
    stageVolumeApply(outer);
    Loop loop{outer};
    loop.modal = true;
    ble.setConnected(false);
    loop.tick(1000);
    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_EQUAL_UINT32(0, ble.setVolumeCalls);
}
void seedInterruptedFactoryReset(bool sent) {
    mock_preferences::reset();
    Store previousBoot;
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, previousBoot.begin());
    TEST_ASSERT_EQUAL_INT(Store::StartStatus::Started,
        previousBoot.startFactoryReset("AA:BB:CC:DD:EE:FF", false, false).status);
    auto components = previousBoot.snapshot().components;
    for (auto& component : components) {
        component.requested = true;
        component.sent = sent;
        component.outcome = sent ? Store::ComponentOutcome::Sent : Store::ComponentOutcome::Pending;
    }
    TEST_ASSERT_TRUE(previousBoot.updateComponents(components, 0));
    TEST_ASSERT_TRUE(previousBoot.markRunning(0));
}

void interruptedFactoryResetCannotReplay(bool sent, bool writeFailure) {
    seedInterruptedFactoryReset(sent);
    DriveRuntime outer;
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, outer.settingsOperations_.begin());
    mockMillis = 0;
    mock_preferences::set_fail_writes(writeFailure);
    outer.start(0, 0, 0);
    outer.processSettingsOperation(100);
    outer.processSettingsOperation(1000);
    outer.processSettingsOperation(2000);
    TEST_ASSERT_EQUAL_UINT32(0, outer.ble_.resetCalls);
    if (writeFailure) {
        TEST_ASSERT_EQUAL_INT(Store::State::Running, outer.settingsOperations_.snapshot().state);
        mock_preferences::set_fail_writes(false);
        outer.processSettingsOperation(2500);
        TEST_ASSERT_EQUAL_INT(Store::State::Running, outer.settingsOperations_.snapshot().state);
        outer.processSettingsOperation(3000);
    }
    TEST_ASSERT_EQUAL_INT(Store::State::Recapturing, outer.settingsOperations_.snapshot().state);
    outer.processSettingsOperation(4000);
    TEST_ASSERT_EQUAL_INT(Store::State::Recapturing, outer.settingsOperations_.snapshot().state);
    TEST_ASSERT_EQUAL_UINT32(0, outer.ble_.resetCalls);
    Store reloaded;
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, reloaded.begin());
    TEST_ASSERT_EQUAL_INT(Store::State::Recapturing, reloaded.snapshot().state);
}
void test_sent_factory_reset_never_replayed_after_recovery_write_failure() {
    interruptedFactoryResetCannotReplay(true, true);
}
void test_unknown_factory_reset_never_replayed_after_recovery_write_failure() {
    interruptedFactoryResetCannotReplay(false, true);
}
void test_successful_boot_recovery_only_runs_once() {
    interruptedFactoryResetCannotReplay(true, false);
}
void test_empty_boot_still_allows_a_new_operation() {
    mock_preferences::reset();
    DriveRuntime outer;
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, outer.settingsOperations_.begin());
    outer.start(0, 0, 0);
    TEST_ASSERT_FALSE(outer.settingsOperationBootRecoveryPending_);
    TEST_ASSERT_EQUAL_INT(Store::StartStatus::Started,
        outer.settingsOperations_.startFactoryReset("AA:BB:CC:DD:EE:FF", false, false).status);
    auto components = outer.settingsOperations_.snapshot().components;
    for (auto& component : components) {
        component.requested = true;
        component.outcome = Store::ComponentOutcome::Pending;
    }
    TEST_ASSERT_TRUE(outer.settingsOperations_.updateComponents(components, 0));
    TEST_ASSERT_TRUE(outer.settingsOperations_.markRunning(0));
    outer.processSettingsOperation(100);
    TEST_ASSERT_EQUAL_UINT32(1, outer.ble_.resetCalls);
    outer.processSettingsOperation(200);
    TEST_ASSERT_EQUAL_UINT32(1, outer.ble_.resetCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_overdue_modal_cancels_before_any_detector_write);
    RUN_TEST(test_modal_services_apply_and_completes_verification);
    RUN_TEST(test_timeout_after_sent_write_preserves_partial_and_releases_volume_owner);
    RUN_TEST(test_persist_failure_cannot_leave_timed_out_executor_running);
    RUN_TEST(test_disconnect_cancels_without_detector_write);
    RUN_TEST(test_sent_factory_reset_never_replayed_after_recovery_write_failure);
    RUN_TEST(test_unknown_factory_reset_never_replayed_after_recovery_write_failure);
    RUN_TEST(test_successful_boot_recovery_only_runs_once);
    RUN_TEST(test_empty_boot_still_allows_a_new_operation);
    return UNITY_END();
}
