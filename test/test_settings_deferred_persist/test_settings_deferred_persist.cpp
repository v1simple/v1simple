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
#include "../../src/storage_json_rollback.cpp"
#include "../../src/v1_devices.cpp"
#include "../../src/settings_v1_device_delete.cpp"
#include "../../src/touch_handler.cpp"
#include "../mocks/display.h"
#include "../mocks/runtime_presentation_source.h"
#include "../../src/modules/touch/touch_ui_module.cpp"
#include "../../src/modules/wifi/wifi_audio_api_service.cpp"

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

std::string readFileToString(fs::FS& fs, const char* path) {
    File file = fs.open(path, FILE_READ);
    if (!file) return {};
    std::string contents;
    while (file.available()) contents.push_back(static_cast<char>(file.read()));
    file.close();
    return contents;
}

void resetRuntimeState() {
    mockStringCopyAssignmentsBeforeFailure = -1;
    mockStringCopyConstructorFailureSource = nullptr;
    mockStringCopyConstructorFailures = 0;
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
                             TouchUiModule& ui, RuntimePresentationSource& presentation) {
    Wire.resetMock();
    TEST_ASSERT_TRUE(touch.begin());
    display.activeSliderFromTouch = 0; // y=80 is the production brightness hit region.
    ui.begin(&display, &touch, &manager, {}, presentation);
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
    mockStringCopyAssignmentsBeforeFailure = -1;
    mockStringCopyConstructorFailureSource = nullptr;
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
    RuntimePresentationSource presentation(manager);
    TouchUiModule ui;
    openAndAdjustBrightness(manager, display, touch, ui, presentation);
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
    RuntimePresentationSource presentation(manager);
    TouchUiModule ui;
    openAndAdjustBrightness(manager, display, touch, ui, presentation);
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
    RuntimePresentationSource presentation(manager);
    TouchUiModule ui;
    openAndAdjustBrightness(manager, display, touch, ui, presentation);
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

void seedRuntimeFallback(const char* address) {
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(kSettingsV1RuntimeNamespace, false));
    TEST_ASSERT_EQUAL_UINT(std::strlen(address), prefs.putString(kNvsLastConnectedV1Address, address));
    prefs.end();
}

void test_device_delete_intent_converges_fallback_and_both_catalog_copies() {
    fs::FS primary(g_tempRoot / "delete_primary");
    fs::FS secondary(g_tempRoot / "delete_secondary");
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&primary, &secondary));
    TEST_ASSERT_TRUE(devices.upsertDevice("AA:BB:CC:DD:EE:FF"));
    seedRuntimeFallback("AA:BB:CC:DD:EE:FF");

    const V1DeviceMutationResult result =
        settings.deleteV1DeviceTransactional("AA:BB:CC:DD:EE:FF", devices);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(V1DeviceMutationStatus::FullyMirrored),
                          static_cast<int>(result.status));
    TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace,
                                                        kNvsLastConnectedV1Address));
    TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace, kNvsV1DeleteReady));
    bool present = true;
    TEST_ASSERT_TRUE(devices.containsDeviceChecked("AA:BB:CC:DD:EE:FF", present));
    TEST_ASSERT_FALSE(present);

    V1DeviceStore rebooted;
    TEST_ASSERT_TRUE(rebooted.begin(&primary, &secondary));
    TEST_ASSERT_TRUE(rebooted.listDevices().empty());
}

void test_absent_runtime_namespace_is_initialized_before_delete_recovery() {
    mock_preferences::set_require_existing_namespace_for_read(true);
    Preferences absent;
    TEST_ASSERT_FALSE(absent.begin(kSettingsV1RuntimeNamespace, true));

    SettingsManager manager(storage, profiles);
    manager.begin();

    TEST_ASSERT_NOT_NULL(mock_preferences::findNamespace(kSettingsV1RuntimeNamespace));
    fs::FS primary(g_tempRoot / "empty_device_catalog");
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&primary));
    TEST_ASSERT_TRUE(manager.resolvePendingV1DeviceDelete(devices));
}

