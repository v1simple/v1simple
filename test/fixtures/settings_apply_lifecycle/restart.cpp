#define main original_suite_main
#include "test/test_auto_push_module/test_auto_push_module.cpp"
#undef main
#include "src/v1_settings_operation.cpp"
#include "src/modules/settings_operation/settings_operation_coordinator.cpp"

// Execute the entire actual runtime restart method. Only device checkpoints,
// the one-shot boot flag, final logging and the physical restart are recorded.
namespace {
std::vector<std::string> restartEvents;
bool bootFlag = false, bootWriteSucceeds = true, bootRollbackSucceeds = true;
bool failAckAfterBootFlag = false, loggingSafe = true, lockAvailable = true;
unsigned restartCalls = 0;
V1SettingsOperationStore* restartStore = nullptr;

bool requestMaintenanceBoot() {
    restartEvents.push_back("boot-flag");
    if (!bootWriteSucceeds) return false;
    bootFlag = true;
    if (failAckAfterBootFlag) mock_preferences::set_fail_writes(true);
    return true;
}
bool readAndClearMaintenanceBootRequest() {
    restartEvents.push_back("rollback-boot-flag");
    if (!bootRollbackSucceeds) return false;
    bootFlag = false;
    return true;
}
bool completeLoggingForControlledRestart(int&, int&) {
    restartEvents.push_back("logging");
    // A terminal return must be durably consumed before this phase. An active
    // operation's intent must survive this same USB/touch restart path.
    if (restartStore->isTerminal()) TEST_ASSERT_FALSE(restartStore->snapshot().returnToMaintenance);
    return loggingSafe;
}
void markCleanShutdown() { restartEvents.push_back("clean"); }
void recordRestartDelay(unsigned long duration) {
    TEST_ASSERT_EQUAL_UINT32(50, duration);
    restartEvents.push_back("delay-50");
}
struct RestartProbe {
    void restart() { ++restartCalls; restartEvents.push_back("restart"); }
} restartProbe;
}

class StorageManager {
  public:
    bool sd = false;
    bool isSDCard() const { return sd; }
    int* getSDMutex() { return nullptr; }
    struct SDTryLock {
        SDTryLock(int*, bool checkDmaHeap) {
            TEST_ASSERT_FALSE(checkDmaHeap);
            restartEvents.push_back("sd-lock");
        }
        explicit operator bool() const { return lockAvailable; }
    };
};

class DriveRuntime {
  public:
    struct DeviceCheckpoint {
        bool pending = true, succeeds = true;
        bool hasPendingSave() const { return pending; }
        bool flushPendingSave() {
            TEST_ASSERT_FALSE(bootFlag);
            TEST_ASSERT_TRUE(restartStore->snapshot().returnToMaintenance);
            restartEvents.push_back("snapshot");
            if (succeeds) pending = false;
            return succeeds;
        }
    } devices_;
    struct SettingsCheckpoint {
        bool succeeds = true;
        bool save() { restartEvents.push_back("settings"); return succeeds; }
    } settings_;
    StorageManager storage_;
    int events_ = 0, health_ = 0;
    SettingsOperationCoordinator& settingsOperationCoordinator_;
    explicit DriveRuntime(SettingsOperationCoordinator& coordinator) : settingsOperationCoordinator_(coordinator) {}
    bool requestMaintenanceBootRestart();
};

#define delay recordRestartDelay
#define ESP restartProbe
#include "maintenance_restart.inc"
#undef ESP
#undef delay

struct RestartFixture {
    V1SettingsOperationStore store;
    SettingsOperationCoordinator coordinator;
    DriveRuntime runtime;
    RestartFixture() : coordinator(store, module, ble, {
        this,
        [](String& address, void*) { address = "AA:BB:CC:DD:EE:FF"; return ble.isConnected(); },
        [](uint32_t, void*) { return makeSnapshot(); },
        [](const String&, const V1DetectorSnapshot&, bool, void*) {
            return SettingsOperationCoordinator::SnapshotPersistResult::Ok;
        },
        [](void*) { return 0; },
        [](int, void*) {},
        [](void* context) { return static_cast<RestartFixture*>(context)->runtime.requestMaintenanceBootRestart(); },
    }), runtime(coordinator) {
        restartStore = &store;
        restartEvents.clear();
        restartCalls = 0;
        bootFlag = false;
        bootWriteSucceeds = bootRollbackSucceeds = loggingSafe = lockAvailable = true;
        failAckAfterBootFlag = false;
        mock_preferences::reset();
        TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, store.begin());
        TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Started,
            store.startApply(0, "AA:BB:CC:DD:EE:FF", V1SettingsOperationStore::Source::MaintenanceUi,
                             false, true).status);
    }
    void terminal() {
        TEST_ASSERT_TRUE(store.finish(V1SettingsOperationStore::State::Failed,
                                      V1SettingsOperationStore::Reason::DetectorTimeout));
    }
};

