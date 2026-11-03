#define main original_suite_main
#define setUp autoPushFixtureSetUp
#include "test/test_auto_push_module/test_auto_push_module.cpp"
#undef main
#undef setUp
#include "src/runtime_coordinator.h"
#include "src/v1_settings_operation.cpp"

#include "src/modules/settings_operation/settings_operation_coordinator.cpp"

extern "C" void setUp() {
    autoPushFixtureSetUp();
    // Isolate distinct durable-operation cases; constructors deliberately do
    // not reset NVS because several cases recreate owners over retained jobs.
    mock_preferences::reset();
    mock_nvs::reset();
}

// The actual coordinator, durable store, parser and AutoPush run together.
// These six effects record only the surrounding runtime boundaries.
struct OperationFixture {
    V1SettingsOperationStore settingsOperations_;
    V1DetectorSnapshot captured = makeSnapshot();
    V1DetectorSnapshot persisted;
    int presentedSlot = -1;
    uint32_t executorAtPresentation = UINT32_MAX;
    V1SettingsOperationStore::State stateAtPresentation = V1SettingsOperationStore::State::None;
    SettingsOperationCoordinator::SnapshotPersistResult persistResult =
        SettingsOperationCoordinator::SnapshotPersistResult::Ok;
    String address = "AA:BB:CC:DD:EE:FF";
    int activeSlot = 0;
    unsigned captureCalls = 0, persistCalls = 0, restartCalls = 0;
    uint32_t lastBoundary = 0;
    bool lastFlush = false, acknowledgeOnRestart = false;
    std::vector<std::string> effects;
    SettingsOperationCoordinator coordinator;

    OperationFixture() : coordinator(settingsOperations_, module, ble, {
        this,
        [](String& address, void* context) {
            auto& self = *static_cast<OperationFixture*>(context);
            address = self.address;
            return ble.isConnected();
        },
        [](uint32_t boundary, void* context) {
            auto& self = *static_cast<OperationFixture*>(context);
            ++self.captureCalls;
            self.lastBoundary = boundary;
            self.effects.push_back("capture");
            return self.captured;
        },
        [](const String&, const V1DetectorSnapshot& snapshot, bool flush, void* context) {
            auto& self = *static_cast<OperationFixture*>(context);
            ++self.persistCalls;
            self.lastFlush = flush;
            self.persisted = snapshot;
            self.effects.push_back(flush ? "persist-flush" : "persist-memory");
            return self.persistResult;
        },
        [](void* context) { return static_cast<OperationFixture*>(context)->activeSlot; },
        [](int slot, void* context) {
            auto& self = *static_cast<OperationFixture*>(context);
            self.effects.push_back("present-profile");
            self.presentedSlot = slot;
            self.stateAtPresentation = self.settingsOperations_.snapshot().state;
            self.executorAtPresentation = self.settingsOperations_.snapshot().executorOperationId;
            TEST_ASSERT_NOT_EQUAL_UINT32(0, module.executionSummary().operationId);
        },
        [](void* context) {
            auto& self = *static_cast<OperationFixture*>(context);
            ++self.restartCalls;
            self.effects.push_back("restart");
            return self.acknowledgeOnRestart && self.coordinator.acknowledgePendingMaintenanceReturn();
        },
    }) { addSweepSnapshot(captured); }

    void tick(uint32_t nowMs) { mockMillis = nowMs; coordinator.process(nowMs); }
};

// Production coordinator owns whether the two production owners are serviced.
struct Loop {
    OperationFixture &outer;
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
        outer.tick(now);
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

void stageVolumeApply(OperationFixture &outer) {
    mock_preferences::reset();
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, outer.settingsOperations_.begin());
    configureProfile();
    auto &detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Started,
        outer.settingsOperations_.startApply(0, "AA:BB:CC:DD:EE:FF",
            V1SettingsOperationStore::Source::TripleTap, false, false).status);
    outer.tick(100); // Anchor the real owner before the executor is queued.
    queueAndPreflight(200); // Queue admission has its existing 100ms delay.
    TEST_ASSERT_TRUE(module.isActive());
    const auto pendingVolume = componentStatus(ApplyComponent::Volume);
    TEST_ASSERT_TRUE(pendingVolume.requested);
    TEST_ASSERT_FALSE(pendingVolume.sent);
    TEST_ASSERT_FALSE(pendingVolume.verified);
    TEST_ASSERT_EQUAL_INT(ApplyOutcome::Pending, pendingVolume.outcome);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
    TEST_ASSERT_FALSE(ble.commandInFlight(V1CommandOwner::AutoPush));
    TEST_ASSERT_TRUE(outer.settingsOperations_.markRunning(module.executionSummary().operationId));
}

