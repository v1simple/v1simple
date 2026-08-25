#include <unity.h>

#include <vector>

#include "../../src/runtime_coordinator.h"

namespace {

enum class Call {
    PREPARE_PERSISTENCE,
    DISCONNECT_BLE,
    DISCONNECT_OBD,
    STOP_SCAN,
    SETTLE_DRIVE,
    STOP_WIFI,
    CLEAN_MARKER,
    RESUME_PERSISTENCE,
    RESUME_BLE_SCAN,
    RESUME_WIFI,
};

struct FakeRuntimeServices {
    bool persistenceSafe = true;
    bool persistenceActive = true;
    bool bleConnected = true;
    bool obdConnected = true;
    bool bleScanActive = true;
    bool wifiActive = true;
    bool cleanMarkerWritten = false;
    std::vector<Call> calls;

    bool preparePersistenceForShutdownPhase() {
        calls.push_back(Call::PREPARE_PERSISTENCE);
        persistenceActive = false;
        return persistenceSafe;
    }

    void disconnectDriveBleForShutdown() {
        calls.push_back(Call::DISCONNECT_BLE);
        bleConnected = false;
    }

    void disconnectDriveObdForShutdown() {
        calls.push_back(Call::DISCONNECT_OBD);
        obdConnected = false;
    }

    void stopDriveBleScanForShutdown() {
        calls.push_back(Call::STOP_SCAN);
        bleScanActive = false;
    }

    void settleDriveShutdownTransport() { calls.push_back(Call::SETTLE_DRIVE); }

    void writeCleanShutdownMarker() {
        calls.push_back(Call::CLEAN_MARKER);
        cleanMarkerWritten = true;
    }

    void resumePersistenceAfterAbortedShutdownPhase() {
        calls.push_back(Call::RESUME_PERSISTENCE);
        persistenceActive = true;
    }

    void resumeDriveBleAfterAbortedShutdown() {
        calls.push_back(Call::RESUME_BLE_SCAN);
        bleScanActive = true;
    }

    bool maintenanceWifiActive() const { return wifiActive; }

    void stopMaintenanceWifiForShutdown() {
        calls.push_back(Call::STOP_WIFI);
        wifiActive = false;
    }

    void resumeMaintenanceWifiAfterAbortedShutdown() {
        calls.push_back(Call::RESUME_WIFI);
        wifiActive = true;
    }
};

void assertCalls(const FakeRuntimeServices& runtime, const std::vector<Call>& expected) {
    TEST_ASSERT_EQUAL_UINT(expected.size(), runtime.calls.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(expected[i]), static_cast<int>(runtime.calls[i]));
    }
}

} // namespace

void setUp() {}
void tearDown() {}

void test_drive_shutdown_prepares_persistence_before_transports_and_marks_clean() {
    FakeRuntimeServices runtime;

    RuntimeServiceLifecycleCoordinator::prepareDrive(runtime);

    assertCalls(runtime,
                {Call::PREPARE_PERSISTENCE, Call::DISCONNECT_BLE, Call::DISCONNECT_OBD,
                 Call::STOP_SCAN, Call::SETTLE_DRIVE, Call::CLEAN_MARKER});
    TEST_ASSERT_FALSE(runtime.persistenceActive);
    TEST_ASSERT_FALSE(runtime.bleConnected);
    TEST_ASSERT_FALSE(runtime.obdConnected);
    TEST_ASSERT_FALSE(runtime.bleScanActive);
    TEST_ASSERT_TRUE(runtime.cleanMarkerWritten);
}

void test_drive_shutdown_abort_restores_persistence_before_ble_scan_service() {
    FakeRuntimeServices runtime;
    RuntimeServiceLifecycleCoordinator::prepareDrive(runtime);
    runtime.calls.clear();

    RuntimeServiceLifecycleCoordinator::resumeDrive(runtime);

    assertCalls(runtime, {Call::RESUME_PERSISTENCE, Call::RESUME_BLE_SCAN});
    TEST_ASSERT_TRUE(runtime.persistenceActive);
    TEST_ASSERT_TRUE(runtime.bleScanActive);
}

void test_maintenance_shutdown_stops_wifi_after_persistence_and_marks_clean() {
    FakeRuntimeServices runtime;

    RuntimeServiceLifecycleCoordinator::prepareMaintenance(runtime);

    assertCalls(runtime, {Call::PREPARE_PERSISTENCE, Call::STOP_WIFI, Call::CLEAN_MARKER});
    TEST_ASSERT_FALSE(runtime.persistenceActive);
    TEST_ASSERT_FALSE(runtime.wifiActive);
    TEST_ASSERT_TRUE(runtime.cleanMarkerWritten);
}

void test_maintenance_shutdown_abort_restores_persistence_before_wifi_service() {
    FakeRuntimeServices runtime;
    RuntimeServiceLifecycleCoordinator::prepareMaintenance(runtime);
    runtime.calls.clear();

    RuntimeServiceLifecycleCoordinator::resumeMaintenance(runtime);

    assertCalls(runtime, {Call::RESUME_PERSISTENCE, Call::RESUME_WIFI});
    TEST_ASSERT_TRUE(runtime.persistenceActive);
    TEST_ASSERT_TRUE(runtime.wifiActive);
}

void test_failed_persistence_release_never_writes_clean_marker() {
    FakeRuntimeServices drive;
    drive.persistenceSafe = false;
    RuntimeServiceLifecycleCoordinator::prepareDrive(drive);
    TEST_ASSERT_FALSE(drive.cleanMarkerWritten);

    FakeRuntimeServices maintenance;
    maintenance.persistenceSafe = false;
    RuntimeServiceLifecycleCoordinator::prepareMaintenance(maintenance);
    TEST_ASSERT_FALSE(maintenance.cleanMarkerWritten);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_drive_shutdown_prepares_persistence_before_transports_and_marks_clean);
    RUN_TEST(test_drive_shutdown_abort_restores_persistence_before_ble_scan_service);
    RUN_TEST(test_maintenance_shutdown_stops_wifi_after_persistence_and_marks_clean);
    RUN_TEST(test_maintenance_shutdown_abort_restores_persistence_before_wifi_service);
    RUN_TEST(test_failed_persistence_release_never_writes_clean_marker);
    return UNITY_END();
}
