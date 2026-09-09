/**
 * Regression boundary: deferred settings updates coalesce, retry failed NVS
 * writes, and flush immediately when an explicit save is requested.
 */
#include <unity.h>

#include <filesystem>

#include <ArduinoJson.h>

#include "../mocks/Arduino.h"
#include "../mocks/Preferences.h"
#include "../mocks/nvs.h"
#include "../mocks/storage_manager.h"
#include "../../src/settings.h"
#include "../../src/settings_keys.h"
#include "../../src/v1_profiles.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

namespace ArduinoJson {

inline void convertFromJson(JsonVariantConst src, ::String& dst) {
    const char* raw = src.as<const char*>();
    dst = ::String(raw ? raw : "");
}

inline bool canConvertFromJson(JsonVariantConst src, const ::String&) {
    return src.is<const char*>();
}

}  // namespace ArduinoJson

V1ProfileManager profiles;
SettingsManager settings(storage, profiles);

#include "../../src/v1_profiles.cpp"
#include "../../src/backup_payload_builder.cpp"
#include "../../src/psram_freertos_alloc.cpp"
#include "../../src/settings.cpp"
#include "../../src/settings_setters.cpp"
#include "../../src/settings_nvs.cpp"
#include "../../src/settings_backup.cpp"
#include "../../src/settings_backup_doc.cpp"
#include "../../src/settings_restore.cpp"
#include "../../src/touch_handler.cpp"
#include "../mocks/display.h"
#include "../../src/modules/touch/touch_ui_module.cpp"

// The real touch/settings path runs below; only audio and physical display/I2C
// interfaces are substituted. Persistence failures use the Preferences boundary.
void audio_set_volume(uint8_t) {}
void play_test_voice() {}

namespace {

std::filesystem::path g_tempRoot;
int g_tempRootIndex = 0;

std::filesystem::path nextTempRoot() {
    return std::filesystem::temp_directory_path() /
           ("settings_deferred_persist_" + std::to_string(++g_tempRootIndex));
}

void resetRuntimeState() {
    mock_preferences::reset();
    mock_nvs::reset();
    mock_reset_heap_caps();
    mock_reset_queue_create_state();
    mock_reset_task_create_state();
    storage.reset();
    StorageManager::resetMockSdLockState();
    resetDeferredSettingsBackupStateForTest();
    profiles = V1ProfileManager();
    settings = SettingsManager(storage, profiles);
    mockMillis = 1000;
    mockMicros = 1000000;
}

String activeNamespaceOrEmpty() {
    return mock_preferences::getString(SETTINGS_NS_META, "active", "");
}

bool processSliderInput(TouchUiModule& ui, uint32_t nowMs, bool bootPressed) {
    mockMillis = nowMs;
    return ui.process(nowMs, bootPressed);
}

void openAndAdjustBrightness(SettingsManager& manager, V1Display& display, TouchHandler& touch,
                             TouchUiModule& ui) {
    Wire.resetMock();
    TEST_ASSERT_TRUE(touch.begin());
    display.activeSliderFromTouch = 0; // y=80 is the production brightness hit region.
    ui.begin(&display, &touch, &manager, {});
    processSliderInput(ui, 1000, true);
    TEST_ASSERT_TRUE(processSliderInput(ui, 1400, false));

    // A real controller packet passes through TouchHandler before the UI maps
    // x=40 to maximum brightness. Entering the menu already observed release.
    std::vector<uint8_t> touchBytes(32, 0);
    touchBytes[1] = 1;
    touchBytes[3] = 40;
    touchBytes[5] = 80;
    Wire.queueRequestFrom(touchBytes.size(), touchBytes);
    TEST_ASSERT_TRUE(processSliderInput(ui, 1600, false));
    TEST_ASSERT_EQUAL_UINT8(255, display.lastSettingsBrightness);
}

void closeBrightnessSliders(TouchUiModule& ui, uint32_t pressAtMs = 2200) {
    processSliderInput(ui, pressAtMs, true);
    TEST_ASSERT_FALSE(processSliderInput(ui, pressAtMs + 400u, false));
}

void serviceSliderPersistence(SettingsManager& manager, uint32_t nowMs) {
    mockMillis = nowMs;
    manager.serviceDeferredPersist(nowMs);
}

}  // namespace