using Store = V1SettingsOperationStore;
const Store::ComponentSummary &volume(const Store::Snapshot &snapshot) {
    return snapshot.components[static_cast<size_t>(Store::Component::Volume)];
}
void assertDeadlineStopped(OperationFixture &outer, bool sent) {
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
    OperationFixture outer;
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
    OperationFixture outer;
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
    OperationFixture outer;
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
    OperationFixture outer;
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
    OperationFixture outer;
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
    OperationFixture outer;
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, outer.settingsOperations_.begin());
    mockMillis = 0;
    mock_preferences::set_fail_writes(writeFailure);
    outer.coordinator.beginNormalBoot();
    outer.tick(100);
    outer.tick(1000);
    outer.tick(2000);
    TEST_ASSERT_EQUAL_UINT32(0, ble.resetCalls);
    if (writeFailure) {
        TEST_ASSERT_EQUAL_INT(Store::State::Running, outer.settingsOperations_.snapshot().state);
        mock_preferences::set_fail_writes(false);
        outer.tick(2500);
        TEST_ASSERT_EQUAL_INT(Store::State::Running, outer.settingsOperations_.snapshot().state);
        outer.tick(3000);
    }
    TEST_ASSERT_EQUAL_INT(Store::State::Recapturing, outer.settingsOperations_.snapshot().state);
    outer.tick(4000);
    TEST_ASSERT_EQUAL_INT(Store::State::Recapturing, outer.settingsOperations_.snapshot().state);
    TEST_ASSERT_EQUAL_UINT32(0, ble.resetCalls);
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
    OperationFixture outer;
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, outer.settingsOperations_.begin());
    outer.coordinator.beginNormalBoot();
    TEST_ASSERT_EQUAL_INT(Store::StartStatus::Started,
        outer.settingsOperations_.startFactoryReset("AA:BB:CC:DD:EE:FF", false, false).status);
    auto components = outer.settingsOperations_.snapshot().components;
    for (auto& component : components) {
        component.requested = true;
        component.outcome = Store::ComponentOutcome::Pending;
    }
    TEST_ASSERT_TRUE(outer.settingsOperations_.updateComponents(components, 0));
    TEST_ASSERT_TRUE(outer.settingsOperations_.markRunning(0));
    outer.tick(100);
    TEST_ASSERT_EQUAL_UINT32(1, ble.resetCalls);
    outer.tick(200);
    TEST_ASSERT_EQUAL_UINT32(1, ble.resetCalls);
}

void stageFactoryReset(OperationFixture& outer) {
    mock_preferences::reset();
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, outer.settingsOperations_.begin());
    TEST_ASSERT_EQUAL_INT(Store::StartStatus::Started,
        outer.settingsOperations_.startFactoryReset("AA:BB:CC:DD:EE:FF", false, false).status);
    auto components = outer.settingsOperations_.snapshot().components;
    for (auto& component : components) {
        component.requested = true;
        component.outcome = Store::ComponentOutcome::Pending;
    }
    TEST_ASSERT_TRUE(outer.settingsOperations_.updateComponents(components, 0));
    TEST_ASSERT_TRUE(outer.settingsOperations_.markRunning(0));
}

void test_factory_reset_delayed_ack_does_not_latch_or_publish_sent_early() {
    OperationFixture outer;
    stageFactoryReset(outer);
    ble.nextResetResult = SendResult::IN_FLIGHT;
    outer.tick(100);
    TEST_ASSERT_TRUE(ble.commandInFlight(V1CommandOwner::FactoryReset));
    TEST_ASSERT_FALSE(Store::hasDurableFactoryResetSend(outer.settingsOperations_.snapshot()));
    TEST_ASSERT_EQUAL_INT(Store::State::Running, outer.settingsOperations_.snapshot().state);
    outer.tick(150);
    TEST_ASSERT_FALSE(ble.commandInFlight(V1CommandOwner::FactoryReset));
    TEST_ASSERT_TRUE(Store::hasDurableFactoryResetSend(outer.settingsOperations_.snapshot()));
    TEST_ASSERT_EQUAL_INT(Store::State::Recapturing, outer.settingsOperations_.snapshot().state);
    outer.tick(200);
    outer.tick(250);
    TEST_ASSERT_EQUAL_UINT32(2, ble.resetCalls); // One admission, one completion poll.
}

