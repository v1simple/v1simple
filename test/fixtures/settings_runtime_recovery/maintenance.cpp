#define main original_settings_suite_main
#include "test/test_settings_persistence/test_settings_persistence.cpp"
#undef main
#include "main_runtime_state.h"

// Run the actual maintenance tick and HTTP post-restore callback. Only the
// display, WiFi transport, touch and unrelated peripheral owners are modeled.
struct MaintenanceDisplay {
    uint8_t brightness = 0;
    unsigned writes = 0;
    void setBrightness(uint8_t value) { brightness = value; ++writes; }
    void showMaintenanceMode(const char*, bool) {}
};
struct MaintenancePreview {
    bool running = false;
    bool isRunning() const { return running; }
    void update() {}
    bool consumeEnded() { return false; }
};
struct MaintenanceWifi {
    bool isConnected() const { return false; }
    String getIPAddress() const { return "192.0.2.1"; }
    String getAPIPAddress() const { return "192.0.2.2"; }
    bool isUiActive(uint32_t) const { return false; }
};
struct MaintenancePower {
    bool displayOwned = false;
    void process(uint32_t) {}
    bool ownsDisplayPresentation() const { return displayOwned; }
};
struct MaintenanceDevices {
    bool catalogReadable() const { return true; }
    void serviceRecovery(uint32_t) {}
};
struct MaintenanceTouch {
    bool begin(int, int, uint8_t, int) { return true; }
};
struct MaintenanceWifiCoordinator {
    template<class T> static void service(T&, uint32_t, bool) {}
};
void audio_process_amp_timeout() {}
constexpr int BOOT_BUTTON_GPIO = 0;
constexpr uint32_t kMaintenanceExitLongPressMs = 4000;

class MaintenanceRuntime {
public:
    SettingsManager& settings_;
    V1ProfileManager& profiles_ = profiles;
    StorageManager& storage_ = storage;
    MaintenanceDisplay display_;
    MaintenancePreview preview_;
    MaintenanceWifi wifi_;
    MaintenancePower power_;
    MaintenanceDevices devices_;
    MaintenanceTouch touch_;
    MainRuntimeState state_;
    bool active_ = true, idleHeapLogged_ = false, shownStation_ = false;
    // The fixture's digital input stays LOW; exclude button handling entirely.
    bool bootButtonReleaseRequired_ = true, exitRequestFired_ = false;
    uint32_t bootButtonPressStartMs_ = 0;
    uint8_t appliedBrightness_ = 0;
    String shownIp_;
    explicit MaintenanceRuntime(SettingsManager& settings) : settings_(settings) {
        initializeTouchAndDisplayControls();
    }
    void servicePowerDisplayOwnership(uint32_t) {}
    void logHeapSnapshot(const char*) {}
    void restartNormal(const char*) { TEST_FAIL_MESSAGE("unexpected maintenance exit"); }
    void initializeTouchAndDisplayControls();
    void tick(uint32_t);
};
#include "maintenance_recovery.inc"

struct NoDriveBle {
    void setProxyRuntimeEnabled(bool, const char*) { TEST_FAIL_MESSAGE("maintenance touched BLE"); }
};
struct NoDriveRuntime {};
class WiFiManager {
public:
    SettingsManager& settings_;
    NoDriveBle* bleRuntime_ = nullptr;
    NoDriveRuntime* obdRuntime_ = nullptr;
    NoDriveRuntime* speedSelector_ = nullptr;
    explicit WiFiManager(SettingsManager& settings) : settings_(settings) {}
    bool isMaintenanceBootMode() const { return true; }
};
namespace SettingsRuntimeSync {
void syncObdVehicleRuntimeSettings(const V1Settings&, NoDriveRuntime&, NoDriveRuntime&) {
    TEST_FAIL_MESSAGE("maintenance touched drive peripherals");
}
}
#include "maintenance_restore_sync.inc"

static int uploadBackup(SettingsManager& manager, uint8_t brightness) {
    JsonDocument backup;
    backup["_type"] = "v1simple_backup";
    backup["brightness"] = brightness;
    String body;
    serializeJson(backup, body);
    auto runtime = actualBackupRuntime(manager);
    runtime.syncAfterRestore = [](void* ctx) {
        WiFiManager wifi(*static_cast<SettingsManager*>(ctx));
        actualPostRestore(&wifi);
    };
    WebServer upload(80);
    BackupApiService::handleApiRestoreBody(upload, runtime,
        reinterpret_cast<const uint8_t*>(body.c_str()), body.length(),
        nullptr, nullptr, nullptr, nullptr);
    return upload.lastStatusCode;
}

static void saveInitial(SettingsManager& owner, uint8_t brightness) {
    makeCurrentV21Source(owner.mutableSettings());
    owner.mutableSettings().brightness = brightness;
    TEST_ASSERT_TRUE(owner.save());
}

