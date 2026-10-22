#include "test/fixtures/battery_input_lifecycle/battery_io.h"
#include "test/mocks/display.h"
#include "test/mocks/settings.h"
#include "src/modules/power/power_module.cpp"
#include "main_runtime_state.h"
#include <unity.h>

// The real BatteryManager and PowerModule own sampling, warning and cutoff.
// Unrelated maintenance collaborators remain inert but the complete tick runs.
struct RuntimeSettings : SettingsManager {
    bool recoveryPending() const { return false; }
    void serviceDeferredBackup(uint32_t) {}
};
struct RecoveryOwner {
    bool catalogReadable() const { return true; }
    void serviceRecovery(uint32_t) {}
};
struct StorageManager {
    bool isSDCard() const { return false; }
    void* getSDMutex() const { return nullptr; }
    struct SDTryLock {
        SDTryLock(void*, bool) {}
        explicit operator bool() const { return true; }
    };
};
struct MaintenanceWifi {
    bool isConnected() const { return false; }
    String getIPAddress() const { return "192.0.2.1"; }
    String getAPIPAddress() const { return "192.0.2.2"; }
    bool isUiActive(uint32_t) const { return false; }
};
struct MaintenancePreview {
    bool isRunning() const { return false; }
    bool consumeEnded() { return false; }
    void update() {}
};
struct MaintenanceWifiCoordinator {
    template<class T> static void service(T&, uint32_t, bool) {}
};
void audio_process_amp_timeout() {}
constexpr uint32_t kMaintenanceExitLongPressMs = 4000;

struct RuntimePower {
    BatteryManager battery_;
    RuntimeSettings settings_;
    V1Display display_;
    PowerModule power_;
    RuntimePower() {
        assert(battery_.begin());
        settings_.settings.autoPowerOffMinutes = 0;
        power_.begin(&battery_, &display_, &settings_);
    }
};
struct DriveRuntime : RuntimePower {
    void processPower(uint32_t);
    void tick(uint32_t now) { processPower(now); }
};
struct MaintenanceRuntime : RuntimePower {
    RecoveryOwner profiles_, devices_;
    StorageManager storage_;
    MaintenanceWifi wifi_;
    MaintenancePreview preview_;
    MainRuntimeState state_;
    bool active_ = true, idleHeapLogged_ = false, shownStation_ = false;
    bool bootButtonReleaseRequired_ = false, exitRequestFired_ = false;
    uint32_t bootButtonPressStartMs_ = 0;
    uint8_t appliedBrightness_ = 128;
    String shownIp_;
    void servicePowerDisplayOwnership(uint32_t) {}
    void logHeapSnapshot(const char*) {}
    void restartNormal(const char*) { TEST_FAIL_MESSAGE("unexpected maintenance exit"); }
    void tick(uint32_t);
};
#include "runtime_power.inc"

void setUp() { resetBatteryIo(); externallyPowered = false; }
void tearDown() {}

template<class Runtime>
bool tick(Runtime& runtime, uint32_t now) {
    mockMillis = now;
    try { runtime.tick(now); }
    catch (const RailCut&) { return true; }
    return false;
}

enum class Confirmation { Recovered, Low, Unavailable, Zero, WindowLow, WindowRecovered };

template<class Runtime>
void exercise_confirmation(Confirmation confirmation, uint32_t adcElapsed = 1,
                           uint32_t triggerAt = 10000, bool checkGraceBoundary = true) {
    mockMillis = static_cast<uint32_t>(triggerAt - 10000);
    Runtime runtime;
    TEST_ASSERT_EQUAL_UINT32(1, adcReads);
    voltageMv = 3100;
    adcElapsedMs = adcElapsed;
    TEST_ASSERT_FALSE(tick(runtime, triggerAt));
    TEST_ASSERT_EQUAL_UINT32(2, adcReads);
    TEST_ASSERT_EQUAL_UINT16(3100, runtime.battery_.getVoltageMillivolts());
#ifdef CAR_MODE_PWR_SHORT
    TEST_ASSERT_FALSE(runtime.power_.ownsDisplayPresentation());
    TEST_ASSERT_FALSE(tick(runtime, triggerAt + 60000));
    TEST_ASSERT_EQUAL_UINT32(3, adcReads); // Car polling remains active.
    TEST_ASSERT_EQUAL_UINT32(0, latchWrites);
#else
    const uint32_t warningAt = triggerAt + adcElapsed;
    TEST_ASSERT_TRUE(runtime.power_.ownsDisplayPresentation());
    TEST_ASSERT_FALSE(runtime.battery_.hasVoltageReadingAfter(warningAt));
    if (confirmation == Confirmation::Recovered || confirmation == Confirmation::WindowRecovered)
        voltageMv = 3800;
    if (confirmation == Confirmation::Zero) voltageMv = 0;
    if (confirmation == Confirmation::Unavailable) adcReadOk = false;
    if (confirmation == Confirmation::WindowLow || confirmation == Confirmation::WindowRecovered) {
        mockMillis = static_cast<uint32_t>(warningAt + 2000);
        TEST_ASSERT_TRUE(runtime.battery_.refreshVoltage());
        TEST_ASSERT_TRUE(runtime.battery_.hasVoltageReadingAfter(warningAt));
    }
    if (checkGraceBoundary) {
        // The full grace interval starts after acquisition, including across wrap.
        TEST_ASSERT_FALSE_MESSAGE(tick(runtime, warningAt + 5000),
                                  "a trigger sample must not shorten its warning window");
        TEST_ASSERT_EQUAL_UINT32(confirmation == Confirmation::WindowLow ||
                                confirmation == Confirmation::WindowRecovered ? 3 : 2, adcReads);
        TEST_ASSERT_EQUAL(confirmation != Confirmation::WindowRecovered,
                          runtime.power_.ownsDisplayPresentation());
    }
    const bool shouldCut = confirmation == Confirmation::Low || confirmation == Confirmation::WindowLow;
    TEST_ASSERT_EQUAL_MESSAGE(shouldCut, tick(runtime, warningAt + 5001),
                              "shutdown requires a distinct successful low acquisition");
    TEST_ASSERT_EQUAL_UINT32(3, adcReads);
    TEST_ASSERT_EQUAL_UINT32(shouldCut ? 1 : 0, latchWrites);
    if (!shouldCut) {
        TEST_ASSERT_FALSE(runtime.power_.ownsDisplayPresentation());
        TEST_ASSERT_TRUE(runtime.power_.consumeDisplayRestoreRequest());
    }
#endif
}

