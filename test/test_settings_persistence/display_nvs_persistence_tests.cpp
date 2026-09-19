#include "display_nvs_persistence_tests.h"

#include <unity.h>

#include "../mocks/Preferences.h"
#include "../mocks/storage_manager.h"
#include "../../include/settings_internals.h"

extern V1ProfileManager profiles;

// applyDisplaySettingsUpdate must sanitize incoming color values the same way
// NVS load and SD restore do. A zero value would black out the affected item.
void test_display_update_rejects_zero_color_keeps_current() {
    SettingsManager manager(storage, profiles);
    const uint16_t originalBogey = manager.get().colorBogey;
    TEST_ASSERT_NOT_EQUAL(0x0000u, originalBogey);

    DisplaySettingsUpdate update;
    update.hasColorBogey = true;
    update.colorBogey = 0x0000;
    manager.applyDisplaySettingsUpdate(update);

    TEST_ASSERT_EQUAL_HEX16(originalBogey, manager.get().colorBogey);
}

void test_display_update_accepts_nonzero_color() {
    SettingsManager manager(storage, profiles);

    DisplaySettingsUpdate update;
    update.hasColorBandKa = true;
    update.colorBandKa = 0x07E0;
    manager.applyDisplaySettingsUpdate(update);

    TEST_ASSERT_EQUAL_HEX16(0x07E0u, manager.get().colorBandKa);
}

void test_v10_six_color_theme_loads_directly() {
    Preferences active;
    TEST_ASSERT_TRUE(active.begin(SETTINGS_NS_A, false));
    TEST_ASSERT_TRUE(active.clear());
    TEST_ASSERT_GREATER_THAN(0, active.putInt(kNvsValid, SETTINGS_VERSION));
    TEST_ASSERT_GREATER_THAN(0, active.putInt(kNvsSettingsVer, 10));
    const uint16_t colors[6] = {0x001F, 0x07E0, 0xFFE0, 0xF800, 0xF81F, 0xFFFF};
    TEST_ASSERT_GREATER_THAN(0, active.putUShort(kNvsLegacyColorBar1, colors[0]));
    TEST_ASSERT_GREATER_THAN(0, active.putUShort(kNvsLegacyColorBar2, colors[1]));
    TEST_ASSERT_GREATER_THAN(0, active.putUShort(kNvsLegacyColorBar3, colors[2]));
    TEST_ASSERT_GREATER_THAN(0, active.putUShort(kNvsLegacyColorBar4, colors[3]));
    TEST_ASSERT_GREATER_THAN(0, active.putUShort(kNvsLegacyColorBar5, colors[4]));
    TEST_ASSERT_GREATER_THAN(0, active.putUShort(kNvsLegacyColorBar6, colors[5]));
    TEST_ASSERT_FALSE(active.isKey(kNvsColorBarSeg1));
    active.end();

    Preferences meta;
    TEST_ASSERT_TRUE(meta.begin(SETTINGS_NS_META, false));
    TEST_ASSERT_GREATER_THAN(0, meta.putString(kNvsMetaActive, SETTINGS_NS_A));
    meta.end();

    SettingsManager manager(storage, profiles);
    manager.load();

    for (int i = 0; i < SIGNAL_BAR_COLOR_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT16(colors[i], manager.get().colorBars[i]);
    }
}