void test_unavailable_runtime_namespace_still_fails_delete_recovery_closed() {
    mock_preferences::set_require_existing_namespace_for_read(true);
    mock_preferences::set_fail_begin_for_namespace(kSettingsV1RuntimeNamespace);
    SettingsManager manager(storage, profiles);
    manager.begin();

    fs::FS primary(g_tempRoot / "unavailable_device_catalog");
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&primary));
    TEST_ASSERT_FALSE(manager.resolvePendingV1DeviceDelete(devices));
    mock_preferences::set_fail_begin_for_namespace(nullptr);
}

void test_invalid_runtime_delete_intent_remains_preserved_and_blocking() {
    mock_preferences::set_require_existing_namespace_for_read(true);
    SettingsManager manager(storage, profiles);
    manager.begin();
    Preferences runtime;
    TEST_ASSERT_TRUE(runtime.begin(kSettingsV1RuntimeNamespace, false));
    TEST_ASSERT_EQUAL_UINT(sizeof(uint8_t), runtime.putUChar(kNvsV1DeleteReady, 2));
    runtime.end();

    fs::FS primary(g_tempRoot / "invalid_device_catalog");
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&primary));
    TEST_ASSERT_FALSE(manager.resolvePendingV1DeviceDelete(devices));
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace,
                                                        kNvsV1DeleteReady));
}

void test_device_delete_primary_failure_is_durable_pending_and_boot_retries() {
    fs::FS primary(g_tempRoot / "delete_retry");
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&primary));
    TEST_ASSERT_TRUE(devices.upsertDevice("AA:BB:CC:DD:EE:FF"));
    seedRuntimeFallback("AA:BB:CC:DD:EE:FF");
    fs::mock_set_fs_write_budget(20);

    const V1DeviceMutationResult pending =
        settings.deleteV1DeviceTransactional("AA:BB:CC:DD:EE:FF", devices);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(V1DeviceMutationStatus::DurablePending),
                          static_cast<int>(pending.status));
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace, kNvsV1DeleteReady));
    TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace,
                                                         kNvsLastConnectedV1Address));
    bool present = false;
    TEST_ASSERT_TRUE(devices.containsDeviceChecked("AA:BB:CC:DD:EE:FF", present));
    TEST_ASSERT_TRUE(present);

    fs::mock_reset_fs_write_budget();
    V1DeviceStore rebooted;
    TEST_ASSERT_TRUE(rebooted.begin(&primary));
    TEST_ASSERT_TRUE(settings.resolvePendingV1DeviceDelete(rebooted));
    TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace, kNvsV1DeleteReady));
    TEST_ASSERT_TRUE(rebooted.listDevices().empty());
}

void test_device_delete_staging_failures_never_publish_intent_or_mutate() {
    static constexpr const char* kFailedKeys[] = {
        kNvsV1DeleteAddress, kNvsV1DeleteToken, kNvsV1DeletePhase, kNvsV1DeleteReady,
    };
    for (const char* failedKey : kFailedKeys) {
        resetRuntimeState();
        fs::FS primary(g_tempRoot / failedKey);
        V1DeviceStore devices;
        TEST_ASSERT_TRUE(devices.begin(&primary));
        TEST_ASSERT_TRUE(devices.upsertDevice("AA:BB:CC:DD:EE:FF"));
        seedRuntimeFallback("AA:BB:CC:DD:EE:FF");
        const std::string before = readFileToString(primary, "/v1devices.json");
        mock_preferences::set_fail_writes_for_key(failedKey);

        const V1DeviceMutationResult result =
            settings.deleteV1DeviceTransactional("AA:BB:CC:DD:EE:FF", devices);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(V1DeviceMutationStatus::NotCommitted),
                              static_cast<int>(result.status));
        TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace, kNvsV1DeleteReady));
        TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace,
                                                           kNvsLastConnectedV1Address));
        TEST_ASSERT_EQUAL_STRING(before.c_str(), readFileToString(primary, "/v1devices.json").c_str());
        mock_preferences::set_fail_writes_for_key(nullptr);
        const V1DeviceMutationResult retried =
            settings.deleteV1DeviceTransactional("AA:BB:CC:DD:EE:FF", devices);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(V1DeviceMutationStatus::FullyMirrored),
                              static_cast<int>(retried.status));
        TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace, kNvsV1DeleteReady));
        TEST_ASSERT_TRUE(devices.listDevices().empty());
    }
}

