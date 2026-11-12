#define main original_settings_suite_main
#include "test/test_settings_persistence/test_settings_persistence.cpp"
#undef main
#include "test/mocks/runtime_presentation_source.h"
#include "src/modules/touch/touch_ui_module.h"

#include "test/mocks/battery_manager.h"
#include "src/modules/power/power_module.cpp"

// Reuse the production settings owner and OBD synchronizer from the native
// persistence harness. These collaborators record hardware configuration only.
static uint8_t appliedVoiceVolume = 0;
void audio_set_volume(uint8_t volume) { appliedVoiceVolume = volume; }

struct RecoveryGps {
  bool enabled = false;
  uint32_t baud = 0;
  unsigned beginCalls = 0;
  void setEnabled(bool value) { enabled = value; }
  void begin(bool value, uint32_t rate) {
    enabled = value;
    baud = rate;
    ++beginCalls;
  }
};

struct RecoveryObdTransport {};
struct RecoveryObd : ObdRuntimeModule {
  unsigned beginCalls = 0;
  int8_t minRssi = 0;
  void begin(RecoveryObdTransport *, bool enabled, const char *address,
             uint8_t type, int8_t rssi) {
    ++beginCalls;
    minRssi = rssi;
    ObdRuntimeModule::begin(nullptr, enabled, address, type, rssi);
  }
};

struct RecoverySpeed {
  bool obdEnabled = false, gpsEnabled = false;
  void syncEnabledInputs(bool obd, bool gps) {
    obdEnabled = obd;
    gpsEnabled = gps;
  }
};

struct RecoveryAlp {
  bool enabled = false;
  unsigned beginCalls = 0;
  void begin(bool value) {
    enabled = value;
    ++beginCalls;
  }
};

struct RecoveryDisplay {
  uint8_t brightness = 0;
  unsigned themes = 0, redraws = 0;
  void updateColorTheme(const DisplayStyleView&) { ++themes; }
  void forceNextRedraw() { ++redraws; }
  void setBrightness(uint8_t value) { brightness = value; }
};

struct RecoveryBle {
  bool proxyEnabled = false;
  String proxyName;
  void setProxyRuntimeEnabled(bool enabled, const char *name) {
    proxyEnabled = enabled;
    proxyName = name;
  }
  void serviceDeferredBondBackup(uint32_t) {}
};

struct RecoveryDevices {
  bool catalogReadable() const { return true; }
  void serviceRecovery(uint32_t) {}
};
void processV1DeviceStoreSave(uint32_t, StorageManager &, RecoveryDevices &) {}

class DriveRuntime {
public:
  SettingsManager &settings_;
  RecoveryGps gps_;
  RecoveryObd obd_;
  RecoveryObdTransport obdBle_;
  RecoverySpeed speed_;
  RecoveryAlp alp_;
  RecoveryDisplay display_;
  RecoveryBle ble_;
  struct {
    void requestRecoveredSettingsAdmission() {}
  } connectionCycle_;
  ObdSettingsSyncModule obdSettingsSync_;
  RuntimePresentationSource presentation_{settings_};
  TouchUiModule touchUi_;
  PowerModule power_;
  BatteryManager battery_;
  V1Display powerDisplay_;
  struct {
    void process(uint32_t) {}
  } settingsOperationCoordinator_;
  StorageManager &storage_ = storage;
  V1ProfileManager &profiles_ = profiles;
  RecoveryDevices devices_;
  uint32_t observedSettingsRecoveryRevision_ = 0;
  bool usbTailBusy_ = false, connectedPersistenceWindowAnchored_ = false;
  uint32_t connectedPersistenceWindowStartedMs_ = 0;
  static constexpr uint32_t kConnectedPersistenceDeferralMs = 10000;

  explicit DriveRuntime(SettingsManager &owner) : settings_(owner) {
    battery_.setOnBattery(true);
    battery_.setBatteryPercent(90);
    power_.begin(&battery_, &powerDisplay_, &settings_, presentation_);
    obdSettingsSync_.begin(&settings_, &obd_);
  }
  void syncRecoveredSettings();
  void serviceDeferredConnectionAutoPush() {}
  void processPeriodicMaintenance(uint32_t nowMs, bool connected, bool pressure,
                                  bool overloaded, bool drain);
  void logDisplayConfiguration(uint32_t) {}
};

