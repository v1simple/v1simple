// Full status-bar implementation, with the classic font's actual A/W glyphs
// and 6x8 advance/line metrics. This models framebuffer pixels, not a panel.
#ifndef DISPLAY_WAVESHARE_349
#define DISPLAY_WAVESHARE_349 1
#endif
#include <unity.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include "../mocks/display_driver.h"
#include "../mocks/settings.h"
#include "../mocks/battery_manager.h"
#include "../../src/display.h"

// These unrelated status indicators are compiled but not exercised here.
#define WIFI_MANAGER_H
class WiFiManager {
  public:
    bool isWifiServiceActive() const { return false; }
    bool isConnected() const { return false; }
    bool isReconnectGaveUp() const { return false; }
};

SerialClass Serial;
unsigned long mockMillis = 1000;
unsigned long mockMicros = 0;
V1Display* g_displayInstance = nullptr;
V1Display::V1Display(SettingsManager& settings) : settings_(settings) {
    currentPalette_ = ColorThemes::STANDARD();
}
V1Display::~V1Display() = default;
bool V1Display::hasFreshBleContext(uint32_t) const { return false; }
#include "../../src/display_status_bar.cpp"

class ProfileCanvas : public Arduino_Canvas {
  public:
    ProfileCanvas() : Arduino_Canvas(SCREEN_WIDTH, SCREEN_HEIGHT, nullptr), pixels(SCREEN_WIDTH * SCREEN_HEIGHT) {}
    std::vector<uint16_t> pixels;
    std::string lastText;
    int prints = 0;
    int textSize = 1;
    int cursorX = 0;
    int cursorY = 0;
    int lastTextX = 0;
    int lastTextWidth = 0;
    uint16_t foreground = 0xffff;
    uint16_t background = 0;

    void setCursor(int16_t x, int16_t y) override { cursorX = x; cursorY = y; }
    void setTextSize(uint8_t size) override { textSize = size; }
    void setTextColor(uint16_t fg, uint16_t bg) override { foreground = fg; background = bg; }
    void getTextBounds(const char* text, int16_t x, int16_t y, int16_t* x1, int16_t* y1,
                       uint16_t* w, uint16_t* h) override {
        *x1 = x; *y1 = y; *w = strlen(text) * 6 * textSize; *h = 8 * textSize;
    }
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
        Arduino_Canvas::fillRect(x, y, w, h, color);
        for (int yy = std::max<int>(0, y); yy < std::min<int>(SCREEN_HEIGHT, y + h); ++yy) {
            for (int xx = std::max<int>(0, x); xx < std::min<int>(SCREEN_WIDTH, x + w); ++xx) {
                pixels[yy * SCREEN_WIDTH + xx] = color;
            }
        }
    }
    void print(const char* text) override {
        ++prints;
        lastText = text;
        lastTextX = cursorX;
        lastTextWidth = strlen(text) * 6 * textSize;
        // Arduino_GFX 1.6.7 src/font/glcdfont.h (classic Adafruit 5x7 font).
        static constexpr uint8_t glyphA[] = {0x7c, 0x12, 0x11, 0x12, 0x7c};
        static constexpr uint8_t glyphW[] = {0x3f, 0x40, 0x38, 0x40, 0x3f};
        for (; *text; ++text, cursorX += 6 * textSize) {
            TEST_ASSERT_TRUE(*text == 'A' || *text == 'W');
            const uint8_t* glyph = *text == 'A' ? glyphA : glyphW;
            for (int col = 0; col < 6; ++col) {
                const uint8_t bits = col < 5 ? glyph[col] : 0;
                for (int row = 0; row < 8; ++row) {
                    fillRect(cursorX + col * textSize, cursorY + row * textSize, textSize, textSize,
                             bits & (1 << row) ? foreground : background);
                }
            }
        }
    }
    int foregroundOutsideProfile() const {
        const auto region = DisplayLayout::profileRect();
        int count = 0;
        for (int y = region.y; y < region.y + region.h; ++y) {
            // The battery owns the area to the right. The original residue was
            // to the left, where no later status painter repairs the pixels.
            for (int x = 0; x < region.x; ++x) {
                count += pixels[y * SCREEN_WIDTH + x] != 0;
            }
        }
        return count;
    }
};

void setUp() { mockMillis = 1000; }
void tearDown() {}

static void configure(SettingsManager& settings) {
    settings.settings.slot0Name = "WWWWWWWWWWWWWWWWWWWW";
    settings.settings.slot1Name = "A";
    settings.settings.slot0Color = 0xffff;
    settings.settings.slot1Color = 0xffff;
}

void test_long_profile_fits_and_short_change_leaves_no_old_pixels() {
    SettingsManager settings;
    configure(settings);
    V1Display display(settings);
    auto* canvas = new ProfileCanvas;
    display.setTestCanvas(canvas);
    display.drawProfileIndicator(0);
    const auto region = DisplayLayout::profileRect();
    TEST_ASSERT_EQUAL_STRING("WWWWWWWWWWWWWWWWWWWW", canvas->lastText.c_str());
    TEST_ASSERT_EQUAL_INT(120, canvas->lastTextWidth);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(region.x, canvas->lastTextX);
    TEST_ASSERT_LESS_OR_EQUAL_INT(region.x + region.w, canvas->lastTextX + canvas->lastTextWidth);
    TEST_ASSERT_EQUAL_INT(0, canvas->foregroundOutsideProfile());

    display.drawProfileIndicator(1);
    TEST_ASSERT_EQUAL_STRING("A", canvas->lastText.c_str());
    TEST_ASSERT_EQUAL_INT(2, canvas->textSize);
    TEST_ASSERT_EQUAL_INT(0, canvas->foregroundOutsideProfile());
    const auto afterShort = canvas->pixels;
    const int prints = canvas->prints;
    display.drawProfileIndicator(1);
    TEST_ASSERT_EQUAL_INT(prints, canvas->prints);
    TEST_ASSERT_TRUE(afterShort == canvas->pixels);

    // A changed frame must equal a fresh short-name draw, including old ink
    // inside the indicator rectangle; a mere transfer of stale pixels fails.
    V1Display fresh(settings);
    auto* expected = new ProfileCanvas;
    fresh.setTestCanvas(expected);
    fresh.drawProfileIndicator(1);
    TEST_ASSERT_TRUE(expected->pixels == canvas->pixels);
}

void test_hiding_long_profile_clears_all_of_its_ink() {
    SettingsManager settings;
    configure(settings);
    settings.settings.hideProfileIndicator = true;
    V1Display display(settings);
    auto* canvas = new ProfileCanvas;
    display.setTestCanvas(canvas);
    display.drawProfileIndicator(0);
    mockMillis += 10000;
    display.drawProfileIndicator(0);
    TEST_ASSERT_EQUAL_INT(0, canvas->foregroundOutsideProfile());
    const auto region = DisplayLayout::profileRect();
    for (int y = region.y; y < region.y + region.h; ++y) {
        for (int x = region.x; x < region.x + region.w; ++x) {
            TEST_ASSERT_EQUAL_UINT16(0, canvas->pixels[y * SCREEN_WIDTH + x]);
        }
    }
    const auto hidden = canvas->pixels;
    display.drawProfileIndicator(0);
    TEST_ASSERT_TRUE(hidden == canvas->pixels);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_long_profile_fits_and_short_change_leaves_no_old_pixels);
    RUN_TEST(test_hiding_long_profile_clears_all_of_its_ink);
    return UNITY_END();
}