void test_factory_reset_operation_timeout_cancels_unacknowledged_send() {
    OperationFixture outer;
    stageFactoryReset(outer);
    ble.nextResetResult = SendResult::IN_FLIGHT;
    outer.tick(100);
    TEST_ASSERT_TRUE(ble.commandInFlight(V1CommandOwner::FactoryReset));
    outer.tick(90100);
    TEST_ASSERT_FALSE(ble.commandInFlight(V1CommandOwner::FactoryReset));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(V1CommandOwner::FactoryReset),
                          static_cast<int>(ble.lastCancelledCommandOwner));
    TEST_ASSERT_FALSE(Store::hasDurableFactoryResetSend(outer.settingsOperations_.snapshot()));
    TEST_ASSERT_EQUAL_INT(Store::State::Failed, outer.settingsOperations_.snapshot().state);
    outer.tick(90200);
    TEST_ASSERT_EQUAL_UINT32(1, ble.resetCalls);
}

void test_factory_reset_lost_ack_cannot_replay_after_reconnect_or_persistence_failure() {
    for (bool failPersistence : {false, true}) {
        setUp();
        OperationFixture outer;
        stageFactoryReset(outer);
        ble.nextResetResult = SendResult::IN_FLIGHT;
        outer.tick(100);
        TEST_ASSERT_TRUE(ble.commandInFlight(V1CommandOwner::FactoryReset));
        // Model transport timeout/close retiring the worker, then reconnect to
        // the same target. The unknown physical outcome does not permit replay.
        ble.commandPending = false;
        ble.setConnected(false);
        ble.setSessionGeneration(ble.sessionGeneration() + 1);
        outer.tick(150);
        ble.setConnected(true);
        mock_preferences::set_fail_writes(failPersistence);
        outer.tick(200);
        outer.tick(250);
        TEST_ASSERT_EQUAL_UINT32(1, ble.resetCalls);
            TEST_ASSERT_FALSE(Store::hasDurableFactoryResetSend(outer.settingsOperations_.snapshot()));
        mock_preferences::set_fail_writes(false);
        outer.tick(300);
        TEST_ASSERT_EQUAL_UINT32(1, ble.resetCalls);
        TEST_ASSERT_EQUAL_INT(Store::State::Failed, outer.settingsOperations_.snapshot().state);
        TEST_ASSERT_EQUAL_INT(Store::Reason::FactoryResetScopeUnverified,
                              outer.settingsOperations_.snapshot().reason);
    }
}

void prepareConnectionVolumeProfile() {
    configureProfile();
    auto& detector = profiles.loadableProfile.detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.displayPolicy = V1DisplayPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Unchanged;
    stageSnapshot(makeSnapshot());
}

