#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/settings.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

SettingsManager settingsManager;

#include "../../src/modules/obd/obd_elm327_parser.cpp"
#include "../../src/modules/obd/obd_runtime_module.cpp"
#include "../../src/modules/obd/obd_runtime_transport.cpp"
#include "../../src/modules/obd/obd_runtime_commands.cpp"
#include "../../src/modules/obd/obd_runtime_state_machine.cpp"
#include "../../src/modules/obd/obd_settings_sync_module.cpp"

static ObdSettingsSyncModule module;

static void resetState() {
    settingsManager = SettingsManager();
    obdRuntimeModule = ObdRuntimeModule();
    module = ObdSettingsSyncModule();
    module.begin(&settingsManager, &obdRuntimeModule);
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_process_skips_save_when_runtime_already_matches_settings() {
    V1Settings& settings = settingsManager.mutableSettings();
    settings.obdSavedAddress = "A4:C1:38:00:11:22";
    settings.obdSavedAddrType = 1;

    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 1, -80);

    module.process(1000);

    TEST_ASSERT_EQUAL_INT(0, settingsManager.requestDeferredPersistCalls);
    TEST_ASSERT_FALSE(module.hasPendingSnapshotForTest());
}

void test_process_saves_once_after_runtime_values_stabilize() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 2, -80);

    module.process(1000);
    TEST_ASSERT_TRUE(module.hasPendingSnapshotForTest());
    TEST_ASSERT_EQUAL_UINT32(1000, module.getPendingChangedAtMsForTest());
    TEST_ASSERT_EQUAL_INT(0, settingsManager.requestDeferredPersistCalls);

    module.process(5999);
    TEST_ASSERT_EQUAL_INT(0, settingsManager.requestDeferredPersistCalls);

    module.process(6000);

    const V1Settings& settings = settingsManager.get();
    TEST_ASSERT_EQUAL_INT(1, settingsManager.requestDeferredPersistCalls);
    TEST_ASSERT_FALSE(module.hasPendingSnapshotForTest());
    TEST_ASSERT_EQUAL_STRING("A4:C1:38:00:11:22", settings.obdSavedAddress.c_str());
    TEST_ASSERT_EQUAL_UINT8(2, settings.obdSavedAddrType);
}

void test_process_resets_debounce_when_runtime_keeps_changing() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 1, -80);
    module.process(1000);

    obdRuntimeModule.begin(nullptr, true, "B4:C1:38:00:11:33", 3, -80);
    module.process(4000);

    TEST_ASSERT_TRUE(module.hasPendingSnapshotForTest());
    TEST_ASSERT_EQUAL_UINT32(4000, module.getPendingChangedAtMsForTest());
    TEST_ASSERT_EQUAL_INT(0, settingsManager.requestDeferredPersistCalls);

    module.process(8999);
    TEST_ASSERT_EQUAL_INT(0, settingsManager.requestDeferredPersistCalls);

    module.process(9000);

    const V1Settings& settings = settingsManager.get();
    TEST_ASSERT_EQUAL_INT(1, settingsManager.requestDeferredPersistCalls);
    TEST_ASSERT_EQUAL_STRING("B4:C1:38:00:11:33", settings.obdSavedAddress.c_str());
    TEST_ASSERT_EQUAL_UINT8(3, settings.obdSavedAddrType);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_process_skips_save_when_runtime_already_matches_settings);
    RUN_TEST(test_process_saves_once_after_runtime_values_stabilize);
    RUN_TEST(test_process_resets_debounce_when_runtime_keeps_changing);
    return UNITY_END();
}