void test_restart_checkpoint_or_boot_failure_cannot_consume_return_intent() {
    for (unsigned fault = 0; fault < 3; ++fault) {
        setUp();
        RestartFixture fixture;
        fixture.terminal();
        if (fault == 0) fixture.runtime.devices_.succeeds = false;
        if (fault == 1) { fixture.runtime.storage_.sd = true; lockAvailable = false; }
        if (fault == 2) bootWriteSucceeds = false;
        TEST_ASSERT_FALSE(fixture.runtime.requestMaintenanceBootRestart());
        TEST_ASSERT_FALSE(bootFlag);
        TEST_ASSERT_TRUE(fixture.store.snapshot().returnToMaintenance);
        TEST_ASSERT_EQUAL_UINT(0, restartCalls);
        TEST_ASSERT_TRUE(std::find(restartEvents.begin(), restartEvents.end(), "logging") == restartEvents.end());
    }
}

void test_restart_ack_failure_rolls_back_boot_flag_or_reports_armed_failure() {
    for (bool rollbackSucceeds : {false, true}) {
        setUp();
        RestartFixture fixture;
        fixture.terminal();
        failAckAfterBootFlag = true;
        bootRollbackSucceeds = rollbackSucceeds;
        TEST_ASSERT_FALSE(fixture.runtime.requestMaintenanceBootRestart());
        TEST_ASSERT_EQUAL(!rollbackSucceeds, bootFlag);
        TEST_ASSERT_TRUE(fixture.store.snapshot().returnToMaintenance);
        TEST_ASSERT_EQUAL_UINT(0, restartCalls);
        TEST_ASSERT_TRUE((restartEvents == std::vector<std::string>{"snapshot", "boot-flag", "rollback-boot-flag"}));
        mock_preferences::set_fail_writes(false);
        V1SettingsOperationStore reboot;
        TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, reboot.begin());
        TEST_ASSERT_TRUE(reboot.snapshot().returnToMaintenance);
    }
}

void test_terminal_service_reenters_only_ack_then_restarts_in_original_order() {
    RestartFixture fixture;
    fixture.terminal();
    fixture.coordinator.process(100);
    TEST_ASSERT_EQUAL_UINT(1, restartCalls);
    TEST_ASSERT_TRUE(bootFlag);
    TEST_ASSERT_FALSE(fixture.store.snapshot().returnToMaintenance);
    TEST_ASSERT_TRUE((restartEvents == std::vector<std::string>{
        "snapshot", "boot-flag", "logging", "settings", "clean", "delay-50", "restart"}));
    fixture.coordinator.process(2100);
    TEST_ASSERT_EQUAL_UINT(1, restartCalls);
}

void test_active_restart_and_final_persistence_failures_keep_original_policy() {
    for (unsigned condition = 0; condition < 3; ++condition) {
        setUp();
        RestartFixture fixture;
        fixture.runtime.devices_.pending = false;
        if (condition == 1) fixture.runtime.settings_.succeeds = false;
        if (condition == 2) loggingSafe = false;
        TEST_ASSERT_TRUE(fixture.runtime.requestMaintenanceBootRestart());
        TEST_ASSERT_TRUE(fixture.store.snapshot().returnToMaintenance);
        TEST_ASSERT_TRUE(fixture.store.isActive());
        TEST_ASSERT_EQUAL_UINT(1, restartCalls);
        TEST_ASSERT_EQUAL(condition == 0,
            std::find(restartEvents.begin(), restartEvents.end(), "clean") != restartEvents.end());
        TEST_ASSERT_EQUAL(condition != 2,
            std::find(restartEvents.begin(), restartEvents.end(), "settings") != restartEvents.end());
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_restart_checkpoint_or_boot_failure_cannot_consume_return_intent);
    RUN_TEST(test_restart_ack_failure_rolls_back_boot_flag_or_reports_armed_failure);
    RUN_TEST(test_terminal_service_reenters_only_ack_then_restarts_in_original_order);
    RUN_TEST(test_active_restart_and_final_persistence_failures_keep_original_policy);
    return UNITY_END();
}