void test_device_delete_clears_matching_pending_fallback_but_preserves_other_durable_address() {
    fs::FS primary(g_tempRoot / "delete_inverse_fallback");
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&primary));
    TEST_ASSERT_TRUE(devices.upsertDevice("AA:BB:CC:DD:EE:FF"));
    seedRuntimeFallback("11:22:33:44:55:66");
    settings.requestLastV1AddressFallbackPersist("AA:BB:CC:DD:EE:FF");

    const V1DeviceMutationResult result =
        settings.deleteV1DeviceTransactional("AA:BB:CC:DD:EE:FF", devices);
    TEST_ASSERT_TRUE(result.fullyMirrored());
    settings.serviceDeferredPersist(mockMillis + 5000u);
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66",
                             mock_preferences::getString(kSettingsV1RuntimeNamespace,
                                                         kNvsLastConnectedV1Address, "").c_str());
}

void test_pending_delete_for_another_address_returns_busy_without_accepting_it() {
    fs::FS primary(g_tempRoot / "delete_busy");
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&primary));
    TEST_ASSERT_TRUE(devices.upsertDevice("AA:BB:CC:DD:EE:FF"));
    TEST_ASSERT_TRUE(devices.upsertDevice("11:22:33:44:55:66"));
    seedRuntimeFallback("AA:BB:CC:DD:EE:FF");
    fs::mock_set_fs_write_budget(20);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(V1DeviceMutationStatus::DurablePending),
                          static_cast<int>(settings.deleteV1DeviceTransactional(
                              "AA:BB:CC:DD:EE:FF", devices).status));

    const V1DeviceMutationResult busy =
        settings.deleteV1DeviceTransactional("11:22:33:44:55:66", devices);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(V1DeviceMutationStatus::Busy),
                          static_cast<int>(busy.status));
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF",
                             mock_preferences::getString(kSettingsV1RuntimeNamespace,
                                                         kNvsV1DeleteAddress, "").c_str());
    bool secondPresent = false;
    TEST_ASSERT_TRUE(devices.containsDeviceChecked("11:22:33:44:55:66", secondPresent));
    TEST_ASSERT_TRUE(secondPresent);
    fs::mock_reset_fs_write_budget();
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
    TEST_ASSERT_TRUE(manager.applyAutoPushSlotUpdatePersisted(inactive).success);
    TEST_ASSERT_EQUAL_UINT32(0u, manager.displayConfigurationRevision());

    AutoPushStateUpdate selection;
    selection.hasActiveSlot = true;
    selection.activeSlot = 1;
    manager.applyAutoPushStateUpdate(selection, SettingsPersistMode::Deferred);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.displayConfigurationRevision());
    TEST_ASSERT_TRUE(manager.applyAutoPushSlotUpdatePersisted(inactive).success);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.displayConfigurationRevision());
    inactive.priorityArrowOnly = !inactive.priorityArrowOnly;
    inactive.hasAlertPersist = true;
    inactive.alertPersist = manager.getSlotAlertPersistSec(1) == 0 ? 3 : 0;
    TEST_ASSERT_TRUE(manager.applyAutoPushSlotUpdatePersisted(inactive).success);
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