void test_unreadable_pending_job_blocks_connection_apply_without_losing_alerts_then_recovers() {
    mock_preferences::reset();
    mock_nvs::reset();
    Store writer;
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, writer.begin());
    const auto pending = writer.startApply(2, "AA:BB:CC:DD:EE:FF",
        Store::Source::MaintenanceUi, true, true);
    TEST_ASSERT_EQUAL_INT(Store::StartStatus::Started, pending.status);
    const auto saved = mock_preferences::getBlob(Store::namespaceForTest(), Store::recordKeyForTest());
    mock_preferences::set_scalar_read_fault(Store::namespaceForTest(), Store::recordKeyForTest(),
                                            PT_BLOB, ESP_FAIL, 1);
    OperationFixture outer;
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Unavailable, outer.settingsOperations_.begin());
    outer.coordinator.beginNormalBoot();
    prepareConnectionVolumeProfile();
    TEST_ASSERT_EQUAL_INT(SettingsOperationCoordinator::StableAdmission::OwnedOrBlocked,
                          outer.coordinator.onStableConnection());
    TEST_ASSERT_EQUAL_INT(SettingsOperationCoordinator::StableAdmission::OwnedOrBlocked,
                          outer.coordinator.onStableConnection());
    outer.tick(1000);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_TRUE(ble.isConnected());
    TEST_ASSERT_EQUAL_UINT(0, ble.disconnectCalls);
    observeFadeAlert(true, 1500);
    TEST_ASSERT_EQUAL_UINT32(1, parser.getAlertCount());
    TEST_ASSERT_TRUE(saved == mock_preferences::getBlob(Store::namespaceForTest(), Store::recordKeyForTest()));

    // A later boot/reinitialization rereads the preserved job; it cannot create
    // another identity or let ordinary connection AutoPush race that operation.
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, outer.settingsOperations_.begin());
    outer.coordinator.beginNormalBoot();
    TEST_ASSERT_EQUAL_UINT32(pending.operationId, outer.settingsOperations_.snapshot().operationId);
    TEST_ASSERT_EQUAL_INT(2, outer.settingsOperations_.snapshot().slot);
    TEST_ASSERT_EQUAL_INT(Store::State::WaitingForDetector, outer.settingsOperations_.snapshot().state);
    TEST_ASSERT_EQUAL_INT(SettingsOperationCoordinator::StableAdmission::OwnedOrBlocked,
                          outer.coordinator.onStableConnection());
    TEST_ASSERT_EQUAL_INT(Store::State::WaitingForDetector, outer.settingsOperations_.snapshot().state);
    TEST_ASSERT_EQUAL_INT(0, ble.setVolumeCalls);
    TEST_ASSERT_TRUE(ble.isConnected());
    TEST_ASSERT_EQUAL_UINT(0, ble.disconnectCalls);
}

void test_known_empty_job_still_allows_connection_autopush() {
    mock_preferences::reset();
    mock_nvs::reset();
    OperationFixture outer;
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, outer.settingsOperations_.begin());
    TEST_ASSERT_FALSE(outer.settingsOperations_.snapshot().available);
    outer.coordinator.beginNormalBoot();
    prepareConnectionVolumeProfile();
    TEST_ASSERT_EQUAL_INT(SettingsOperationCoordinator::StableAdmission::AllowOrdinaryAutoPush,
                          outer.coordinator.onStableConnection());
    queueAndPreflight();
    at(1000);
    TEST_ASSERT_EQUAL_INT(1, ble.setVolumeCalls);
    TEST_ASSERT_TRUE(ble.isConnected());
    TEST_ASSERT_EQUAL_UINT(0, ble.disconnectCalls);
}

void stageRecapture(OperationFixture& outer, uint32_t now = 100) {
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, outer.settingsOperations_.begin());
    TEST_ASSERT_EQUAL_INT(Store::StartStatus::Started,
        outer.settingsOperations_.startApply(0, "AA:BB:CC:DD:EE:FF",
            Store::Source::MaintenanceUi, false, false).status);
    TEST_ASSERT_TRUE(outer.settingsOperations_.markRunning(0));
    TEST_ASSERT_TRUE(outer.settingsOperations_.markRecapturing(Store::Reason::None));
    outer.tick(now);
    TEST_ASSERT_EQUAL_UINT(1, ble.settingsRecaptureCalls);
    TEST_ASSERT_EQUAL_INT(SettingsOperationCoordinator::StableAdmission::OwnedOrBlocked,
                          outer.coordinator.onStableConnection());
}

void test_recapture_persist_effect_distinguishes_record_and_flush_failures() {
    using Result = SettingsOperationCoordinator::SnapshotPersistResult;
    for (const auto result : {Result::StoreUnavailable, Result::FlushFailed, Result::Ok}) {
        setUp();
        OperationFixture outer;
        stageRecapture(outer);
        outer.persistResult = result;
        outer.tick(101);
        TEST_ASSERT_EQUAL_UINT(1, outer.persistCalls);
        TEST_ASSERT_TRUE(outer.lastFlush);
        const auto expected = result == Result::StoreUnavailable ? Store::Reason::SnapshotStoreUnavailable
            : result == Result::FlushFailed ? Store::Reason::SnapshotPersistFailed : Store::Reason::None;
        TEST_ASSERT_EQUAL_INT(expected, outer.settingsOperations_.snapshot().reason);
        TEST_ASSERT_EQUAL_INT(result == Result::Ok ? Store::State::Succeeded : Store::State::Failed,
                              outer.settingsOperations_.snapshot().state);
        Store reboot;
        TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, reboot.begin());
        TEST_ASSERT_EQUAL_INT(expected, reboot.snapshot().reason);
    }
}

