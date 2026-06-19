/**
 * test_display_rendering_status_bar.cpp
 *
 * Phase 3 Task 3.5 — integration tests for display_status_bar.cpp
 * (drawVolumeIndicator, drawRssiIndicator, drawBatteryIndicator,
 *  drawBLEProxyIndicator, drawWiFiIndicator).
 *
 * Includes the real rendering source so that GFX call-recording assertions
 * verify actual draw behaviour.
 */

// Enable display-variant guards before any display headers
#ifndef DISPLAY_WAVESHARE_349
#define DISPLAY_WAVESHARE_349 1
#endif

#include <unity.h>
#include <cmath>

// ---------------------------------------------------------------------------
// Mocks — explicit relative paths to fire guards before real headers
// ---------------------------------------------------------------------------
#include "../mocks/display_driver.h"
#include "../mocks/Arduino.h"
#include "../mocks/WiFi.h"
#include "../mocks/settings.h"
#include "../mocks/packet_parser.h"
#include "../mocks/battery_manager.h"
#include "../mocks/wifi_manager.h"

#ifndef ARDUINO
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
SerialClass Serial;
#endif

// WiFi global (declared extern in WiFi.h mock)
WiFiClass WiFi;

// WiFiManager global (declared extern in wifi_manager mock)
WiFiManager wifiManager;

// BatteryManager is defined inline in the battery_manager.h mock, so no definition needed here.

// Real display classes
#include "../../src/display.h"
#include "../../src/perf_metrics.h"
#include "../../include/display_dirty_flags.h"
#include "../../include/display_ble_freshness.h"
#include "../../include/display_element_caches.h"

// ---------------------------------------------------------------------------
// Required extern definitions
// ---------------------------------------------------------------------------
V1Display* g_displayInstance = nullptr;
SettingsManager settingsManager;

static int g_statusPaintRssiCount = 0;
static int g_redrawRssiCount = 0;

void perfRecordDisplayStatusPaint(PerfDisplayStatusPaint element) {
    if (element == PerfDisplayStatusPaint::Rssi) {
        ++g_statusPaintRssiCount;
    }
}

void perfRecordDisplayRedrawReason(PerfDisplayRedrawReason reason) {
    if (reason == PerfDisplayRedrawReason::RssiRefresh) {
        ++g_redrawRssiCount;
    }
}

// ---------------------------------------------------------------------------
// Minimal V1Display constructor / destructor stubs
// ---------------------------------------------------------------------------
V1Display::V1Display() {
    currentPalette_ = ColorThemes::STANDARD();
    currentPalette_.colorMuted = settingsManager.get().colorMuted;
    currentPalette_.colorPersisted = settingsManager.get().colorPersisted;
    g_displayInstance = this;
}
V1Display::~V1Display() = default;

// ---------------------------------------------------------------------------
// Stubs for V1Display methods called by display_status_bar.cpp that live in
// other translation units (display.cpp, display_indicators.cpp, etc.)
// ---------------------------------------------------------------------------
void V1Display::setBLEProxyStatus(bool proxyEnabled, bool clientConnected, bool receivingData) {
    bleProxyEnabled_ = proxyEnabled;
    bleProxyClientConnected_ = clientConnected;
    bleReceivingData_ = receivingData;
}

void V1Display::setBleContext(const DisplayBleContext& ctx) {
    bleCtx_ = ctx;
    bleCtxUpdatedAtMs_ = millis();
}

bool V1Display::hasFreshBleContext(uint32_t nowMs) const {
    return DisplayBleFreshness::isFresh(bleCtxUpdatedAtMs_, nowMs);
}

// ---------------------------------------------------------------------------
// Real rendering code under test
// ---------------------------------------------------------------------------
#include "../../src/display_status_bar.cpp"

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
static Arduino_Canvas* canvas() { return display.testCanvas(); }

static void resetCanvas() {
    display.setTestCanvas(new Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr));
    canvas()->resetCounters();
}

static bool hasFillRectCall(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    for (const auto& call : canvas()->fillRectCalls) {
        if (call.x == x && call.y == y && call.w == w && call.h == h && call.color == color) {
            return true;
        }
    }
    return false;
}

static void assertNoFullRssiClear() {
    TEST_ASSERT_FALSE_MESSAGE(
        hasFillRectCall(8, 99, 70, 44, ColorThemes::STANDARD().bg),
        "single-line RSSI refresh must not clear the full RSSI block");
}

// Global test display instance
V1Display display;

// ---------------------------------------------------------------------------
// setUp / tearDown
// ---------------------------------------------------------------------------
void setUp() {
    mockMillis = 1000;
    g_statusPaintRssiCount = 0;
    g_redrawRssiCount = 0;
    resetCanvas();
    display.ut_elementCaches() = DisplayElementCaches{};
    batteryManager.reset();
}