namespace {

constexpr const char* kSavedApSsid = "SavedAccessPointForRollback";
constexpr const char* kSavedApPassword = "SavedPasswordForRollback123";

void assertSavedApCredentials(const V1Settings& value) {
    TEST_ASSERT_EQUAL_STRING(kSavedApSsid, value.apSSID.c_str());
    TEST_ASSERT_EQUAL_STRING(kSavedApPassword, value.apPassword.c_str());
}

void seedSavedApCredentials(SettingsManager& manager) {
    DeviceSettingsUpdate update;
    update.hasApCredentials = true;
    update.apSSID = kSavedApSsid;
    update.apPassword = kSavedApPassword;
    TEST_ASSERT_TRUE(manager.applyDeviceSettingsUpdate(
        update, SettingsPersistMode::ImmediateNvsDeferredBackup).success);
    SettingsManager reloaded(storage, profiles);
    reloaded.load();
    assertSavedApCredentials(reloaded.get());
}

void assertAudioSavePreservesCredentials(bool failCopy, bool failPersist) {
    SettingsManager manager(storage, profiles);
    seedSavedApCredentials(manager);
    const uint8_t originalVolume = manager.get().voiceVolume;
    WifiAudioApiService::Runtime runtime;
    runtime.ctx = &manager;
    runtime.getSettings = [](void* ctx) -> const V1Settings& {
        return static_cast<SettingsManager*>(ctx)->get();
    };
    runtime.applySettingsUpdate = [](const AudioSettingsUpdate& update, void* ctx) {
        return static_cast<SettingsManager*>(ctx)->applyAudioSettingsUpdate(
            update, SettingsPersistMode::ImmediateNvsDeferredBackup);
    };
    WebServer server;
    server.setArg("voiceVolume", "43");
    if (failCopy) mockStringCopyConstructorFailureSource = &manager.get().apSSID;
    if (failPersist) mock_preferences::set_fail_writes_for_key(kNvsVoiceVolume);
    WifiAudioApiService::handleApiSave(server, runtime);
    mockStringCopyConstructorFailureSource = nullptr;
    mock_preferences::set_fail_writes_for_key(nullptr);

    TEST_ASSERT_EQUAL_UINT(failCopy ? 1u : 0u, mockStringCopyConstructorFailures);
    TEST_ASSERT_EQUAL_INT(failCopy || failPersist ? 500 : 200, server.lastStatusCode);
    assertSavedApCredentials(manager.get());
    TEST_ASSERT_EQUAL_UINT8(failCopy || failPersist ? originalVolume : 43, manager.get().voiceVolume);
    SettingsManager afterFirst(storage, profiles);
    afterFirst.load();
    assertSavedApCredentials(afterFirst.get());
    TEST_ASSERT_EQUAL_UINT8(failCopy || failPersist ? originalVolume : 43, afterFirst.get().voiceVolume);

    server.setArg("voiceVolume", "44");
    WifiAudioApiService::handleApiSave(server, runtime);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    SettingsManager afterHealthySave(storage, profiles);
    afterHealthySave.load();
    assertSavedApCredentials(manager.get());
    assertSavedApCredentials(afterHealthySave.get());
    TEST_ASSERT_EQUAL_UINT8(44, afterHealthySave.get().voiceVolume);
}

enum class SnapshotOwner { ActiveSlot, Stealth, AutoPush, WifiPriority, Audio, Display, DisplayReset, Obd, Device };
constexpr SnapshotOwner kSnapshotOwners[] = {
    SnapshotOwner::ActiveSlot, SnapshotOwner::Stealth, SnapshotOwner::AutoPush,
    SnapshotOwner::WifiPriority, SnapshotOwner::Audio, SnapshotOwner::Display,
    SnapshotOwner::DisplayReset, SnapshotOwner::Obd, SnapshotOwner::Device,
};

SettingsPersistResult mutateSnapshotOwner(SettingsManager& manager, SnapshotOwner owner,
                                         SettingsPersistMode mode = SettingsPersistMode::ImmediateNvsDeferredBackup) {
    switch (owner) {
        case SnapshotOwner::ActiveSlot: return manager.setActiveSlot(1, mode);
        case SnapshotOwner::Stealth: return manager.setStealthEnabled(true, mode);
        case SnapshotOwner::AutoPush: {
            AutoPushStateUpdate update;
            update.hasActiveSlot = true;
            update.activeSlot = 1;
            return manager.applyAutoPushStateUpdate(update, mode);
        }
        case SnapshotOwner::WifiPriority: return manager.applyWifiStaPriorityUpdates({{0, 1}, {1, 0}});
        case SnapshotOwner::Audio: {
            AudioSettingsUpdate update;
            update.hasVoiceVolume = true;
            update.voiceVolume = 43;
            return manager.applyAudioSettingsUpdate(update, mode);
        }
        case SnapshotOwner::Display: {
            DisplaySettingsUpdate update;
            update.hasColorBogey = true;
            update.colorBogey = 0x4321;
            return manager.applyDisplaySettingsUpdate(update, mode);
        }
        case SnapshotOwner::DisplayReset: return manager.resetDisplaySettings(mode);
        case SnapshotOwner::Obd: {
            ObdSettingsUpdate update;
            update.hasMinRssi = true;
            update.minRssi = -65;
            return manager.applyObdSettingsUpdate(update, mode);
        }
        case SnapshotOwner::Device: {
            DeviceSettingsUpdate update;
            update.hasAutoPowerOffMinutes = true;
            update.autoPowerOffMinutes = 37;
            return manager.applyDeviceSettingsUpdate(update, mode);
        }
    }
    return {};
}

std::vector<const String*> snapshotStrings(const V1Settings& value) {
    std::vector<const String*> fields = {
        &value.apSSID, &value.apPassword, &value.wifiClientSSID, &value.proxyName,
        &value.lastV1Address, &value.obdSavedAddress, &value.obdSavedName,
        &value.slot0Name, &value.slot1Name, &value.slot2Name,
        &value.slot0_default.profileName, &value.slot1_highway.profileName, &value.slot2_comfort.profileName,
    };
    for (const auto& slot : value.wifiStaSlots) {
        fields.push_back(&slot.ssid);
        fields.push_back(&slot.label);
    }
    return fields;
}

void seedSnapshotSettings(SettingsManager& manager, bool everyString = false) {
    auto& value = manager.mutableSettings();
    value.apSSID = kSavedApSsid;
    value.apPassword = kSavedApPassword;
    value.colorBogey = 0x1234;
    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        auto& slot = value.wifiStaSlots[index];
        slot.ssid = String("SavedNetworkForTest") + String(static_cast<unsigned>(index));
        slot.label = String("SavedNetworkLabel") + String(static_cast<unsigned>(index));
        slot.priority = static_cast<uint8_t>(index);
    }
    value.refreshWifiClientAliasFromSlots();
    if (everyString) {
        value.proxyName = "SavedProxyNameForTest";
        value.lastV1Address = "AA:BB:CC:DD:EE:FF";
        value.obdSavedAddress = "11:22:33:44:55:66";
        value.obdSavedName = "SavedAdapterNameForTest";
        for (int index = 0; index < 3; ++index) {
            auto slot = value.autoPushSlotView(index);
            slot.name = String("SavedSlotNameTest") + String(index);
            slot.config.profileName = String("SavedProfileForTest") + String(index);
        }
    }
}

