#define main settings_persistence_suite_main
#include "test/test_settings_persistence/test_settings_persistence.cpp"
#undef main
#include "test/mocks/battery_manager.h"

#include "src/modules/power/power_module.cpp"
#include "src/runtime_coordinator.h"
#include "src/storage_json_rollback.cpp"
#include "src/v1_devices.cpp"

// Writer/health adapters report the actual shutdown helper's bounded join
// outcome. Filesystem, settings, device store and power/lifecycle owners are
// real.
struct ShutdownEvents {
    bool released = true;
    unsigned stops = 0;
    bool stopAndFlush(uint32_t, uint32_t) {
        ++stops;
        return released;
    }
};
struct ShutdownHealth {
    unsigned ends = 0;
    bool failDeviceWrites = false;
    void end(uint32_t) {
        ++ends;
        if (failDeviceWrites)
            fs::mock_set_fs_write_budget(0);
    }
};
void feedLoopTaskWatchdogDuringShutdown() {}
void shutdownBleBondBackupWriter(uint32_t) {}
#include "shutdown_persistence.inc"

class DriveRuntime : public PowerLifecycle {
  public:
    SettingsManager &settings_;
    StorageManager &storage_ = storage;
    V1DeviceStore devices_;
    ShutdownEvents events_;
    ShutdownHealth health_;
    bool clean = false;
    unsigned transportStops = 0;
    explicit DriveRuntime(SettingsManager &settingsOwner)
        : settings_(settingsOwner) {}
    bool preparePersistenceForShutdownPhase();
    void prepareForShutdown() override {
        RuntimeServiceLifecycleCoordinator::prepareDrive(*this);
    }
    void resumeAfterAbortedShutdown() override {}
    void disconnectDriveBleForShutdown() { ++transportStops; }
    void disconnectDriveObdForShutdown() { ++transportStops; }
    void stopDriveBleScanForShutdown() { ++transportStops; }
    void settleDriveShutdownTransport() {}
    void writeCleanShutdownMarker() { clean = true; }
};
#include "shutdown_drive.inc"

void exercise_shutdown(bool sd, bool released, bool lockAvailable,
                       bool writeSucceeds) {
    fs::FS filesystem(g_tempRoot);
    storage.setFilesystem(&filesystem, sd);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager owner(storage, profiles);
    makeCurrentV21Source(owner.mutableSettings());
    TEST_ASSERT_TRUE(owner.save());
    DriveRuntime runtime(owner);
    TEST_ASSERT_TRUE(runtime.devices_.begin(&filesystem));
    TEST_ASSERT_TRUE(runtime.devices_.upsertDevice("AA:BB:CC:DD:EE:01"));
    TEST_ASSERT_TRUE(runtime.devices_.upsertDevice("AA:BB:CC:DD:EE:02"));
    mockMillis = 2000; // Before the first connected persistence window.
    V1DetectorSnapshot snapshot;
    snapshot.available = true;
    snapshot.capturedBootId = 9;
    snapshot.capturedUptimeMs = mockMillis;
    snapshot.sessionGeneration = 3;
    snapshot.captureTimedOut = true;
    snapshot.hasCurrentVolume = true;
    snapshot.currentMainVolume = 6;
    snapshot.currentMutedVolume = 2;
    TEST_ASSERT_TRUE(runtime.devices_.touchDeviceInMemory("AA:BB:CC:DD:EE:01"));
    TEST_ASSERT_TRUE(
        runtime.devices_.recordSnapshotInMemory("AA:BB:CC:DD:EE:01", snapshot));
    TEST_ASSERT_TRUE(runtime.devices_.hasPendingSave());
    runtime.events_.released = released;
    if (!lockAvailable)
        StorageManager::mockSdLockState.failNextTryLockCount = 1;
    runtime.health_.failDeviceWrites = !writeSucceeds;
    BatteryManager battery;
    battery.setBatteryPercent(90);
    V1Display display;
    PowerModule power;
    power.begin(&battery, &display, &owner);
    power.setLifecycle(runtime);
    power.performShutdown();
    TEST_ASSERT_EQUAL_UINT(1, runtime.events_.stops);
    TEST_ASSERT_EQUAL_UINT(3, runtime.transportStops);
    TEST_ASSERT_EQUAL_INT(
        1,
        battery.powerOffCalls); // Flush failure cannot veto physical shutdown.
    const bool committed = released && lockAvailable && writeSucceeds;
    TEST_ASSERT_EQUAL(committed, runtime.clean);
    TEST_ASSERT_EQUAL(!committed, runtime.devices_.hasPendingSave());
    V1DeviceStore reboot;
    TEST_ASSERT_TRUE(reboot.begin(&filesystem));
    V1DeviceRecord reloaded;
    const auto status =
        reboot.getSnapshotForAddressChecked("AA:BB:CC:DD:EE:01", reloaded);
    if (committed) {
        TEST_ASSERT_EQUAL(V1DeviceSnapshotStatus::Found, status);
        TEST_ASSERT_EQUAL_UINT32(9, reloaded.snapshot.capturedBootId);
        TEST_ASSERT_EQUAL_UINT8(6, reloaded.snapshot.currentMainVolume);
        TEST_ASSERT_EQUAL_UINT8(2, reloaded.snapshot.currentMutedVolume);
        TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:01",
                                 reboot.listDevices().front().address.c_str());
        TEST_ASSERT_EQUAL_UINT32(2000, reboot.listDevices().front().lastSeenMs);
    } else {
        TEST_ASSERT_FALSE(reloaded.snapshot.available);
    }
}
void test_shutdown_flushes_pending_snapshot_and_recency_on_sd() {
    exercise_shutdown(true, true, true, true);
}
void test_shutdown_flushes_pending_snapshot_without_sd_lock() {
    exercise_shutdown(false, true, true, true);
}
void test_busy_event_writer_skips_competing_flush_and_clean_marker() {
    exercise_shutdown(true, false, true, true);
}
void test_busy_sd_does_not_veto_shutdown_or_claim_clean() {
    exercise_shutdown(true, true, false, true);
}
void test_failed_device_write_does_not_veto_shutdown_or_claim_clean() {
    exercise_shutdown(true, true, true, false);
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_shutdown_flushes_pending_snapshot_and_recency_on_sd);
    RUN_TEST(test_shutdown_flushes_pending_snapshot_without_sd_lock);
    RUN_TEST(test_busy_event_writer_skips_competing_flush_and_clean_marker);
    RUN_TEST(test_busy_sd_does_not_veto_shutdown_or_claim_clean);
    RUN_TEST(test_failed_device_write_does_not_veto_shutdown_or_claim_clean);
    return UNITY_END();
}
