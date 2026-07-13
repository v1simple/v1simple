#include <unity.h>

#include "../mocks/settings.h"

#ifndef ARDUINO
SerialClass Serial;
#endif

class ObdRuntimeModule {
public:
    void setEnabled(bool enabled) {
        ++setEnabledCalls;
        lastEnabled = enabled;
    }

    void setMinRssi(int8_t minRssi) {
        ++setMinRssiCalls;
        lastMinRssi = minRssi;
    }

    int setEnabledCalls = 0;
    int setMinRssiCalls = 0;
    bool lastEnabled = false;
    int8_t lastMinRssi = 0;
};

class SpeedSourceSelector {
public:
    void syncEnabledInputs(bool obdEnabled, bool gpsEnabled = false) {
        ++syncEnabledInputsCalls;
        lastObdEnabled = obdEnabled;
        lastGpsEnabled = gpsEnabled;
    }

    int syncEnabledInputsCalls = 0;
    bool lastObdEnabled = false;
    bool lastGpsEnabled = false;
};

#include "../../src/settings_runtime_sync.h"

void setUp() {}
void tearDown() {}

void test_sync_obd_runtime_settings_applies_enabled_and_min_rssi() {
    V1Settings settings;
    settings.obdEnabled = true;
    settings.obdMinRssi = -55;
    ObdRuntimeModule obdRuntime;

    SettingsRuntimeSync::syncObdRuntimeSettings(settings, obdRuntime);

    TEST_ASSERT_EQUAL_INT(1, obdRuntime.setEnabledCalls);
    TEST_ASSERT_TRUE(obdRuntime.lastEnabled);
    TEST_ASSERT_EQUAL_INT(1, obdRuntime.setMinRssiCalls);
    TEST_ASSERT_EQUAL_INT8(-55, obdRuntime.lastMinRssi);
}

void test_sync_obd_vehicle_runtime_settings_updates_obd_and_selector() {
    V1Settings settings;
    settings.obdEnabled = true;
    settings.obdMinRssi = -63;
    ObdRuntimeModule obdRuntime;
    SpeedSourceSelector speedSourceSelector;

    SettingsRuntimeSync::syncObdVehicleRuntimeSettings(settings,
                                                       obdRuntime,
                                                       speedSourceSelector);

    TEST_ASSERT_EQUAL_INT(1, obdRuntime.setEnabledCalls);
    TEST_ASSERT_TRUE(obdRuntime.lastEnabled);
    TEST_ASSERT_EQUAL_INT(1, obdRuntime.setMinRssiCalls);
    TEST_ASSERT_EQUAL_INT8(-63, obdRuntime.lastMinRssi);
    TEST_ASSERT_EQUAL_INT(1, speedSourceSelector.syncEnabledInputsCalls);
    TEST_ASSERT_TRUE(speedSourceSelector.lastObdEnabled);
}

void test_sync_speed_source_selector_inputs_uses_persisted_inputs() {
    V1Settings settings;
    settings.obdEnabled = true;
    SpeedSourceSelector speedSourceSelector;

    SettingsRuntimeSync::syncSpeedSourceSelectorInputs(settings, speedSourceSelector);

    TEST_ASSERT_EQUAL_INT(1, speedSourceSelector.syncEnabledInputsCalls);
    TEST_ASSERT_TRUE(speedSourceSelector.lastObdEnabled);
}

void test_sync_speed_source_selector_inputs_propagates_gps_enabled() {
    V1Settings settings;
    settings.obdEnabled = false;
    settings.gpsEnabled = true;
    SpeedSourceSelector speedSourceSelector;

    SettingsRuntimeSync::syncSpeedSourceSelectorInputs(settings, speedSourceSelector);

    TEST_ASSERT_EQUAL_INT(1, speedSourceSelector.syncEnabledInputsCalls);
    TEST_ASSERT_FALSE(speedSourceSelector.lastObdEnabled);
    TEST_ASSERT_TRUE(speedSourceSelector.lastGpsEnabled);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_sync_obd_runtime_settings_applies_enabled_and_min_rssi);
    RUN_TEST(test_sync_obd_vehicle_runtime_settings_updates_obd_and_selector);
    RUN_TEST(test_sync_speed_source_selector_inputs_uses_persisted_inputs);
    RUN_TEST(test_sync_speed_source_selector_inputs_propagates_gps_enabled);
    return UNITY_END();
}