#include "runtime_recovery.inc"

void exercise_runtime_recovery(bool setterFirst) {
  fs::FS filesystem(g_tempRoot);
  storage.setFilesystem(&filesystem, true);
  TEST_ASSERT_TRUE(profiles.begin(storage));
  SettingsManager source(storage, profiles);
  auto &saved = source.mutableSettings();
  makeCurrentV21Source(saved);
  saved.gpsEnabled = true;
  saved.gpsBaud = 38400;
  saved.obdEnabled = true;
  saved.proxyBLE = false;
  saved.obdSavedAddress = "A4:C1:38:00:33:44";
  saved.obdSavedAddrType = 1;
  saved.obdMinRssi = -75;
  saved.alpEnabled = true;
  saved.voiceVolume = 55;
  saved.brightness = 87;
  TEST_ASSERT_TRUE(source.save());
  const String newest = activeNamespaceOrEmpty();
  mock_preferences::set_fail_begin_for_namespace(newest.c_str());
  SettingsManager boot(storage, profiles);
  boot.begin();
  DriveRuntime runtime(boot);
  TEST_ASSERT_FALSE(boot.get().obdEnabled);
  TEST_ASSERT_FALSE(runtime.obd_.isEnabled());
  mock_preferences::set_fail_begin_for_namespace(nullptr);
  if (setterFirst) {
    TEST_ASSERT_TRUE(
        boot.setStealthEnabled(true, SettingsPersistMode::Deferred).success);
  }
  // Setter-first recovery must apply even when persistence is deferred by
  // input pressure. Service-first recovery must apply in the same tick.
  runtime.processPeriodicMaintenance(10000, false, setterFirst, false, false);
  TEST_ASSERT_EQUAL_UINT32(1, boot.recoveryRevision());
  TEST_ASSERT_TRUE(runtime.gps_.enabled);
  TEST_ASSERT_EQUAL_UINT32(38400, runtime.gps_.baud);
  TEST_ASSERT_TRUE(runtime.obd_.isEnabled());
  TEST_ASSERT_EQUAL_STRING("A4:C1:38:00:33:44", runtime.obd_.getSavedAddress());
  TEST_ASSERT_EQUAL_UINT8(1, runtime.obd_.getSavedAddrType());
  TEST_ASSERT_EQUAL_INT8(-75, runtime.obd_.minRssi);
  TEST_ASSERT_TRUE(runtime.speed_.obdEnabled);
  TEST_ASSERT_TRUE(runtime.speed_.gpsEnabled);
  TEST_ASSERT_TRUE(runtime.alp_.enabled);
  TEST_ASSERT_EQUAL_UINT8(55, appliedVoiceVolume);
  TEST_ASSERT_EQUAL_UINT8(87, runtime.display_.brightness);
  TEST_ASSERT_EQUAL(boot.get().proxyBLE, runtime.ble_.proxyEnabled);
  TEST_ASSERT_EQUAL_STRING(boot.get().proxyName.c_str(),
                           runtime.ble_.proxyName.c_str());
  TEST_ASSERT_FALSE(runtime.obdSettingsSync_.hasPendingSnapshotForTest());

  // Beyond the real synchronizer's five-second window, the provisional
  // empty adapter must not replace the recovered persisted pairing.
  runtime.processPeriodicMaintenance(16000, false, false, false, false);
  runtime.processPeriodicMaintenance(22000, false, false, false, false);
  TEST_ASSERT_EQUAL_STRING("A4:C1:38:00:33:44",
                           boot.get().obdSavedAddress.c_str());
  TEST_ASSERT_EQUAL_UINT(1, runtime.gps_.beginCalls);
  TEST_ASSERT_EQUAL_UINT(1, runtime.obd_.beginCalls);
  TEST_ASSERT_EQUAL_UINT(1, runtime.alp_.beginCalls);
  TEST_ASSERT_EQUAL_UINT(1, runtime.display_.themes);
  TEST_ASSERT_EQUAL_UINT(1, runtime.display_.redraws);
  SettingsManager reboot(storage, profiles);
  reboot.load();
  TEST_ASSERT_EQUAL_STRING("A4:C1:38:00:33:44",
                           reboot.get().obdSavedAddress.c_str());
}