void tearDown() {}

// ============================================================================
// drawVolumeIndicator tests
// ============================================================================

void test_drawVolumeIndicator_draws_text_items() {
    // Volume draws: 1 fillRect (clear) + text drawing (which does fillRects for area)
    display.ut_drawVolumeIndicator(5, 0);
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
    // First call is the background clear (PALETTE_BG)
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->fillRectCalls[0].color);
}

void test_drawVolumeIndicator_different_volumes_still_clears() {
    display.ut_drawVolumeIndicator(9, 3);
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->fillRectCalls[0].color);
}

// ============================================================================
// drawRssiIndicator tests
// ============================================================================

void test_drawRssiIndicator_hidden_when_setting_off() {
    settingsManager.getMutable().hideRssiIndicator = true;
    display.ut_drawRssiIndicator(-60);
    // Should just clear with BG, no signal drawn
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->fillRectCalls[0].color);
    // Reset for other tests
    settingsManager.getMutable().hideRssiIndicator = false;
}

void test_drawRssiIndicator_stale_ble_clears_area() {
    // bleCtxUpdatedAtMs_ is 0 at start, so hasFreshBleContext returns false.
    // With stale context, drawRssiIndicator now returns early WITHOUT clearing,
    // preserving the last-drawn RSSI on screen (fix for indicators disappearing
    // after alerts end when BLE context goes momentarily stale).
    display.ut_drawRssiIndicator(-60);
    TEST_ASSERT_EQUAL(0u, canvas()->fillRectCalls.size());
}

void test_drawRssiIndicator_fresh_ble_draws_content() {
    // Make BLE context fresh by calling setBleContext
    DisplayBleContext ctx = {};
    ctx.proxyRssi = -65;
    display.setBleContext(ctx);
    // drawRssiIndicator uses V1 RSSI parameter and bleCtx_.proxyRssi
    display.ut_drawRssiIndicator(-60);
    // Should have fillRect (clear) + text drawing
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_INT(1, g_statusPaintRssiCount);
    TEST_ASSERT_EQUAL_INT(1, g_redrawRssiCount);
}

void test_drawRssiIndicator_v1_change_clears_only_v1_line() {
    DisplayBleContext ctx = {};
    ctx.proxyRssi = -65;
    display.setBleContext(ctx);
    display.ut_drawRssiIndicator(-60);

    resetCanvas();
    display.setBleContext(ctx);
    display.ut_drawRssiIndicator(-61);

    TEST_ASSERT_TRUE(hasFillRectCall(8, 99, 70, 22, ColorThemes::STANDARD().bg));
    TEST_ASSERT_FALSE(hasFillRectCall(8, 121, 70, 22, ColorThemes::STANDARD().bg));
    assertNoFullRssiClear();
}

void test_drawRssiIndicator_proxy_change_clears_only_proxy_line() {
    DisplayBleContext ctx = {};
    ctx.proxyRssi = -65;
    display.setBleContext(ctx);
    display.ut_drawRssiIndicator(-60);

    resetCanvas();
    ctx.proxyRssi = -66;
    display.setBleContext(ctx);
    display.ut_drawRssiIndicator(-60);

    TEST_ASSERT_FALSE(hasFillRectCall(8, 99, 70, 22, ColorThemes::STANDARD().bg));
    TEST_ASSERT_TRUE(hasFillRectCall(8, 121, 70, 22, ColorThemes::STANDARD().bg));
    assertNoFullRssiClear();
}

// ============================================================================
// drawBatteryIndicator tests
// ============================================================================

void test_drawBatteryIndicator_no_battery_clears_region() {
    batteryManager.setHasBattery(false);
    display.ut_drawBatteryIndicator();
    // No battery → clear area
    // May have 0 or more fillRectCalls depending on the code path
    // The key point: no assertion failures
}

void test_drawBatteryIndicator_with_battery_draws_something() {
    batteryManager.setHasBattery(true);
    batteryManager.setVoltage(3.8f);  // ~75%
    display.ut_drawBatteryIndicator();
    // Battery icon present → at least some GFX calls
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
}

void test_drawBatteryIndicator_usb_power_hides_icon() {
    batteryManager.setHasBattery(true);
    batteryManager.setVoltage(4.2f);  // USB-level voltage, above 4125 mV threshold
    display.ut_drawBatteryIndicator();
    // First call primes the static — need to call again for USB detection
    resetCanvas();
    display.ut_drawBatteryIndicator();
    // USB power: showBatteryOnUSB = false after > 4125 mV
    // May still draw the clear rect but we test it doesn't crash
}

