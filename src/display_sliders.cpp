#include "display.h"
#include "display_layout.h"
#include "display_palette.h"
#include "display_flush.h"
#include "display_slider_math.h"

// Fixed hardware UI colors; these do not follow the alert theme.
static constexpr uint16_t SLIDER_WHITE = 0xFFFF;
static constexpr uint16_t SLIDER_TRACK_BORDER = 0x4208;
static constexpr uint16_t SLIDER_TRACK_FILL = 0x2104;
static constexpr uint16_t SLIDER_BRIGHTNESS_FG = 0x07E0;
static constexpr uint16_t SLIDER_VOLUME_FG = 0x001F;
static constexpr uint16_t SLIDER_HINT_TEXT = 0x8410;

// Settings screen with brightness and voice volume sliders.
void V1Display::showSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel) {
    tft_->fillScreen(PALETTE_BG);

    const int sliderMargin = 40;
    const int sliderHeight = 10;
    const int sliderWidth = SCREEN_WIDTH - (sliderMargin * 2);
    const int sliderX = sliderMargin;
    const int brightnessY = 45;
    const int volumeY = 115;

    tft_->setTextColor(SLIDER_WHITE);
    tft_->setTextSize(2);
    tft_->setCursor((SCREEN_WIDTH - 120) / 2, 5);
    tft_->print("SETTINGS");

    tft_->setTextSize(1);
    tft_->setTextColor(SLIDER_WHITE);
    tft_->setCursor(sliderMargin, brightnessY - 16);
    tft_->print("BRIGHTNESS");

    tft_->drawRect(sliderX - 2, brightnessY - 2, sliderWidth + 4, sliderHeight + 4, SLIDER_TRACK_BORDER);
    tft_->fillRect(sliderX, brightnessY, sliderWidth, sliderHeight, SLIDER_TRACK_FILL);

    int brightnessFill = computeBrightnessSliderFill(brightnessLevel, sliderWidth);
    tft_->fillRect(sliderX, brightnessY, brightnessFill, sliderHeight, SLIDER_BRIGHTNESS_FG);

    int brightThumbX = sliderX + brightnessFill - 4;
    if (brightThumbX < sliderX)
        brightThumbX = sliderX;
    if (brightThumbX > sliderX + sliderWidth - 8)
        brightThumbX = sliderX + sliderWidth - 8;
    tft_->fillRect(brightThumbX, brightnessY - 4, 8, sliderHeight + 8, SLIDER_WHITE);

    char brightStr[8];
    int brightPercent = computeBrightnessSliderPercent(brightnessLevel);
    snprintf(brightStr, sizeof(brightStr), "%d%%", brightPercent);
    tft_->setCursor(sliderX + sliderWidth + 8, brightnessY);
    tft_->print(brightStr);

    tft_->setTextColor(SLIDER_WHITE);
    tft_->setCursor(sliderMargin, volumeY - 16);
    tft_->print("VOICE VOLUME");

    tft_->drawRect(sliderX - 2, volumeY - 2, sliderWidth + 4, sliderHeight + 4, SLIDER_TRACK_BORDER);
    tft_->fillRect(sliderX, volumeY, sliderWidth, sliderHeight, SLIDER_TRACK_FILL);

    const int volumeFill = (static_cast<int>(volumeLevel) * sliderWidth) / 100;
    tft_->fillRect(sliderX, volumeY, volumeFill, sliderHeight, SLIDER_VOLUME_FG);

    int volumeThumbX = sliderX + volumeFill - 4;
    if (volumeThumbX < sliderX)
        volumeThumbX = sliderX;
    if (volumeThumbX > sliderX + sliderWidth - 8)
        volumeThumbX = sliderX + sliderWidth - 8;
    tft_->fillRect(volumeThumbX, volumeY - 4, 8, sliderHeight + 8, SLIDER_WHITE);

    char volumeStr[8];
    snprintf(volumeStr, sizeof(volumeStr), "%d%%", volumeLevel);
    tft_->setCursor(sliderX + sliderWidth + 8, volumeY);
    tft_->print(volumeStr);

    tft_->setTextSize(1);
    tft_->setTextColor(SLIDER_HINT_TEXT);
    tft_->setCursor((SCREEN_WIDTH - 220) / 2, 155);
    tft_->print("Touch sliders - BOOT to save");

    DISPLAY_FLUSH();
}

void V1Display::updateSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel, int activeSlider) {
    (void)activeSlider;
    setBrightness(brightnessLevel);
    showSettingsSliders(brightnessLevel, volumeLevel);
}

// Returns which slider was touched: 0=brightness, 1=volume, -1=none.
// Touch Y is inverted relative to display Y:
//   Low touch Y = bottom of display = volume slider
//   High touch Y = top of display = brightness slider
int V1Display::getActiveSliderFromTouch(int16_t touchY) {
    if (touchY <= 60)
        return 1;
    if (touchY >= 80)
        return 0;
    return -1;
}

void V1Display::hideBrightnessSlider() {
    // The caller restores the normal display after clearing the slider.
    clear();
}