void test_service_recovery_restores_runtime_before_obd_sync() {
  exercise_runtime_recovery(false);
}
void test_setter_recovery_restores_runtime_during_persistence_pressure() {
  exercise_runtime_recovery(true);
}

void test_pending_sd_recovery_cannot_commit_provisional_obd_snapshot() {
  fs::FS filesystem(g_tempRoot);
  storage.setFilesystem(&filesystem, true);
  TEST_ASSERT_TRUE(profiles.begin(storage));
  SettingsManager source(storage, profiles);
  makeCurrentV21Source(source.mutableSettings());
  source.mutableSettings().obdEnabled = true;
  source.mutableSettings().proxyBLE = false;
  source.mutableSettings().obdSavedAddress = "A4:C1:38:00:33:44";
  TEST_ASSERT_TRUE(source.save());
  TEST_ASSERT_TRUE(filesystem.exists(SETTINGS_BACKUP_PATH));
  const String newest = activeNamespaceOrEmpty();
  mock_preferences::set_fail_begin_for_namespace(newest.c_str());
  SettingsManager boot(storage, profiles);
  boot.begin();
  DriveRuntime runtime(boot);
  mock_preferences::set_fail_begin_for_namespace(nullptr);

  // NVS becomes readable first, while SD recovery remains unfinished.
  fs::mock_fail_next_read_open(SETTINGS_BACKUP_PATH);
  TEST_ASSERT_FALSE(boot.setStealthEnabled(true).success);
  TEST_ASSERT_EQUAL_UINT32(0, boot.recoveryRevision());
  TEST_ASSERT_EQUAL_STRING("A4:C1:38:00:33:44",
                           boot.get().obdSavedAddress.c_str());
  runtime.processPeriodicMaintenance(10000, false, true, false, false);
  runtime.processPeriodicMaintenance(16000, false, true, false, false);
  TEST_ASSERT_EQUAL_STRING("A4:C1:38:00:33:44",
                           boot.get().obdSavedAddress.c_str());
  runtime.processPeriodicMaintenance(22000, false, false, false, false);
  TEST_ASSERT_EQUAL_UINT32(1, boot.recoveryRevision());
  TEST_ASSERT_EQUAL_STRING("A4:C1:38:00:33:44", runtime.obd_.getSavedAddress());
  TEST_ASSERT_FALSE(runtime.obdSettingsSync_.hasPendingSnapshotForTest());
}


void test_recovered_auto_off_starts_full_interval_without_new_detector_edge() {
  fs::FS filesystem(g_tempRoot);
  storage.setFilesystem(&filesystem, true);
  TEST_ASSERT_TRUE(profiles.begin(storage));
  SettingsManager source(storage, profiles);
  makeCurrentV21Source(source.mutableSettings());
  source.mutableSettings().autoPowerOffMinutes = 1;
  TEST_ASSERT_TRUE(source.save());
  const String newest = activeNamespaceOrEmpty();
  mock_preferences::set_fail_begin_for_namespace(newest.c_str());
  SettingsManager boot(storage, profiles);
  boot.begin();
  DriveRuntime runtime(boot);
  TEST_ASSERT_TRUE(boot.recoveryPending());
  TEST_ASSERT_EQUAL_UINT8(0, boot.get().autoPowerOffMinutes);
  mockMillis = 500;
  runtime.power_.onV1ConnectionChange(true);
  runtime.power_.onV1DataReceived();
  mockMillis = 1000;
  runtime.power_.onV1ConnectionChange(false);
  TEST_ASSERT_TRUE(runtime.power_.autoPowerOffArmedForTest());
  TEST_ASSERT_FALSE(runtime.power_.autoPowerOffTimerRunningForTest());

  mock_preferences::set_fail_begin_for_namespace(nullptr);
  mockMillis = 10000;
  runtime.processPeriodicMaintenance(mockMillis, false, false, false, false);
  TEST_ASSERT_FALSE(boot.recoveryPending());
  TEST_ASSERT_EQUAL_UINT32(1, runtime.observedSettingsRecoveryRevision_);
  TEST_ASSERT_EQUAL_UINT8(1, boot.get().autoPowerOffMinutes);
  runtime.power_.process();
  TEST_ASSERT_TRUE(runtime.power_.autoPowerOffTimerRunningForTest());
  TEST_ASSERT_EQUAL_UINT32(10000, runtime.power_.autoPowerOffTimerStartForTest());
  mockMillis = 69999;
  runtime.power_.process();
  TEST_ASSERT_EQUAL_INT(0, runtime.battery_.powerOffCalls);
  mockMillis = 70000;
  runtime.power_.process();
  TEST_ASSERT_EQUAL_INT(1, runtime.battery_.powerOffCalls);
}