void assertSnapshotState(const V1Settings& expected, const V1Settings& actual) {
    const auto expectedStrings = snapshotStrings(expected);
    const auto actualStrings = snapshotStrings(actual);
    for (size_t index = 0; index < expectedStrings.size(); ++index) {
        TEST_ASSERT_EQUAL_STRING(expectedStrings[index]->c_str(), actualStrings[index]->c_str());
    }
    TEST_ASSERT_EQUAL(expected.activeSlot, actual.activeSlot);
    TEST_ASSERT_EQUAL(expected.stealthEnabled, actual.stealthEnabled);
    TEST_ASSERT_EQUAL(expected.autoPushEnabled, actual.autoPushEnabled);
    TEST_ASSERT_EQUAL(expected.voiceVolume, actual.voiceVolume);
    TEST_ASSERT_EQUAL(expected.obdMinRssi, actual.obdMinRssi);
    TEST_ASSERT_EQUAL(expected.autoPowerOffMinutes, actual.autoPowerOffMinutes);
    DisplaySettingsContract::forEachScalar([&](const auto& field) {
        TEST_ASSERT_EQUAL(expected.*(field.value), actual.*(field.value));
    });
    TEST_ASSERT_EQUAL_UINT16_ARRAY(expected.colorBars, actual.colorBars, SIGNAL_BAR_COLOR_COUNT);
    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        TEST_ASSERT_EQUAL(expected.wifiStaSlots[index].priority, actual.wifiStaSlots[index].priority);
    }
}

void assertOwnerRejectsEveryIncompleteSnapshot(SnapshotOwner owner) {
    SettingsManager manager(storage, profiles);
    seedSnapshotSettings(manager, true);
    const V1Settings before = manager.get();
    unsigned writes = 0;
    mock_preferences::afterWrite() = [&](const char*) { ++writes; };
    for (const String* field : snapshotStrings(manager.get())) {
        TEST_ASSERT_GREATER_OR_EQUAL_UINT(14, field->length());
        mockStringCopyConstructorFailureSource = field;
        mockStringCopyConstructorFailures = 0;
        const auto result = mutateSnapshotOwner(manager, owner);
        mockStringCopyConstructorFailureSource = nullptr;
        TEST_ASSERT_EQUAL_UINT(1, mockStringCopyConstructorFailures);
        TEST_ASSERT_FALSE(result.success);
        TEST_ASSERT_FALSE(result.changed);
        assertSnapshotState(before, manager.get());
        TEST_ASSERT_FALSE(manager.deferredPersistPending());
        TEST_ASSERT_EQUAL_UINT32(0, manager.displayConfigurationRevision());
        TEST_ASSERT_EQUAL_UINT32(1, manager.backupRevision());
        TEST_ASSERT_EQUAL_UINT(0, writes);
    }
    mock_preferences::afterWrite() = {};
}

} // namespace