void test_recapture_incomplete_wait_is_five_seconds_and_does_not_extend_on_callback() {
    for (uint32_t now : {100u, UINT32_MAX - 1000u}) {
        setUp();
        OperationFixture outer;
        stageRecapture(outer, now);
        outer.captured.hasUserBytes = false;
        outer.tick(now + 4999u);
        TEST_ASSERT_EQUAL_UINT(0, outer.persistCalls);
        TEST_ASSERT_EQUAL_INT(SettingsOperationCoordinator::StableAdmission::OwnedOrBlocked,
                              outer.coordinator.onStableConnection());
        outer.tick(now + 5000u);
        TEST_ASSERT_EQUAL_UINT(1, outer.persistCalls);
        TEST_ASSERT_EQUAL_INT(Store::State::Partial, outer.settingsOperations_.snapshot().state);
        TEST_ASSERT_EQUAL_INT(Store::Reason::RecaptureTimedOut, outer.settingsOperations_.snapshot().reason);
        TEST_ASSERT_TRUE(outer.persisted.captureTimedOut);
    }
}

void test_wrong_target_disconnects_once_without_snapshot_or_apply() {
    OperationFixture outer;
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, outer.settingsOperations_.begin());
    TEST_ASSERT_EQUAL_INT(Store::StartStatus::Started,
        outer.settingsOperations_.startApply(0, "11:22:33:44:55:66",
            Store::Source::MaintenanceUi, false, false).status);
    outer.tick(100);
    TEST_ASSERT_EQUAL_INT(SettingsOperationCoordinator::StableAdmission::OwnedOrBlocked,
                          outer.coordinator.onStableConnection());
    TEST_ASSERT_TRUE(ble.isConnected());
    outer.tick(101);
    outer.tick(102);
    TEST_ASSERT_EQUAL_UINT(1, ble.disconnectCalls);
    TEST_ASSERT_FALSE(ble.isConnected());
    TEST_ASSERT_EQUAL_UINT(0, outer.captureCalls);
    TEST_ASSERT_EQUAL_UINT(0, ble.setVolumeCalls);
}

void test_wait_deadline_boundary_and_wrap_preserve_failure_reason() {
    for (uint32_t now : {100u, UINT32_MAX - 1000u}) {
        setUp();
        OperationFixture outer;
        TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, outer.settingsOperations_.begin());
        TEST_ASSERT_EQUAL_INT(Store::StartStatus::Started,
            outer.settingsOperations_.startApply(0, "AA:BB:CC:DD:EE:FF",
                Store::Source::MaintenanceUi, false, false).status);
        outer.tick(now);
        outer.tick(now + 59999u);
        TEST_ASSERT_EQUAL_INT(Store::State::WaitingForDetector, outer.settingsOperations_.snapshot().state);
        outer.tick(now + 60000u);
        TEST_ASSERT_EQUAL_INT(Store::State::Failed, outer.settingsOperations_.snapshot().state);
        TEST_ASSERT_EQUAL_INT(Store::Reason::DetectorTimeout, outer.settingsOperations_.snapshot().reason);
    }
}

void test_terminal_retry_defers_before_synchronous_ack_and_does_not_reuse_snapshot() {
    OperationFixture outer;
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, outer.settingsOperations_.begin());
    TEST_ASSERT_EQUAL_INT(Store::StartStatus::Started,
        outer.settingsOperations_.startApply(0, "AA:BB:CC:DD:EE:FF",
            Store::Source::MaintenanceUi, false, true).status);
    TEST_ASSERT_TRUE(outer.coordinator.acknowledgePendingMaintenanceReturn());
    TEST_ASSERT_TRUE(outer.settingsOperations_.snapshot().returnToMaintenance);
    TEST_ASSERT_TRUE(outer.settingsOperations_.finish(Store::State::Failed, Store::Reason::DetectorTimeout));
    outer.tick(100);
    TEST_ASSERT_EQUAL_UINT(1, outer.restartCalls);
    TEST_ASSERT_TRUE(outer.settingsOperations_.snapshot().returnToMaintenance);
    outer.tick(2099);
    TEST_ASSERT_EQUAL_UINT(1, outer.restartCalls);
    outer.acknowledgeOnRestart = true;
    outer.tick(2100);
    TEST_ASSERT_EQUAL_UINT(2, outer.restartCalls);
    TEST_ASSERT_FALSE(outer.settingsOperations_.snapshot().returnToMaintenance);
    outer.tick(4100);
    TEST_ASSERT_EQUAL_UINT(2, outer.restartCalls);
    TEST_ASSERT_TRUE(outer.coordinator.acknowledgePendingMaintenanceReturn());
}