static void exerciseDelayedRecovery(bool sd, bool preview) {
    fs::FS fs(g_tempRoot);
    storage.setFilesystem(&fs, sd);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager saved(storage, profiles);
    saveInitial(saved, 87);
    const String newest = activeNamespaceOrEmpty();
    mock_preferences::set_fail_begin_for_namespace(newest.c_str());
    SettingsManager boot(storage, profiles);
    boot.begin();
    MaintenanceRuntime runtime(boot);
    runtime.preview_.running = preview;
    const uint8_t provisional = runtime.display_.brightness;
    runtime.tick(10000);
    TEST_ASSERT_TRUE(boot.recoveryPending());
    TEST_ASSERT_EQUAL_UINT8(provisional, runtime.display_.brightness);
    TEST_ASSERT_EQUAL_UINT(1, runtime.display_.writes);
    mock_preferences::set_fail_begin_for_namespace(nullptr);
    runtime.tick(20000);
    TEST_ASSERT_FALSE(boot.recoveryPending());
    TEST_ASSERT_EQUAL_UINT8(87, runtime.display_.brightness);
    TEST_ASSERT_EQUAL_UINT(2, runtime.display_.writes);
    runtime.tick(30000);
    TEST_ASSERT_EQUAL_UINT(2, runtime.display_.writes);
    SettingsManager reboot(storage, profiles);
    reboot.begin();
    MaintenanceRuntime next(reboot);
    TEST_ASSERT_EQUAL_UINT8(87, next.display_.brightness);
}
void test_delayed_recovery_without_sd_applies_brightness() { exerciseDelayedRecovery(false, false); }
void test_delayed_recovery_with_sd_applies_brightness_during_preview() { exerciseDelayedRecovery(true, true); }

void test_http_restore_applies_brightness_and_reboots_without_repeated_writes() {
    fs::FS fs(g_tempRoot);
    storage.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager owner(storage, profiles);
    saveInitial(owner, 200);
    MaintenanceRuntime runtime(owner);
    TEST_ASSERT_EQUAL_INT(200, uploadBackup(owner, 87));
    runtime.tick(10000);
    TEST_ASSERT_EQUAL_UINT8(87, runtime.display_.brightness);
    TEST_ASSERT_EQUAL_UINT(2, runtime.display_.writes);
    runtime.tick(20000);
    TEST_ASSERT_EQUAL_UINT(2, runtime.display_.writes);
    SettingsManager reboot(storage, profiles);
    reboot.begin();
    MaintenanceRuntime next(reboot);
    TEST_ASSERT_EQUAL_UINT8(87, next.display_.brightness);
}

void test_failed_restore_does_not_apply_uncommitted_brightness() {
    fs::FS fs(g_tempRoot);
    storage.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager owner(storage, profiles);
    saveInitial(owner, 200);
    MaintenanceRuntime runtime(owner);
    mock_preferences::set_fail_writes_for_key(kNvsBrightness);
    TEST_ASSERT_EQUAL_INT(500, uploadBackup(owner, 87));
    mock_preferences::set_fail_writes_for_key(nullptr);
    runtime.tick(10000);
    TEST_ASSERT_EQUAL_UINT8(200, owner.get().brightness);
    TEST_ASSERT_EQUAL_UINT8(200, runtime.display_.brightness);
    TEST_ASSERT_EQUAL_UINT(1, runtime.display_.writes);
}

void test_restore_waits_for_display_availability_and_uses_latest_value() {
    fs::FS fs(g_tempRoot);
    storage.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager owner(storage, profiles);
    saveInitial(owner, 200);
    MaintenanceRuntime runtime(owner);
    runtime.power_.displayOwned = true;
    TEST_ASSERT_EQUAL_INT(200, uploadBackup(owner, 87));
    runtime.tick(10000);
    TEST_ASSERT_EQUAL_UINT8(200, runtime.display_.brightness);
    TEST_ASSERT_EQUAL_INT(200, uploadBackup(owner, 42));
    runtime.tick(20000);
    TEST_ASSERT_EQUAL_UINT(1, runtime.display_.writes);
    runtime.power_.displayOwned = false;
    runtime.tick(30000);
    TEST_ASSERT_EQUAL_UINT8(42, runtime.display_.brightness);
    TEST_ASSERT_EQUAL_UINT(2, runtime.display_.writes);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_delayed_recovery_without_sd_applies_brightness);
    RUN_TEST(test_delayed_recovery_with_sd_applies_brightness_during_preview);
    RUN_TEST(test_http_restore_applies_brightness_and_reboots_without_repeated_writes);
    RUN_TEST(test_failed_restore_does_not_apply_uncommitted_brightness);
    RUN_TEST(test_restore_waits_for_display_availability_and_uses_latest_value);
    return UNITY_END();
}