#include "src/modules/touch/touch_ui_module.cpp"
void play_test_voice() {}

void queueBrightnessTouch(uint16_t x, uint8_t count = 1) {
    std::vector<uint8_t> frame(32, 0);
    frame[1] = count;
    frame[2] = static_cast<uint8_t>(x >> 8);
    frame[3] = static_cast<uint8_t>(x);
    frame[5] = 100;
    Wire.queueRequestFrom(frame.size(), frame);
}

void beginLocalSettingsRecovery() {
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager source(storage, profiles);
    makeCurrentV21Source(source.mutableSettings());
    source.mutableSettings().brightness = 87;
    source.mutableSettings().voiceVolume = 55;
    TEST_ASSERT_TRUE(source.save());
    const String newest = activeNamespaceOrEmpty();
    mock_preferences::set_fail_begin_for_namespace(newest.c_str());
}

void exerciseLocalBrightnessRecovery(bool recoverOnClose, bool liveAlert) {
    fs::FS filesystem(g_tempRoot);
    storage.setFilesystem(&filesystem, true);
    beginLocalSettingsRecovery();
    SettingsManager boot(storage, profiles);
    boot.begin();
    TEST_ASSERT_TRUE(boot.recoveryPending());
    TEST_ASSERT_NOT_EQUAL(55, boot.get().voiceVolume);
    V1Display display;
    display.activeSliderFromTouch = 0;
    RuntimePresentationSource presentation{boot};
    TouchHandler touch;
    TEST_ASSERT_TRUE(touch.begin());
    TouchUiModule ui;
    ui.begin(&display, &touch, &boot, {}, presentation);
    auto tick = [&](uint32_t now, bool held) {
        mockMillis = now;
        return ui.process(now, held);
    };
    tick(1000, true);
    queueBrightnessTouch(0, 0);
    TEST_ASSERT_TRUE(tick(1400, false));
    queueBrightnessTouch(600);
    TEST_ASSERT_TRUE(tick(1505, false));
    TEST_ASSERT_EQUAL_UINT8(80, display.lastSettingsBrightness);
    queueBrightnessTouch(0, 0);
    tick(2100, true);
    if (recoverOnClose) mock_preferences::set_fail_begin_for_namespace(nullptr);
    if (liveAlert) {
        TEST_ASSERT_TRUE(ui.preemptForLiveAlert());
    } else {
        TEST_ASSERT_FALSE(tick(2500, false));
    }
    if (!recoverOnClose) {
        TEST_ASSERT_TRUE(boot.recoveryPending());
        TEST_ASSERT_EQUAL_UINT8(80, boot.localSliderBrightness());
        // A normal reopen shows the pending choice even while NVS is unreadable.
        tick(3100, true);
        queueBrightnessTouch(0, 0);
        TEST_ASSERT_TRUE(tick(3500, false));
        TEST_ASSERT_EQUAL_UINT8(80, display.lastSettingsBrightness);
        TEST_ASSERT_TRUE(ui.preemptForLiveAlert());
        mock_preferences::set_fail_begin_for_namespace(nullptr);
        mockMillis = 10000;
        boot.serviceDeferredPersist(mockMillis);
        mockMillis = 20000;
        boot.serviceDeferredPersist(mockMillis);
    }
    TEST_ASSERT_FALSE(boot.recoveryPending());
    SettingsManager reboot(storage, profiles);
    reboot.load();
    TEST_ASSERT_EQUAL_UINT8(80, reboot.get().brightness);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(55, reboot.get().voiceVolume,
        "Changing brightness persisted a provisional speaker volume over untouched recovered state");
}