void test_string_constructor_fault_is_targeted_once_and_propagates_invalid_state() {
    String source(kSavedApSsid);
    String equalValue(kSavedApSsid);
    mockStringCopyConstructorFailureSource = &source;
    const String unrelated(equalValue);
    TEST_ASSERT_EQUAL_STRING(kSavedApSsid, unrelated.c_str());
    const String failed(source);
    TEST_ASSERT_EQUAL_UINT(1, mockStringCopyConstructorFailures);
    TEST_ASSERT_EQUAL_UINT(0, failed.length());
    TEST_ASSERT_NULL(failed.c_str());
    const String copiedInvalid(failed);
    TEST_ASSERT_NULL(copiedInvalid.c_str());
    String assignedInvalid("Previously valid");
    assignedInvalid = failed;
    TEST_ASSERT_NULL(assignedInvalid.c_str());
    const String nextCopy(source);
    TEST_ASSERT_EQUAL_STRING(kSavedApSsid, nextCopy.c_str());
    const String validEmpty("");
    TEST_ASSERT_NOT_NULL(validEmpty.c_str());
    String small("Inline");
    mockStringCopyConstructorFailureSource = &small;
    const String inlineCopy(small);
    TEST_ASSERT_EQUAL_STRING("Inline", inlineCopy.c_str());
    TEST_ASSERT_EQUAL_UINT(1, mockStringCopyConstructorFailures);
    mockStringCopyConstructorFailureSource = nullptr;
}

void test_audio_http_healthy_save_preserves_credentials() { assertAudioSavePreservesCredentials(false, false); }
void test_audio_http_nvs_failure_preserves_credentials() { assertAudioSavePreservesCredentials(false, true); }
void test_audio_http_snapshot_failure_preserves_credentials() { assertAudioSavePreservesCredentials(true, false); }
void test_audio_http_snapshot_and_nvs_failure_preserve_credentials() { assertAudioSavePreservesCredentials(true, true); }
void test_active_slot_rejects_incomplete_snapshot() { assertOwnerRejectsEveryIncompleteSnapshot(SnapshotOwner::ActiveSlot); }
void test_stealth_rejects_incomplete_snapshot() { assertOwnerRejectsEveryIncompleteSnapshot(SnapshotOwner::Stealth); }
void test_autopush_rejects_incomplete_snapshot() { assertOwnerRejectsEveryIncompleteSnapshot(SnapshotOwner::AutoPush); }
void test_wifi_priority_rejects_incomplete_snapshot() { assertOwnerRejectsEveryIncompleteSnapshot(SnapshotOwner::WifiPriority); }
void test_audio_rejects_incomplete_snapshot() { assertOwnerRejectsEveryIncompleteSnapshot(SnapshotOwner::Audio); }
void test_display_rejects_incomplete_snapshot() { assertOwnerRejectsEveryIncompleteSnapshot(SnapshotOwner::Display); }
void test_display_reset_rejects_incomplete_snapshot() { assertOwnerRejectsEveryIncompleteSnapshot(SnapshotOwner::DisplayReset); }
void test_obd_rejects_incomplete_snapshot() { assertOwnerRejectsEveryIncompleteSnapshot(SnapshotOwner::Obd); }
void test_device_rejects_incomplete_snapshot() { assertOwnerRejectsEveryIncompleteSnapshot(SnapshotOwner::Device); }

void test_snapshot_failure_rejects_each_persist_mode() {
    for (const auto owner : kSnapshotOwners) {
        for (const auto mode : {SettingsPersistMode::Immediate, SettingsPersistMode::ImmediateNvsDeferredBackup,
                                SettingsPersistMode::Deferred}) {
            if (owner == SnapshotOwner::WifiPriority && mode != SettingsPersistMode::Immediate) continue;
            resetRuntimeState();
            SettingsManager manager(storage, profiles);
            seedSnapshotSettings(manager);
            const V1Settings before = manager.get();
            mockStringCopyConstructorFailureSource = &manager.get().apSSID;
            const auto result = mutateSnapshotOwner(manager, owner, mode);
            TEST_ASSERT_EQUAL_UINT(1, mockStringCopyConstructorFailures);
            TEST_ASSERT_FALSE(result.success);
            TEST_ASSERT_FALSE(result.changed);
            assertSnapshotState(before, manager.get());
            TEST_ASSERT_FALSE(manager.deferredPersistPending());
            TEST_ASSERT_EQUAL_UINT32(0, manager.displayConfigurationRevision());
        }
    }
}