void test_terminal_ack_failure_preserves_return_intent() {
    OperationFixture outer;
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, outer.settingsOperations_.begin());
    TEST_ASSERT_EQUAL_INT(Store::StartStatus::Started,
        outer.settingsOperations_.startApply(0, "AA:BB:CC:DD:EE:FF",
            Store::Source::MaintenanceUi, false, true).status);
    TEST_ASSERT_TRUE(outer.settingsOperations_.finish(Store::State::Failed, Store::Reason::DetectorTimeout));
    mock_preferences::set_fail_writes(true);
    TEST_ASSERT_FALSE(outer.coordinator.acknowledgePendingMaintenanceReturn());
    TEST_ASSERT_TRUE(outer.settingsOperations_.snapshot().returnToMaintenance);
    TEST_ASSERT_EQUAL_UINT(0, outer.restartCalls);
    mock_preferences::set_fail_writes(false);
    TEST_ASSERT_TRUE(outer.coordinator.acknowledgePendingMaintenanceReturn());
    TEST_ASSERT_FALSE(outer.settingsOperations_.snapshot().returnToMaintenance);
}

void test_connected_slot_queue_presents_only_after_acceptance_before_summary_persist() {
    OperationFixture outer;
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, outer.settingsOperations_.begin());
    configureProfile();
    stageSnapshot(outer.captured);
    TEST_ASSERT_TRUE(outer.coordinator.beginSlotApply(0));
    TEST_ASSERT_EQUAL_INT(Store::State::Preparing, outer.settingsOperations_.snapshot().state);
    TEST_ASSERT_EQUAL_INT(-1, outer.presentedSlot);
    outer.tick(100);
    TEST_ASSERT_EQUAL_UINT(1, ble.settingsRecaptureCalls);
    TEST_ASSERT_EQUAL_INT(SettingsOperationCoordinator::StableAdmission::OwnedOrBlocked,
                          outer.coordinator.onStableConnection());
    outer.tick(101);
    TEST_ASSERT_TRUE((outer.effects == std::vector<std::string>{"capture", "persist-memory", "present-profile"}));
    TEST_ASSERT_EQUAL_INT(0, outer.presentedSlot);
    TEST_ASSERT_EQUAL_INT(Store::State::Running, outer.stateAtPresentation);
    TEST_ASSERT_EQUAL_UINT32(0, outer.executorAtPresentation);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, outer.settingsOperations_.snapshot().executorOperationId);
    TEST_ASSERT_FALSE(outer.lastFlush);
}

void test_failed_preapply_snapshot_never_queues_or_presents() {
    OperationFixture outer;
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, outer.settingsOperations_.begin());
    configureProfile();
    stageSnapshot(outer.captured);
    TEST_ASSERT_TRUE(outer.coordinator.beginSlotApply(0));
    outer.tick(100);
    (void)outer.coordinator.onStableConnection();
    outer.persistResult = SettingsOperationCoordinator::SnapshotPersistResult::StoreUnavailable;
    outer.tick(101);
    TEST_ASSERT_EQUAL_INT(Store::State::Failed, outer.settingsOperations_.snapshot().state);
    TEST_ASSERT_EQUAL_INT(Store::Reason::SnapshotStoreUnavailable, outer.settingsOperations_.snapshot().reason);
    TEST_ASSERT_FALSE(module.isActive());
    TEST_ASSERT_EQUAL_INT(-1, outer.presentedSlot);
    TEST_ASSERT_EQUAL_UINT(0, ble.writeUserBytesCalls);
}