void test_local_brightness_adjustment_preserves_untouched_recovered_volume() {
    exerciseLocalBrightnessRecovery(true, false);
}

void test_local_brightness_adjustment_waits_for_unavailable_settings() {
    exerciseLocalBrightnessRecovery(false, false);
}

void test_local_brightness_preemption_defers_recovery_and_persistence() {
    exerciseLocalBrightnessRecovery(false, true);
}

void test_recovery_updates_untouched_sliders_and_preserves_an_open_preview() {
    fs::FS filesystem(g_tempRoot);
    storage.setFilesystem(&filesystem, true);
    beginLocalSettingsRecovery();
    SettingsManager boot(storage, profiles);
    boot.begin();
    TEST_ASSERT_TRUE(boot.recoveryPending());
    TEST_ASSERT_EQUAL_UINT8(75, boot.get().voiceVolume);
    DriveRuntime runtime(boot);
    V1Display display;
    RuntimePresentationSource presentation{boot};
    TouchHandler touch;
    TEST_ASSERT_TRUE(touch.begin());
    runtime.touchUi_.begin(&display, &touch, &boot, {}, presentation);
    auto tick = [&](uint32_t now, bool held) {
        mockMillis = now;
        return runtime.touchUi_.process(now, held);
    };
    tick(1000, true);
    queueBrightnessTouch(0, 0);
    TEST_ASSERT_TRUE(tick(1400, false));
    display.activeSliderFromTouch = 1;
    queueBrightnessTouch(180); // Touch the already shown provisional 75%.
    TEST_ASSERT_TRUE(tick(1500, false));
    display.activeSliderFromTouch = 0;
    queueBrightnessTouch(600);
    TEST_ASSERT_TRUE(tick(1560, false));
    TEST_ASSERT_EQUAL_UINT8(80, display.lastSettingsBrightness);

    mock_preferences::set_fail_begin_for_namespace(nullptr);
    TEST_ASSERT_TRUE(boot.setStealthEnabled(true).success);
    runtime.syncRecoveredSettings();
    TEST_ASSERT_EQUAL_UINT8(80, display.lastBrightness);
    TEST_ASSERT_EQUAL_UINT8(55, appliedVoiceVolume);
    queueBrightnessTouch(0, 0);
    TEST_ASSERT_TRUE(tick(1700, false));
    TEST_ASSERT_EQUAL_UINT8(80, display.lastSettingsBrightness);
    TEST_ASSERT_EQUAL_UINT8(55, display.lastSettingsVolume);
    tick(2100, true);
    TEST_ASSERT_FALSE(tick(2500, false));

    SettingsManager reboot(storage, profiles);
    reboot.load();
    TEST_ASSERT_EQUAL_UINT8(80, reboot.get().brightness);
    TEST_ASSERT_EQUAL_UINT8(55, reboot.get().voiceVolume);
    TEST_ASSERT_TRUE(reboot.get().stealthEnabled);
}

int main() {
  UNITY_BEGIN();
    RUN_TEST(test_local_brightness_adjustment_preserves_untouched_recovered_volume);
    RUN_TEST(test_local_brightness_adjustment_waits_for_unavailable_settings);
    RUN_TEST(test_local_brightness_preemption_defers_recovery_and_persistence);
    RUN_TEST(test_recovery_updates_untouched_sliders_and_preserves_an_open_preview);
  RUN_TEST(test_recovered_auto_off_starts_full_interval_without_new_detector_edge);
  RUN_TEST(test_service_recovery_restores_runtime_before_obd_sync);
  RUN_TEST(test_setter_recovery_restores_runtime_during_persistence_pressure);
  RUN_TEST(test_pending_sd_recovery_cannot_commit_provisional_obd_snapshot);
  return UNITY_END();
}