void test_settings_nvs_failure_moves_rollback_and_healthy_retry_persists() {
    for (const auto owner : kSnapshotOwners) {
        resetRuntimeState();
        SettingsManager manager(storage, profiles);
        seedSnapshotSettings(manager);
        TEST_ASSERT_TRUE(manager.saveDeferredBackup());
        const V1Settings before = manager.get();
        // Arm only after the final field of the second failed NVS write. The
        // remaining fault proves rollback makes no String copy assignments;
        // it does not assert that a real assignment must allocate.
        unsigned writeAttempts = 0;
        mock_preferences::afterWrite() = [&](const char* key) {
            if (std::strcmp(key, kNvsGpsBaud) == 0 && ++writeAttempts == 2) {
                mockStringCopyAssignmentsBeforeFailure = 0;
            }
        };
        mock_preferences::set_fail_writes_for_key(kNvsVoiceVolume);
        const auto rejected = mutateSnapshotOwner(manager, owner);
        const int remainingAssignmentFault = mockStringCopyAssignmentsBeforeFailure;
        mockStringCopyAssignmentsBeforeFailure = -1;
        mock_preferences::afterWrite() = {};
        mock_preferences::set_fail_writes_for_key(nullptr);
        TEST_ASSERT_FALSE(rejected.success);
        TEST_ASSERT_TRUE(rejected.changed);
        TEST_ASSERT_EQUAL_UINT(2, writeAttempts);
        TEST_ASSERT_EQUAL_INT(0, remainingAssignmentFault);
        assertSnapshotState(before, manager.get());
        SettingsManager beforeRetry(storage, profiles);
        beforeRetry.load();
        assertSnapshotState(before, beforeRetry.get());

        const auto accepted = mutateSnapshotOwner(manager, owner);
        TEST_ASSERT_TRUE(accepted.success);
        TEST_ASSERT_TRUE(accepted.changed);
        SettingsManager reloaded(storage, profiles);
        reloaded.load();
        assertSnapshotState(manager.get(), reloaded.get());
        assertSavedApCredentials(reloaded.get());
    }
}

void test_rejected_settings_mutation_preserves_prior_deferred_retry() {
    for (const auto owner : kSnapshotOwners) {
        for (const bool failSnapshot : {false, true}) {
            resetRuntimeState();
            SettingsManager manager(storage, profiles);
            seedSnapshotSettings(manager);
            TEST_ASSERT_TRUE(manager.saveDeferredBackup());
            DeviceSettingsUpdate prior;
            prior.hasProxyName = true;
            prior.proxyName = "PendingProxyNameForTest";
            TEST_ASSERT_TRUE(manager.applyDeviceSettingsUpdate(prior, SettingsPersistMode::Deferred).success);
            mock_preferences::set_fail_writes_for_key(kNvsVoiceVolume);
            manager.serviceDeferredPersist(manager.deferredPersistNextAttemptAtMs());
            TEST_ASSERT_TRUE(manager.deferredPersistRetryScheduled());
            const auto due = manager.deferredPersistNextAttemptAtMs();
            const auto revision = manager.backupRevision();
            const V1Settings before = manager.get();
            if (failSnapshot) mockStringCopyConstructorFailureSource = &manager.get().apSSID;
            TEST_ASSERT_FALSE(mutateSnapshotOwner(manager, owner).success);
            mockStringCopyConstructorFailureSource = nullptr;
            mock_preferences::set_fail_writes_for_key(nullptr);
            TEST_ASSERT_EQUAL_UINT(failSnapshot ? 1u : 0u, mockStringCopyConstructorFailures);
            assertSnapshotState(before, manager.get());
            TEST_ASSERT_TRUE(manager.deferredPersistPending());
            TEST_ASSERT_TRUE(manager.deferredPersistRetryScheduled());
            TEST_ASSERT_EQUAL_UINT32(due, manager.deferredPersistNextAttemptAtMs());
            TEST_ASSERT_EQUAL_UINT32(revision, manager.backupRevision());
            manager.serviceDeferredPersist(due);
            TEST_ASSERT_FALSE(manager.deferredPersistPending());
            SettingsManager reloaded(storage, profiles);
            reloaded.load();
            assertSnapshotState(before, reloaded.get());
        }
    }
}