void setUp() {
    g_tempRoot = nextTempRoot();
    std::filesystem::remove_all(g_tempRoot);
    std::filesystem::create_directories(g_tempRoot);
    resetRuntimeState();
}

void tearDown() {
    std::filesystem::remove_all(g_tempRoot);
    resetDeferredSettingsBackupStateForTest();
}

void assertSliderExitReloadsSelectedBrightness(bool failWrite, bool preempt) {
    SettingsManager manager(storage, profiles);
    TEST_ASSERT_TRUE(manager.saveDeferredBackup());
    TEST_ASSERT_EQUAL_UINT8(200, manager.get().brightness);
    TEST_ASSERT_FALSE(manager.deferredPersistPending());
    V1Display display;
    TouchHandler touch;
    TouchUiModule ui;
    openAndAdjustBrightness(manager, display, touch, ui);
    if (failWrite) mock_preferences::set_fail_writes_for_key(kNvsBrightness);

    if (preempt) {
        mockMillis = 2200;
        TEST_ASSERT_TRUE(ui.preemptForLiveAlert());
        serviceSliderPersistence(manager, 3000);
    } else {
        closeBrightnessSliders(ui);
    }
    TEST_ASSERT_EQUAL_INT(1, display.hideBrightnessSliderCalls);
    TEST_ASSERT_EQUAL_UINT8(255, manager.get().brightness);

    mock_preferences::set_fail_writes_for_key(nullptr);
    serviceSliderPersistence(manager, 10000);
    SettingsManager reloaded(storage, profiles);
    reloaded.load();
    TEST_ASSERT_EQUAL_UINT8(255, reloaded.get().brightness);
    TEST_ASSERT_FALSE(manager.deferredPersistPending());
}

void test_successful_slider_exit_persists_selected_brightness() {
    assertSliderExitReloadsSelectedBrightness(false, false);
}

void test_failed_slider_exit_retries_and_reloads_selected_brightness() {
    assertSliderExitReloadsSelectedBrightness(true, false);
}

void test_alert_preemption_retries_and_reloads_selected_brightness() {
    assertSliderExitReloadsSelectedBrightness(true, true);
}

void test_failed_slider_exit_retries_persistent_failure_with_backoff() {
    SettingsManager manager(storage, profiles);
    TEST_ASSERT_TRUE(manager.saveDeferredBackup());
    V1Display display;
    TouchHandler touch;
    TouchUiModule ui;
    openAndAdjustBrightness(manager, display, touch, ui);
    mock_preferences::set_fail_writes_for_key(kNvsBrightness);
    closeBrightnessSliders(ui);
    TEST_ASSERT_TRUE(manager.deferredPersistPending());
    const uint32_t firstDueMs = manager.deferredPersistNextAttemptAtMs();
    TEST_ASSERT_EQUAL_UINT32(3350u, firstDueMs);
    serviceSliderPersistence(manager, firstDueMs);
    TEST_ASSERT_TRUE(manager.deferredPersistRetryScheduled());
    const uint32_t retryMs = manager.deferredPersistNextAttemptAtMs();
    TEST_ASSERT_EQUAL_UINT32(firstDueMs + 1000u, retryMs);
    serviceSliderPersistence(manager, retryMs - 1u);
    TEST_ASSERT_EQUAL_UINT32(retryMs, manager.deferredPersistNextAttemptAtMs());
    SettingsManager beforeRecovery(storage, profiles);
    beforeRecovery.load();
    TEST_ASSERT_EQUAL_UINT8(200, beforeRecovery.get().brightness);

    mock_preferences::set_fail_writes_for_key(nullptr);
    serviceSliderPersistence(manager, retryMs);
    SettingsManager reloaded(storage, profiles);
    reloaded.load();
    TEST_ASSERT_EQUAL_UINT8(255, reloaded.get().brightness);
    TEST_ASSERT_FALSE(manager.deferredPersistPending());
}