void test_session_change_restarts_capture_and_discards_old_completion() {
    OperationFixture outer;
    TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, outer.settingsOperations_.begin());
    configureProfile();
    stageSnapshot(outer.captured);
    TEST_ASSERT_TRUE(outer.coordinator.beginSlotApply(0));
    outer.tick(100);
    (void)outer.coordinator.onStableConnection();
    ble.setSessionGeneration(ble.sessionGeneration() + 1u);
    const uint32_t nextBoundary = ble.noteV1NotificationIngress();
    outer.tick(101);
    TEST_ASSERT_EQUAL_UINT(2, ble.settingsRecaptureCalls);
    TEST_ASSERT_EQUAL_UINT(0, outer.captureCalls);
    TEST_ASSERT_FALSE(module.isActive());
    outer.captured.sessionGeneration = ble.sessionGeneration();
    (void)outer.coordinator.onStableConnection();
    outer.tick(102);
    TEST_ASSERT_EQUAL_UINT32(nextBoundary, outer.lastBoundary);
    TEST_ASSERT_EQUAL_UINT(1, outer.captureCalls);
    TEST_ASSERT_EQUAL_INT(0, outer.presentedSlot);
}

void test_factory_not_yet_retains_fifty_millisecond_retry_pacing() {
    OperationFixture outer;
    stageFactoryReset(outer);
    ble.nextResetResult = SendResult::NOT_YET;
    outer.tick(100);
    outer.tick(149);
    TEST_ASSERT_EQUAL_UINT(1, ble.resetCalls);
    TEST_ASSERT_FALSE(Store::hasDurableFactoryResetSend(outer.settingsOperations_.snapshot()));
    outer.tick(150);
    TEST_ASSERT_EQUAL_UINT(2, ble.resetCalls);
    TEST_ASSERT_TRUE(Store::hasDurableFactoryResetSend(outer.settingsOperations_.snapshot()));
}

void test_boot_recovery_retry_boundary_survives_clock_wrap() {
    for (uint32_t now : {100u, UINT32_MAX - 1000u}) {
        setUp();
        seedInterruptedFactoryReset(true);
        OperationFixture outer;
        TEST_ASSERT_EQUAL_INT(Store::LoadStatus::Ready, outer.settingsOperations_.begin());
        mockMillis = now;
        mock_preferences::set_fail_writes(true);
        outer.coordinator.beginNormalBoot();
        mock_preferences::set_fail_writes(false);
        outer.tick(now + 999u);
        TEST_ASSERT_EQUAL_INT(Store::State::Running, outer.settingsOperations_.snapshot().state);
        outer.tick(now + 1000u);
        TEST_ASSERT_EQUAL_INT(Store::State::Recapturing, outer.settingsOperations_.snapshot().state);
        TEST_ASSERT_EQUAL_UINT(0, ble.resetCalls);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_connected_slot_queue_presents_only_after_acceptance_before_summary_persist);
    RUN_TEST(test_failed_preapply_snapshot_never_queues_or_presents);
    RUN_TEST(test_session_change_restarts_capture_and_discards_old_completion);
    RUN_TEST(test_factory_not_yet_retains_fifty_millisecond_retry_pacing);
    RUN_TEST(test_boot_recovery_retry_boundary_survives_clock_wrap);

    RUN_TEST(test_recapture_persist_effect_distinguishes_record_and_flush_failures);
    RUN_TEST(test_recapture_incomplete_wait_is_five_seconds_and_does_not_extend_on_callback);
    RUN_TEST(test_wrong_target_disconnects_once_without_snapshot_or_apply);
    RUN_TEST(test_wait_deadline_boundary_and_wrap_preserve_failure_reason);
    RUN_TEST(test_terminal_retry_defers_before_synchronous_ack_and_does_not_reuse_snapshot);
    RUN_TEST(test_terminal_ack_failure_preserves_return_intent);

    RUN_TEST(test_unreadable_pending_job_blocks_connection_apply_without_losing_alerts_then_recovers);
    RUN_TEST(test_known_empty_job_still_allows_connection_autopush);
    RUN_TEST(test_factory_reset_lost_ack_cannot_replay_after_reconnect_or_persistence_failure);
    RUN_TEST(test_factory_reset_delayed_ack_does_not_latch_or_publish_sent_early);
    RUN_TEST(test_factory_reset_operation_timeout_cancels_unacknowledged_send);
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