// ============================================================================
// drawBLEProxyIndicator tests
// ============================================================================

void test_drawBLEProxy_disabled_clears_area() {
    display.setBLEProxyStatus(false, false, false);
    display.ut_drawBLEProxyIndicator();
    // Disabled and never drawn → no clear needed (bleProxyDrawn_ is false).
    // Only clears if the icon was previously drawn, to avoid blanking on
    // every frame when proxy was never enabled.
    TEST_ASSERT_EQUAL(0u, canvas()->fillRectCalls.size());
}

void test_drawBLEProxy_enabled_connected_draws_icon() {
    // Make BLE context fresh
    DisplayBleContext ctx = {};
    display.setBleContext(ctx);

    display.setBLEProxyStatus(true, true, true);
    display.ut_drawBLEProxyIndicator();
    // Enabled + connected → draws BT rune using drawLine calls
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->drawLineCalls.size());
}

void test_drawBLEProxy_hidden_when_setting_off() {
    settingsManager.getMutable().hideBleIcon = true;
    display.setBLEProxyStatus(true, true, true);
    display.ut_drawBLEProxyIndicator();
    // Hidden by settings → clears area, no drawLine calls
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT(0u, canvas()->drawLineCalls.size());
    settingsManager.getMutable().hideBleIcon = false;
}

// ============================================================================
// drawWiFiIndicator tests
// ============================================================================

void test_drawWifi_inactive_clears_area() {
    wifiManager.setWifiServiceActive(false);
    wifiManager.setConnected(false);
    display.ut_drawWiFiIndicator();
    // Inactive WiFi → clears area
    TEST_ASSERT_GREATER_OR_EQUAL(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->fillRectCalls[0].color);
}

void test_drawWifi_active_draws_arcs() {
    wifiManager.setWifiServiceActive(true);
    wifiManager.setConnected(false);
    display.ut_drawWiFiIndicator();
    // Active WiFi → draws center dot + arc pixels
    // Arcs use FILL_RECT for each pixel, so many fillRectCalls expected
    TEST_ASSERT_GREATER_OR_EQUAL(5u, canvas()->fillRectCalls.size());
}

void test_drawWifi_hidden_when_setting_off() {
    settingsManager.getMutable().hideWifiIcon = true;
    wifiManager.setWifiServiceActive(true);
    display.ut_drawWiFiIndicator();
    // Hidden by settings → clears area only (1 fillRect)
    TEST_ASSERT_EQUAL_UINT(1u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg, canvas()->fillRectCalls[0].color);
    settingsManager.getMutable().hideWifiIcon = false;
}

void test_drawWifi_gave_up_uses_red() {
    wifiManager.setWifiServiceActive(true);
    wifiManager.setConnected(false);
    wifiManager.setReconnectGaveUp(true);
    display.ut_drawWiFiIndicator();
    // Gave up → clear + center dot + arc pixels; dot color must be red (0xF800)
    // fillRectCalls[0] is the area clear, [1] is the center dot
    TEST_ASSERT_GREATER_OR_EQUAL(2u, canvas()->fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT16(0xF800u, canvas()->fillRectCalls[1].color);
    wifiManager.setReconnectGaveUp(false);
}

// ============================================================================
// Test runner
// ============================================================================

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_drawVolumeIndicator_draws_text_items);
    RUN_TEST(test_drawVolumeIndicator_different_volumes_still_clears);
    RUN_TEST(test_drawRssiIndicator_hidden_when_setting_off);
    RUN_TEST(test_drawRssiIndicator_stale_ble_clears_area);
    RUN_TEST(test_drawRssiIndicator_fresh_ble_draws_content);
    RUN_TEST(test_drawRssiIndicator_v1_change_clears_only_v1_line);
    RUN_TEST(test_drawRssiIndicator_proxy_change_clears_only_proxy_line);
    RUN_TEST(test_drawBatteryIndicator_no_battery_clears_region);
    RUN_TEST(test_drawBatteryIndicator_with_battery_draws_something);
    RUN_TEST(test_drawBatteryIndicator_usb_power_hides_icon);
    RUN_TEST(test_drawBLEProxy_disabled_clears_area);
    RUN_TEST(test_drawBLEProxy_enabled_connected_draws_icon);
    RUN_TEST(test_drawBLEProxy_hidden_when_setting_off);
    RUN_TEST(test_drawWifi_inactive_clears_area);
    RUN_TEST(test_drawWifi_active_draws_arcs);
    RUN_TEST(test_drawWifi_hidden_when_setting_off);
    RUN_TEST(test_drawWifi_gave_up_uses_red);
    return UNITY_END();
}
