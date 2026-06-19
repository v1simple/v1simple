/**
 * Settings slider methods — extracted from display.cpp (Phase 3B)
 *
 * Contains showSettingsSliders, updateSettingsSliders,
 * getActiveSliderFromTouch, hideBrightnessSlider.
 */

#include "display.h"
#include "display_layout.h"
#include "display_palette.h"
#include "display_flush.h"
#include "display_slider_math.h"

// Slider-specific color constants (not theme-dependent — fixed hardware UI)
static constexpr uint16_t SLIDER_WHITE         = 0xFFFF;  // Labels and thumb
static constexpr uint16_t SLIDER_TRACK_BORDER  = 0x4208;  // Outer border of track
static constexpr uint16_t SLIDER_TRACK_FILL    = 0x2104;  // Empty track fill
static constexpr uint16_t SLIDER_BRIGHTNESS_FG = 0x07E0;  // Green — brightness fill
static constexpr uint16_t SLIDER_VOLUME_FG     = 0x001F;  // Blue — volume fill
static constexpr uint16_t SLIDER_HINT_TEXT     = 0x8410;  // Medium gray — hint text

// Settings screen with brightness and voice volume sliders.
void V1Display::showSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel) {
    // Clear screen to dark background
    tft_->fillScreen(PALETTE_BG);

    // Layout: 640x172 landscape - two horizontal sliders stacked.
    const int sliderMargin = 40;
    const int sliderHeight = 10;
    const int sliderWidth = SCREEN_WIDTH - (sliderMargin * 2);  // 560 pixels
    const int sliderX = sliderMargin;
    const int brightnessY = 45;
    const int volumeY = 115;

    // Title
    tft_->setTextColor(SLIDER_WHITE);
    tft_->setTextSize(2);
    tft_->setCursor((SCREEN_WIDTH - 120) / 2, 5);
    tft_->print("SETTINGS");

    // ============================================================================
    // Brightness slider
    // ============================================================================
    tft_->setTextSize(1);
    tft_->setTextColor(SLIDER_WHITE);
    tft_->setCursor(sliderMargin, brightnessY - 16);
    tft_->print("BRIGHTNESS");

    // Draw slider track
    tft_->drawRect(sliderX - 2, brightnessY - 2, sliderWidth + 4, sliderHeight + 4, SLIDER_TRACK_BORDER);
    tft_->fillRect(sliderX, brightnessY, sliderWidth, sliderHeight, SLIDER_TRACK_FILL);

    // Fill based on brightness level (80-255 range)
    int brightnessFill = computeBrightnessSliderFill(brightnessLevel, sliderWidth);
    tft_->fillRect(sliderX, brightnessY, brightnessFill, sliderHeight, SLIDER_BRIGHTNESS_FG);  // Green

    // Thumb
    int brightThumbX = sliderX + brightnessFill - 4;
    if (brightThumbX < sliderX) brightThumbX = sliderX;
    if (brightThumbX > sliderX + sliderWidth - 8) brightThumbX = sliderX + sliderWidth - 8;
    tft_->fillRect(brightThumbX, brightnessY - 4, 8, sliderHeight + 8, SLIDER_WHITE);

    // Percentage text
    char brightStr[8];
    int brightPercent = computeBrightnessSliderPercent(brightnessLevel);
    snprintf(brightStr, sizeof(brightStr), "%d%%", brightPercent);
    tft_->setCursor(sliderX + sliderWidth + 8, brightnessY);
    tft_->print(brightStr);

    // ============================================================================
    // Voice volume slider
    // ============================================================================
    tft_->setTextColor(SLIDER_WHITE);
    tft_->setCursor(sliderMargin, volumeY - 16);
    tft_->print("VOICE VOLUME");

    // Draw slider track
    tft_->drawRect(sliderX - 2, volumeY - 2, sliderWidth + 4, sliderHeight + 4, SLIDER_TRACK_BORDER);
    tft_->fillRect(sliderX, volumeY, sliderWidth, sliderHeight, SLIDER_TRACK_FILL);

    // Fill based on volume level (0-100 range)
    const int volumeFill = (static_cast<int>(volumeLevel) * sliderWidth) / 100;
    tft_->fillRect(sliderX, volumeY, volumeFill, sliderHeight, SLIDER_VOLUME_FG);

    // Thumb
    int volumeThumbX = sliderX + volumeFill - 4;
    if (volumeThumbX < sliderX) volumeThumbX = sliderX;
    if (volumeThumbX > sliderX + sliderWidth - 8) volumeThumbX = sliderX + sliderWidth - 8;
    tft_->fillRect(volumeThumbX, volumeY - 4, 8, sliderHeight + 8, SLIDER_WHITE);

    // Percentage text
    char volumeStr[8];
    snprintf(volumeStr, sizeof(volumeStr), "%d%%", volumeLevel);
    tft_->setCursor(sliderX + sliderWidth + 8, volumeY);
    tft_->print(volumeStr);

    // Instructions at bottom
    tft_->setTextSize(1);
    tft_->setTextColor(SLIDER_HINT_TEXT);  // Medium gray
    tft_->setCursor((SCREEN_WIDTH - 220) / 2, 155);
    tft_->print("Touch sliders - BOOT to save");

    DISPLAY_FLUSH();
}

void V1Display::updateSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel, int activeSlider) {
    (void)activeSlider;
    // Apply brightness in real-time for visual feedback
    setBrightness(brightnessLevel);
    showSettingsSliders(brightnessLevel, volumeLevel);
}

// Returns which slider was touched: 0=brightness, 1=volume, -1=none.
// Touch Y is inverted relative to display Y:
//   Low touch Y = bottom of display = volume slider
//   High touch Y = top of display = brightness slider
int V1Display::getActiveSliderFromTouch(int16_t touchY) {
    if (touchY <= 60) return 1;
    if (touchY >= 80) return 0;
    return -1;
}

void V1Display::hideBrightnessSlider() {
    // Just clear - caller will refresh normal display
    clear();
}