void test_wifi_priority_validation_rolls_back_without_string_copy_assignment() {
    const std::vector<WifiStaPriorityUpdate> rejectedUpdates[] = {
        {{0, 1}, {0, 2}}, // Reject the second duplicate slot after changing the first.
        {{0, 1}},         // Reject a final priority collision with untouched slot 1.
    };
    for (const auto& updates : rejectedUpdates) {
        SettingsManager manager(storage, profiles);
        seedSnapshotSettings(manager);
        const V1Settings before = manager.get();
        mockStringCopyAssignmentsBeforeFailure = 0;
        const auto result = manager.applyWifiStaPriorityUpdates(updates);
        const int remainingAssignmentFault = mockStringCopyAssignmentsBeforeFailure;
        mockStringCopyAssignmentsBeforeFailure = -1;
        TEST_ASSERT_FALSE(result.success);
        TEST_ASSERT_EQUAL_INT(0, remainingAssignmentFault);
        assertSnapshotState(before, manager.get());
    }
}

#include "device_delete_read_tests.h"

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_string_constructor_fault_is_targeted_once_and_propagates_invalid_state);
    RUN_TEST(test_audio_http_healthy_save_preserves_credentials);
    RUN_TEST(test_audio_http_nvs_failure_preserves_credentials);
    RUN_TEST(test_audio_http_snapshot_failure_preserves_credentials);
    RUN_TEST(test_audio_http_snapshot_and_nvs_failure_preserve_credentials);
    RUN_TEST(test_active_slot_rejects_incomplete_snapshot);
    RUN_TEST(test_stealth_rejects_incomplete_snapshot);
    RUN_TEST(test_autopush_rejects_incomplete_snapshot);
    RUN_TEST(test_wifi_priority_rejects_incomplete_snapshot);
    RUN_TEST(test_audio_rejects_incomplete_snapshot);
    RUN_TEST(test_display_rejects_incomplete_snapshot);
    RUN_TEST(test_display_reset_rejects_incomplete_snapshot);
    RUN_TEST(test_obd_rejects_incomplete_snapshot);
    RUN_TEST(test_device_rejects_incomplete_snapshot);
    RUN_TEST(test_snapshot_failure_rejects_each_persist_mode);
    RUN_TEST(test_settings_nvs_failure_moves_rollback_and_healthy_retry_persists);
    RUN_TEST(test_rejected_settings_mutation_preserves_prior_deferred_retry);
    RUN_TEST(test_wifi_priority_validation_rolls_back_without_string_copy_assignment);
    RUN_TEST(test_device_delete_ready_read_failure_preserves_prior_acknowledged_intent);
    RUN_TEST(test_device_delete_ready_read_failure_on_fresh_boot_retries_without_loss);
    RUN_TEST(test_device_delete_healthy_lookup_finishes_prior_intent_before_next_delete);
    RUN_TEST(test_device_delete_healthy_fresh_boot_finishes_pending_intent);
    RUN_TEST(test_device_delete_presence_lookup_error_retains_intent);

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
    RUN_TEST(test_device_delete_intent_converges_fallback_and_both_catalog_copies);
    RUN_TEST(test_absent_runtime_namespace_is_initialized_before_delete_recovery);
    RUN_TEST(test_unavailable_runtime_namespace_still_fails_delete_recovery_closed);
    RUN_TEST(test_invalid_runtime_delete_intent_remains_preserved_and_blocking);
    RUN_TEST(test_device_delete_primary_failure_is_durable_pending_and_boot_retries);
    RUN_TEST(test_device_delete_staging_failures_never_publish_intent_or_mutate);
    RUN_TEST(test_device_delete_clears_matching_pending_fallback_but_preserves_other_durable_address);
    RUN_TEST(test_pending_delete_for_another_address_returns_busy_without_accepting_it);
    return UNITY_END();
}
