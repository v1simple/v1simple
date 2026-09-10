/**
 * test_color_palette.cpp
 *
 * Pure-logic tests for color_themes.h. A base or partially initialized palette
 * must keep muted and persisted alerts visible before settings are available.
 */

#include <unity.h>

#include "../../include/color_themes.h"

void setUp() {}
void tearDown() {}

void test_standard_palette_keeps_subdued_alerts_visible() {
    const ColorPalette& palette = ColorThemes::STANDARD();

    TEST_ASSERT_NOT_EQUAL(palette.bg, palette.colorMuted);
    TEST_ASSERT_NOT_EQUAL(palette.bg, palette.colorPersisted);
}

void test_default_palette_keeps_subdued_alerts_visible() {
    const ColorPalette palette{};

    TEST_ASSERT_NOT_EQUAL(palette.bg, palette.colorMuted);
    TEST_ASSERT_NOT_EQUAL(palette.bg, palette.colorPersisted);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_standard_palette_keeps_subdued_alerts_visible);
    RUN_TEST(test_default_palette_keeps_subdued_alerts_visible);
    return UNITY_END();
}