void test_drive_delayed_trigger_recovers() { exercise_confirmation<DriveRuntime>(Confirmation::Recovered, 1, 10000, false); }
void test_maintenance_delayed_trigger_recovers() { exercise_confirmation<MaintenanceRuntime>(Confirmation::Recovered, 1, 10000, false); }
void test_drive_distinct_low_confirms() { exercise_confirmation<DriveRuntime>(Confirmation::Low); }
void test_maintenance_distinct_low_confirms() { exercise_confirmation<MaintenanceRuntime>(Confirmation::Low); }
void test_drive_unavailable_confirmation_cancels() { exercise_confirmation<DriveRuntime>(Confirmation::Unavailable); }
void test_maintenance_unavailable_confirmation_cancels() { exercise_confirmation<MaintenanceRuntime>(Confirmation::Unavailable); }
void test_drive_zero_confirmation_cancels() { exercise_confirmation<DriveRuntime>(Confirmation::Zero); }
void test_maintenance_zero_confirmation_cancels() { exercise_confirmation<MaintenanceRuntime>(Confirmation::Zero); }
void test_drive_distinct_warning_sample_reused() { exercise_confirmation<DriveRuntime>(Confirmation::WindowLow); }
void test_maintenance_distinct_warning_sample_reused() { exercise_confirmation<MaintenanceRuntime>(Confirmation::WindowLow); }
void test_drive_recovered_warning_sample_cancels() { exercise_confirmation<DriveRuntime>(Confirmation::WindowRecovered); }
void test_maintenance_recovered_warning_sample_cancels() { exercise_confirmation<MaintenanceRuntime>(Confirmation::WindowRecovered); }
void test_drive_same_clock_trigger_recovers() { exercise_confirmation<DriveRuntime>(Confirmation::Recovered, 0); }
void test_maintenance_same_clock_trigger_recovers() { exercise_confirmation<MaintenanceRuntime>(Confirmation::Recovered, 0); }
void test_drive_trigger_crossing_rollover_recovers() { exercise_confirmation<DriveRuntime>(Confirmation::Recovered, 1, UINT32_MAX); }
void test_maintenance_warning_crossing_rollover_confirms() { exercise_confirmation<MaintenanceRuntime>(Confirmation::Low, 1, UINT32_MAX - 2000); }

int main() {
    UNITY_BEGIN();
#ifdef CAR_MODE_PWR_SHORT
    RUN_TEST(test_drive_delayed_trigger_recovers);
    RUN_TEST(test_maintenance_delayed_trigger_recovers);
    RUN_TEST(test_drive_trigger_crossing_rollover_recovers);
    RUN_TEST(test_maintenance_warning_crossing_rollover_confirms);
#else
    RUN_TEST(test_drive_delayed_trigger_recovers);
    RUN_TEST(test_maintenance_delayed_trigger_recovers);
    RUN_TEST(test_drive_distinct_low_confirms);
    RUN_TEST(test_maintenance_distinct_low_confirms);
    RUN_TEST(test_drive_unavailable_confirmation_cancels);
    RUN_TEST(test_maintenance_unavailable_confirmation_cancels);
    RUN_TEST(test_drive_zero_confirmation_cancels);
    RUN_TEST(test_maintenance_zero_confirmation_cancels);
    RUN_TEST(test_drive_distinct_warning_sample_reused);
    RUN_TEST(test_maintenance_distinct_warning_sample_reused);
    RUN_TEST(test_drive_recovered_warning_sample_cancels);
    RUN_TEST(test_maintenance_recovered_warning_sample_cancels);
    RUN_TEST(test_drive_same_clock_trigger_recovers);
    RUN_TEST(test_maintenance_same_clock_trigger_recovers);
    RUN_TEST(test_drive_trigger_crossing_rollover_recovers);
    RUN_TEST(test_maintenance_warning_crossing_rollover_confirms);
#endif
    return UNITY_END();
}