void test_failed_slider_exit_preserves_earlier_deferred_retry_and_values() {
    SettingsManager manager(storage, profiles);
    TEST_ASSERT_TRUE(manager.saveDeferredBackup());
    V1Display display;
    TouchHandler touch;
    TouchUiModule ui;
    openAndAdjustBrightness(manager, display, touch, ui);
    ObdSettingsUpdate learned;
    learned.hasSavedAddress = true;
    learned.savedAddress = "AA:BB:CC:DD:EE:FF";
    TEST_ASSERT_TRUE(manager.applyObdSettingsUpdate(learned, SettingsPersistMode::Deferred).success);
    mock_preferences::set_fail_writes_for_key(kNvsBrightness);
    serviceSliderPersistence(manager, manager.deferredPersistNextAttemptAtMs());
    const uint32_t retryMs = manager.deferredPersistNextAttemptAtMs();
    TEST_ASSERT_TRUE(manager.deferredPersistRetryScheduled());
    closeBrightnessSliders(ui, 2600);
    TEST_ASSERT_TRUE(manager.deferredPersistPending());
    TEST_ASSERT_TRUE(manager.deferredPersistRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(retryMs, manager.deferredPersistNextAttemptAtMs());

    mock_preferences::set_fail_writes_for_key(nullptr);
    serviceSliderPersistence(manager, retryMs);
    SettingsManager reloaded(storage, profiles);
    reloaded.load();
    TEST_ASSERT_EQUAL_UINT8(255, reloaded.get().brightness);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", reloaded.get().obdSavedAddress.c_str());
}

void test_deferred_batch_updates_coalesce_to_single_persist_and_request_backup() {
    SettingsManager manager(storage, profiles);

    DeviceSettingsUpdate firstUpdate;
    firstUpdate.hasProxyName = true;
    firstUpdate.proxyName = "First";
    manager.applyDeviceSettingsUpdate(firstUpdate, SettingsPersistMode::Deferred);

    TEST_ASSERT_TRUE(manager.deferredPersistPending());
    TEST_ASSERT_FALSE(manager.deferredPersistRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(1750u, manager.deferredPersistNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());
    TEST_ASSERT_FALSE(manager.deferredBackupPending());

    mockMillis = 1300;

    DeviceSettingsUpdate secondUpdate;
    secondUpdate.hasProxyName = true;
    secondUpdate.proxyName = "Second";
    manager.applyDeviceSettingsUpdate(secondUpdate, SettingsPersistMode::Deferred);

    TEST_ASSERT_TRUE(manager.deferredPersistPending());
    TEST_ASSERT_FALSE(manager.deferredPersistRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(2050u, manager.deferredPersistNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    manager.serviceDeferredPersist(2049);
    TEST_ASSERT_TRUE(manager.deferredPersistPending());
    TEST_ASSERT_EQUAL_UINT32(2050u, manager.deferredPersistNextAttemptAtMs());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    manager.serviceDeferredPersist(2050);

    const String activeNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(activeNs.length() > 0);
    TEST_ASSERT_EQUAL_STRING("Second",
                             mock_preferences::getString(activeNs.c_str(), "proxyName", "").c_str());
    TEST_ASSERT_FALSE(manager.deferredPersistPending());
    TEST_ASSERT_FALSE(manager.deferredPersistRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(0u, manager.deferredPersistNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
    TEST_ASSERT_TRUE(manager.deferredBackupPending());

    manager.serviceDeferredPersist(3000);
    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
}

void test_deferred_persist_retries_after_failed_nvs_write() {
    SettingsManager manager(storage, profiles);
    manager.mutableSettings().apSSID = "RetryPath";
    manager.requestDeferredPersist();

    mock_preferences::set_fail_writes(true);
    manager.serviceDeferredPersist(1750);

    TEST_ASSERT_TRUE(manager.deferredPersistPending());
    TEST_ASSERT_TRUE(manager.deferredPersistRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(2750u, manager.deferredPersistNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_FALSE(manager.deferredBackupPending());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    mock_preferences::set_fail_writes(false);

    manager.serviceDeferredPersist(2749);
    TEST_ASSERT_TRUE(manager.deferredPersistPending());
    TEST_ASSERT_TRUE(manager.deferredPersistRetryScheduled());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    manager.serviceDeferredPersist(2750);

    const String activeNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(activeNs.length() > 0);
    TEST_ASSERT_EQUAL_STRING("RetryPath",
                             mock_preferences::getString(activeNs.c_str(), "apSSID", "").c_str());
    TEST_ASSERT_FALSE(manager.deferredPersistPending());
    TEST_ASSERT_FALSE(manager.deferredPersistRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(0u, manager.deferredPersistNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
    TEST_ASSERT_TRUE(manager.deferredBackupPending());
}

void test_save_flushes_immediately_and_clears_deferred_persist() {
    SettingsManager manager(storage, profiles);
    manager.mutableSettings().proxyName = "Pending";
    manager.requestDeferredPersist();
    manager.mutableSettings().proxyName = "Immediate";

    manager.save();

    const String activeNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(activeNs.length() > 0);
    TEST_ASSERT_EQUAL_STRING("Immediate",
                             mock_preferences::getString(activeNs.c_str(), "proxyName", "").c_str());
    TEST_ASSERT_FALSE(manager.deferredPersistPending());
    TEST_ASSERT_FALSE(manager.deferredPersistRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(0u, manager.deferredPersistNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
}

void test_last_v1_address_does_not_schedule_full_settings_persist() {
    SettingsManager manager(storage, profiles);

    manager.setLastV1Address("  aa:bb:cc:dd:ee:ff  ");

    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", manager.get().lastV1Address.c_str());
    TEST_ASSERT_FALSE(manager.deferredPersistPending());
    TEST_ASSERT_FALSE(manager.deferredBackupPending());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    // The compatibility value is still included when an explicit settings
    // save is requested, including the graceful-shutdown path.
    manager.save();

    const String activeNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(activeNs.length() > 0);
    TEST_ASSERT_EQUAL_STRING(
        "AA:BB:CC:DD:EE:FF",
        mock_preferences::getString(activeNs.c_str(), "lastV1Addr", "").c_str());

    const bool persistPendingBeforeCaseOnlyUpdate = manager.deferredPersistPending();
    const bool backupPendingBeforeCaseOnlyUpdate = manager.deferredBackupPending();
    manager.setLastV1Address("AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", manager.get().lastV1Address.c_str());
    TEST_ASSERT_EQUAL(persistPendingBeforeCaseOnlyUpdate, manager.deferredPersistPending());
    TEST_ASSERT_EQUAL(backupPendingBeforeCaseOnlyUpdate, manager.deferredBackupPending());
}

void test_last_v1_address_degraded_fallback_uses_one_idempotent_nvs_key() {
    SettingsManager manager(storage, profiles);

    mockMillis = UINT32_MAX - 749u;
    manager.requestLastV1AddressFallbackPersist("AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_EQUAL_STRING(
        "",
        mock_preferences::getString(kSettingsV1RuntimeNamespace, kNvsLastConnectedV1Address, "").c_str());
    manager.serviceDeferredPersist(UINT32_MAX);
    TEST_ASSERT_EQUAL_STRING(
        "",
        mock_preferences::getString(kSettingsV1RuntimeNamespace, kNvsLastConnectedV1Address, "").c_str());
    manager.serviceDeferredPersist(0u);
    TEST_ASSERT_EQUAL_STRING(
        "AA:BB:CC:DD:EE:FF",
        mock_preferences::getString(kSettingsV1RuntimeNamespace, kNvsLastConnectedV1Address, "").c_str());
    TEST_ASSERT_EQUAL_UINT(0u, mock_preferences::missingStringReadCount(kNvsLastConnectedV1Address));
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());
    TEST_ASSERT_FALSE(manager.deferredPersistPending());
    TEST_ASSERT_FALSE(manager.deferredBackupPending());

    // Re-recording the same successful connection is a no-op, even when new
    // NVS writes are unavailable. A failed update leaves the last verified
    // fallback intact and retries outside the connection callback.
    mock_preferences::set_fail_writes(true);
    manager.requestLastV1AddressFallbackPersist("AA:BB:CC:DD:EE:FF");
    manager.serviceDeferredPersist(3000u);
    mockMillis = UINT32_MAX - 1749u;
    manager.requestLastV1AddressFallbackPersist("11:22:33:44:55:66");
    manager.serviceDeferredPersist(UINT32_MAX - 999u);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", manager.loadLastV1AddressFallback().c_str());

    mock_preferences::set_fail_writes(false);
    manager.serviceDeferredPersist(UINT32_MAX);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", manager.loadLastV1AddressFallback().c_str());
    manager.serviceDeferredPersist(0u);
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", manager.loadLastV1AddressFallback().c_str());
    TEST_ASSERT_TRUE(manager.clearLastV1AddressFallback());
    TEST_ASSERT_EQUAL_STRING("", manager.loadLastV1AddressFallback().c_str());
}

void test_full_settings_save_supersedes_pending_degraded_fallback() {
    SettingsManager manager(storage, profiles);

    manager.setLastV1Address("11:22:33:44:55:66");
    manager.requestLastV1AddressFallbackPersist("11:22:33:44:55:66");
    manager.save();

    const String activeNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(activeNs.length() > 0);
    TEST_ASSERT_EQUAL_STRING(
        "11:22:33:44:55:66",
        mock_preferences::getString(activeNs.c_str(), kNvsLastV1Address, "").c_str());
    TEST_ASSERT_EQUAL_STRING("", manager.loadLastV1AddressFallback().c_str());

    // Servicing later must not resurrect the fallback cleared by the newer
    // atomic settings snapshot.
    manager.serviceDeferredPersist(5000u);
    TEST_ASSERT_EQUAL_STRING("", manager.loadLastV1AddressFallback().c_str());
}

void test_filtered_fallback_cleanup_preserves_other_persisted_and_pending_addresses() {
    SettingsManager manager(storage, profiles);
    manager.requestLastV1AddressFallbackPersist("AA:BB:CC:DD:EE:FF");
    manager.serviceDeferredPersist(5000);
    manager.requestLastV1AddressFallbackPersist("11:22:33:44:55:66");
    TEST_ASSERT_TRUE(manager.clearLastV1AddressFallback("aa-bb-cc-dd-ee-ff"));
    TEST_ASSERT_EQUAL_STRING("", manager.loadLastV1AddressFallback().c_str());
    manager.serviceDeferredPersist(6000);
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", manager.loadLastV1AddressFallback().c_str());

    manager.requestLastV1AddressFallbackPersist("AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_TRUE(manager.clearLastV1AddressFallback("AA:BB:CC:DD:EE:FF"));
    manager.serviceDeferredPersist(7000);
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", manager.loadLastV1AddressFallback().c_str());
}

void test_filtered_fallback_cleanup_cancels_pending_only_address() {
    SettingsManager manager(storage, profiles);
    manager.requestLastV1AddressFallbackPersist("AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_TRUE(manager.clearLastV1AddressFallback("AA:BB:CC:DD:EE:FF"));
    manager.serviceDeferredPersist(5000);
    TEST_ASSERT_EQUAL_STRING("", manager.loadLastV1AddressFallback().c_str());
}

void test_failed_filtered_fallback_cleanup_preserves_durable_and_pending_intent() {
    SettingsManager manager(storage, profiles);
    manager.requestLastV1AddressFallbackPersist("AA:BB:CC:DD:EE:FF");
    mock_preferences::set_fail_begin_for_namespace(kSettingsV1RuntimeNamespace);
    TEST_ASSERT_FALSE(manager.clearLastV1AddressFallback("AA:BB:CC:DD:EE:FF"));
    mock_preferences::set_fail_begin_for_namespace(nullptr);
    manager.serviceDeferredPersist(5000);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", manager.loadLastV1AddressFallback().c_str());
    manager.requestLastV1AddressFallbackPersist("11:22:33:44:55:66");
    mock_preferences::set_fail_writes_for_key(kNvsLastConnectedV1Address);
    TEST_ASSERT_FALSE(manager.clearLastV1AddressFallback("AA:BB:CC:DD:EE:FF"));
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", manager.loadLastV1AddressFallback().c_str());
    mock_preferences::set_fail_writes_for_key(nullptr);
    manager.serviceDeferredPersist(6000);
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", manager.loadLastV1AddressFallback().c_str());
}

void test_filtered_cleanup_rejects_present_unreadable_key_and_preserves_pending_intent() {
    for (bool wrongType : {false, true}) {
        SettingsManager manager(storage, profiles);
        manager.requestLastV1AddressFallbackPersist("AA:BB:CC:DD:EE:FF");
        Preferences prefs;
        TEST_ASSERT_TRUE(prefs.begin(kSettingsV1RuntimeNamespace, false));
        if (wrongType) prefs.putUInt(kNvsLastConnectedV1Address, 17);
        else prefs.putString(kNvsLastConnectedV1Address, "");
        TEST_ASSERT_TRUE(prefs.isKey(kNvsLastConnectedV1Address));
        const auto previousType = prefs.getType(kNvsLastConnectedV1Address);
        TEST_ASSERT_FALSE(manager.clearLastV1AddressFallback("AA:BB:CC:DD:EE:FF"));
        TEST_ASSERT_EQUAL(previousType, prefs.getType(kNvsLastConnectedV1Address));
        prefs.end();
        manager.serviceDeferredPersist(5000);
        TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", manager.loadLastV1AddressFallback().c_str());
        TEST_ASSERT_TRUE(manager.clearLastV1AddressFallback());
    }
}

void test_display_configuration_revision_preserves_changes_between_serial_samples() {
    SettingsManager manager(storage, profiles);
    TEST_ASSERT_EQUAL_UINT32(0u, manager.displayConfigurationRevision());
    manager.setStealthEnabled(true, SettingsPersistMode::Deferred);
    manager.setStealthEnabled(false, SettingsPersistMode::Deferred);
    TEST_ASSERT_FALSE(manager.get().stealthEnabled);
    TEST_ASSERT_EQUAL_UINT32(2u, manager.displayConfigurationRevision());

    manager.setActiveSlot(1, SettingsPersistMode::Deferred);
    manager.setActiveSlot(0, SettingsPersistMode::Deferred);
    TEST_ASSERT_EQUAL_INT(0, manager.get().activeSlot);
    TEST_ASSERT_EQUAL_UINT32(4u, manager.displayConfigurationRevision());
    // Coalesced persistence has not run, so its revision cannot prove this.
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
}

void test_display_configuration_revision_tracks_effective_policy_and_ignores_noops() {
    SettingsManager manager(storage, profiles);
    manager.setStealthEnabled(false, SettingsPersistMode::Deferred);
    manager.setActiveSlot(0, SettingsPersistMode::Deferred);
    DeviceSettingsUpdate unrelated;
    unrelated.hasProxyName = true;
    unrelated.proxyName = "Changed";
    manager.applyDeviceSettingsUpdate(unrelated, SettingsPersistMode::Deferred);

    AutoPushSlotUpdate inactive;
    inactive.slot = 1;
    inactive.hasPriorityArrowOnly = true;
    inactive.priorityArrowOnly = !manager.getSlotPriorityArrowOnly(1);
    manager.applyAutoPushSlotUpdate(inactive, SettingsPersistMode::Deferred);
    TEST_ASSERT_EQUAL_UINT32(0u, manager.displayConfigurationRevision());

    AutoPushStateUpdate selection;
    selection.hasActiveSlot = true;
    selection.activeSlot = 1;
    manager.applyAutoPushStateUpdate(selection, SettingsPersistMode::Deferred);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.displayConfigurationRevision());
    manager.applyAutoPushSlotUpdate(inactive, SettingsPersistMode::Deferred);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.displayConfigurationRevision());
    inactive.priorityArrowOnly = !inactive.priorityArrowOnly;
    inactive.hasAlertPersist = true;
    inactive.alertPersist = manager.getSlotAlertPersistSec(1) == 0 ? 3 : 0;
    manager.applyAutoPushSlotUpdate(inactive, SettingsPersistMode::Deferred);
    TEST_ASSERT_EQUAL_UINT32(2u, manager.displayConfigurationRevision());

    AudioSettingsUpdate quiet;
    quiet.hasStealthEnabled = true;
    quiet.stealthEnabled = true;
    manager.applyAudioSettingsUpdate(quiet, SettingsPersistMode::Deferred);
    TEST_ASSERT_EQUAL_UINT32(3u, manager.displayConfigurationRevision());
}

void test_display_configuration_revision_is_not_rolled_back_by_failed_persistence() {
    SettingsManager manager(storage, profiles);
    mock_preferences::set_fail_writes(true);
    const auto result = manager.setStealthEnabled(true);
    TEST_ASSERT_FALSE(result.success);
    TEST_ASSERT_FALSE(manager.get().stealthEnabled);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.displayConfigurationRevision());

    AutoPushSlotUpdate update;
    update.slot = 0;
    update.hasPriorityArrowOnly = true;
    const bool originalArrowPolicy = manager.getSlotPriorityArrowOnly(0);
    update.priorityArrowOnly = !originalArrowPolicy;
    TEST_ASSERT_FALSE(manager.applyAutoPushSlotUpdatePersisted(update).success);
    TEST_ASSERT_EQUAL(originalArrowPolicy, manager.getSlotPriorityArrowOnly(0));
    TEST_ASSERT_EQUAL_UINT32(2u, manager.displayConfigurationRevision());
}

void test_display_configuration_revision_saturates_instead_of_reusing_a_value() {
    SettingsManager manager(storage, profiles);
    manager.utSetDisplayConfigurationRevision(UINT32_MAX - 1u);
    manager.setStealthEnabled(true, SettingsPersistMode::Deferred);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, manager.displayConfigurationRevision());
    manager.setStealthEnabled(false, SettingsPersistMode::Deferred);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, manager.displayConfigurationRevision());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_successful_slider_exit_persists_selected_brightness);
    RUN_TEST(test_failed_slider_exit_retries_and_reloads_selected_brightness);
    RUN_TEST(test_alert_preemption_retries_and_reloads_selected_brightness);
    RUN_TEST(test_failed_slider_exit_retries_persistent_failure_with_backoff);
    RUN_TEST(test_failed_slider_exit_preserves_earlier_deferred_retry_and_values);
    RUN_TEST(test_display_configuration_revision_preserves_changes_between_serial_samples);
    RUN_TEST(test_display_configuration_revision_tracks_effective_policy_and_ignores_noops);
    RUN_TEST(test_display_configuration_revision_is_not_rolled_back_by_failed_persistence);
    RUN_TEST(test_display_configuration_revision_saturates_instead_of_reusing_a_value);
    RUN_TEST(test_filtered_fallback_cleanup_preserves_other_persisted_and_pending_addresses);
    RUN_TEST(test_filtered_fallback_cleanup_cancels_pending_only_address);
    RUN_TEST(test_failed_filtered_fallback_cleanup_preserves_durable_and_pending_intent);
    RUN_TEST(test_filtered_cleanup_rejects_present_unreadable_key_and_preserves_pending_intent);
    RUN_TEST(test_deferred_batch_updates_coalesce_to_single_persist_and_request_backup);
    RUN_TEST(test_deferred_persist_retries_after_failed_nvs_write);
    RUN_TEST(test_save_flushes_immediately_and_clears_deferred_persist);
    RUN_TEST(test_last_v1_address_does_not_schedule_full_settings_persist);
    RUN_TEST(test_last_v1_address_degraded_fallback_uses_one_idempotent_nvs_key);
    RUN_TEST(test_full_settings_save_supersedes_pending_degraded_fallback);
    return UNITY_END();
}