void test_v11_eight_segment_theme_collapses_to_six() {
    Preferences active;
    TEST_ASSERT_TRUE(active.begin(SETTINGS_NS_A, false));
    TEST_ASSERT_TRUE(active.clear());
    TEST_ASSERT_GREATER_THAN(0, active.putInt(kNvsValid, SETTINGS_VERSION));
    TEST_ASSERT_GREATER_THAN(0, active.putInt(kNvsSettingsVer, 11));
    const char* keys[8] = {kNvsColorBarSeg1, kNvsColorBarSeg2, kNvsColorBarSeg3, kNvsColorBarSeg4,
                           kNvsColorBarSeg5, kNvsColorBarSeg6, kNvsColorBarSeg7, kNvsColorBarSeg8};
    const uint16_t segments[8] = {0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007, 0x1008};
    for (int i = 0; i < 8; ++i) {
        TEST_ASSERT_GREATER_THAN(0, active.putUShort(keys[i], segments[i]));
    }
    active.end();

    Preferences meta;
    TEST_ASSERT_TRUE(meta.begin(SETTINGS_NS_META, false));
    TEST_ASSERT_GREATER_THAN(0, meta.putString(kNvsMetaActive, SETTINGS_NS_A));
    meta.end();

    SettingsManager manager(storage, profiles);
    manager.load();
    const uint16_t expected[6] = {0x1001, 0x1002, 0x1004, 0x1005, 0x1007, 0x1008};
    for (int i = 0; i < 6; ++i) {
        TEST_ASSERT_EQUAL_UINT16(expected[i], manager.get().colorBars[i]);
    }
}

void test_v12_direct_colors_win_over_compatibility_shadow() {
    Preferences active;
    TEST_ASSERT_TRUE(active.begin(SETTINGS_NS_A, false));
    TEST_ASSERT_TRUE(active.clear());
    TEST_ASSERT_GREATER_THAN(0, active.putInt(kNvsValid, SETTINGS_VERSION));
    TEST_ASSERT_GREATER_THAN(0, active.putInt(kNvsSettingsVer, SETTINGS_VERSION));
    TEST_ASSERT_GREATER_THAN(0, active.putUShort(kNvsLegacyColorBar1, 0x1111));
    TEST_ASSERT_GREATER_THAN(0, active.putUShort(kNvsLegacyColorBar6, 0x6666));
    TEST_ASSERT_GREATER_THAN(0, active.putUShort(kNvsColorBarSeg1, 0xAAAA));
    TEST_ASSERT_GREATER_THAN(0, active.putUShort(kNvsColorBarSeg8, 0xBBBB));
    active.end();

    Preferences meta;
    TEST_ASSERT_TRUE(meta.begin(SETTINGS_NS_META, false));
    TEST_ASSERT_GREATER_THAN(0, meta.putString(kNvsMetaActive, SETTINGS_NS_A));
    meta.end();

    SettingsManager manager(storage, profiles);
    manager.load();

    TEST_ASSERT_EQUAL_UINT16(0x1111, manager.get().colorBars[0]);
    TEST_ASSERT_EQUAL_UINT16(0x6666, manager.get().colorBars[5]);
}

void test_fresh_install_seeds_six_physical_defaults() {
    Preferences active;
    TEST_ASSERT_TRUE(active.begin(SETTINGS_NS_A, false));
    TEST_ASSERT_TRUE(active.clear());
    TEST_ASSERT_GREATER_THAN(0, active.putInt(kNvsValid, SETTINGS_VERSION));
    TEST_ASSERT_GREATER_THAN(0, active.putInt(kNvsSettingsVer, SETTINGS_VERSION));
    TEST_ASSERT_FALSE(active.isKey(kNvsColorBarSeg1));
    TEST_ASSERT_FALSE(active.isKey(kNvsLegacyColorBar1));
    active.end();

    Preferences meta;
    TEST_ASSERT_TRUE(meta.begin(SETTINGS_NS_META, false));
    TEST_ASSERT_GREATER_THAN(0, meta.putString(kNvsMetaActive, SETTINGS_NS_A));
    meta.end();

    SettingsManager manager(storage, profiles);
    manager.load();
    const uint16_t expected[6] = {0x07E0, 0x07E0, 0xFFE0, 0xFFE0, 0xF800, 0xF800};
    for (int i = 0; i < 6; ++i) {
        TEST_ASSERT_EQUAL_UINT16(expected[i], manager.get().colorBars[i]);
    }
}
