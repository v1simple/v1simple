/**
 * Display Driver for V1 Gen2 Display
 * Supports multiple hardware platforms
 */

#include "display.h"
#include "config.h"
#include "display_layout.h" // Centralized layout constants
#include "color_themes.h"
#include "band_utils.h"
#include "display_segments.h" // 7/14-segment data tables
#include "display_ble_freshness.h"
#include "settings.h"
#include "battery_manager.h"
#include "wifi_manager.h"
#include "storage_manager.h"
#include "waveshare_349_hardware.h"
#include <esp_heap_caps.h>
#include <cstring>
#include <algorithm>

// Font rendering — all OpenFontRender instances and caches are owned by
// DisplayFontManager (see display_font_manager.h).
#include "display_font_manager.h"

// Convenience aliases so that drawing code updates stay concise.
using TextWidthCacheEntry = DisplayFontManager::WidthCacheEntry;

namespace {

class PatchedAxs15231B : public Arduino_AXS15231B {
  public:
    using Arduino_AXS15231B::Arduino_AXS15231B;

    void writeAddrWindow(int16_t x, int16_t y, uint16_t w, uint16_t h) override {
        // AXS15231B on the ESP32 QSPI path misprograms CASET/RASET when the
        // 4-byte payload is sent through writeC8D16D16(). Sending the same
        // bytes via writeC8Bytes() is stable for multi-row partial windows.
        if ((x != _currentX) || (w != _currentW)) {
            _currentX = x;
            _currentW = w;
            const uint16_t physX0 = static_cast<uint16_t>(x + _xStart);
            const uint16_t physX1 = static_cast<uint16_t>(physX0 + w - 1);
            uint8_t xBytes[4] = {
                static_cast<uint8_t>(physX0 >> 8),
                static_cast<uint8_t>(physX0),
                static_cast<uint8_t>(physX1 >> 8),
                static_cast<uint8_t>(physX1),
            };
            static_cast<Arduino_ESP32QSPI*>(_bus)->writeC8Bytes(AXS15231B_CASET, xBytes, sizeof(xBytes));
        }

        if ((y != _currentY) || (h != _currentH)) {
            _currentY = y;
            _currentH = h;
            const uint16_t physY0 = static_cast<uint16_t>(y + _yStart);
            const uint16_t physY1 = static_cast<uint16_t>(physY0 + h - 1);
            uint8_t yBytes[4] = {
                static_cast<uint8_t>(physY0 >> 8),
                static_cast<uint8_t>(physY0),
                static_cast<uint8_t>(physY1 >> 8),
                static_cast<uint8_t>(physY1),
            };
            static_cast<Arduino_ESP32QSPI*>(_bus)->writeC8Bytes(AXS15231B_RASET, yBytes, sizeof(yBytes));
        }

        _bus->writeCommand(AXS15231B_RAMWR);
    }
};

using ActiveAxs15231B = PatchedAxs15231B;

} // namespace

// ============================================================================
// Dirty-flag aggregate — see include/display_dirty_flags.h for struct definition
// ============================================================================
#include "display_dirty_flags.h"
#include "display_element_caches.h"

// Use centralized constant from display_layout.h
using DisplayLayout::PRIMARY_ZONE_HEIGHT;

// ============================================================================
// Volume-zero warning state machine
// Shows a flashing "VOL 0" warning when the V1 volume is 0, no app is
// connected, and speed-mute has not intentionally set volume to 0.
// ============================================================================
// VolumeZeroWarning struct — see include/display_vol_warn.h
#include "display_vol_warn.h"

// getEffectiveScreenHeight() now lives in display_layout.h

// DISPLAY_FLUSH() macro — see include/display_flush.h
#include "display_flush.h"

// Drawing primitives, coordinate transforms, dimColor — see include/display_draw.h
#include "display_draw.h"

// Global display instance reference — defined here, declared extern in display_palette.h
V1Display* g_displayInstance = nullptr;

// Palette helpers and colour macros — see include/display_palette.h
#include "display_palette.h"

// Cross-platform text drawing helpers — see include/display_text.h
#include "display_text.h"

using namespace DisplaySegments;

// TOP_COUNTER_* constants now live in display_layout.h
using DisplayLayout::TOP_COUNTER_FALLBACK_WIDTH;
using DisplayLayout::TOP_COUNTER_FIELD_H;
using DisplayLayout::TOP_COUNTER_FIELD_W;
using DisplayLayout::TOP_COUNTER_FIELD_X;
using DisplayLayout::TOP_COUNTER_FIELD_Y;
using DisplayLayout::TOP_COUNTER_FONT_SIZE;
using DisplayLayout::TOP_COUNTER_PAD_RIGHT;
using DisplayLayout::TOP_COUNTER_TEXT_Y;

V1Display::V1Display(SettingsManager& settings) : settings_(settings) {
    // Initialize with standard theme by default
    currentPalette_ = ColorThemes::STANDARD();
    // Set global instance for color palette access
    g_displayInstance = this;

    currentScreen_ = ScreenMode::Unknown;
    paletteRevision_ = 0;
    lastRestingPaletteRevision_ = 0;
    lastRestingProfileSlot_ = -1;
}

V1Display::~V1Display() = default;

bool V1Display::begin() {
    Serial.printf("[Display] Init %s...\n", DISPLAY_NAME);
    const unsigned long beginStartMs = millis();
    unsigned long stageStartMs = beginStartMs;
    auto logDisplayStage = [&](const char* stageName) {
        const unsigned long now = millis();
        Serial.printf("[Display] Stage %s: %lu ms (total=%lu)\n", stageName, now - stageStartMs, now - beginStartMs);
        stageStartMs = now;
    };

    // Ensure restart/re-init paths never leak partially constructed objects.
    tft_.reset();
    gfxPanel_.reset();
    bus_.reset();

    auto& boardHardware = waveshare_349::hardware();
    if (!boardHardware.ready()) {
        Serial.println("[Display] ERROR: Waveshare hardware revision is unknown; staying dark");
        boardHardware.forceBacklightOff();
        return false;
    }
    boardHardware.forceBacklightOff();

    // Preserve the qualified V1 timing while routing V2 reset through EXIO5.
    if (!boardHardware.resetPanel()) {
        Serial.println("[Display] ERROR: panel reset sequence failed");
        boardHardware.forceBacklightOff();
        return false;
    }

    // Create QSPI bus
    bus_.reset(new (std::nothrow) Arduino_ESP32QSPI(LCD_CS,    // CS
                                                    LCD_SCLK,  // SCK
                                                    LCD_DATA0, // D0
                                                    LCD_DATA1, // D1
                                                    LCD_DATA2, // D2
                                                    LCD_DATA3  // D3
                                                    ));
    if (!bus_) {
        Serial.println("[Display] ERROR: Failed to create bus!");
        return false;
    }

    // Create AXS15231B panel - native 172x640 portrait
    // Pass GFX_NOT_DEFINED for RST since we already did manual reset
    gfxPanel_.reset(new (std::nothrow)
                        ActiveAxs15231B(bus_.get(),                       // bus
                                        GFX_NOT_DEFINED,                  // RST - we already did manual reset
                                        0,                                // rotation (0 = no panel rotation)
                                        false,                            // IPS
                                        172,                              // width (Waveshare 3.49" is 172 wide)
                                        640,                              // height
                                        0,                                // col_offset1
                                        0,                                // row_offset1
                                        0,                                // col_offset2
                                        0,                                // row_offset2
                                        axs15231b_180640_init_operations, // init operations for this panel type
                                        sizeof(axs15231b_180640_init_operations)));
    if (!gfxPanel_) {
        Serial.println("[Display] ERROR: Failed to create panel!");
        bus_.reset();
        return false;
    }

    // Create canvas as 172x640 native with rotation=1 for landscape (90°)
    tft_.reset(new (std::nothrow) Arduino_Canvas(172, 640, gfxPanel_.get(), 0, 0, 1));

    if (!tft_) {
        Serial.println("[Display] ERROR: Failed to create canvas!");
        gfxPanel_.reset();
        bus_.reset();
        return false;
    }

    if (!tft_->begin()) {
        Serial.println("[Display] ERROR: tft->begin() failed!");
        tft_.reset();
        gfxPanel_.reset();
        bus_.reset();
        return false;
    }

    // PSRAM verification: Check where the 220 KiB framebuffer landed
    if (tft_) {
        void* canvasPtr = tft_->getFramebuffer();
        if (canvasPtr) {
            bool isPsram = esp_ptr_external_ram(canvasPtr);
            Serial.printf("[Display] Canvas framebuffer: %s (%p, ~220 KiB)\n", isPsram ? "PSRAM" : "INTERNAL SRAM",
                          canvasPtr);
            if (!isPsram) {
                Serial.println("[Display] WARNING: 220 KiB canvas in internal SRAM!");
            }
        }
    }

    tft_->fillScreen(COLOR_BLACK);
    DISPLAY_FLUSH();

    boardHardware.setBrightness(255);
    delay(30);

    delay(10); // Give hardware time to settle

    tft_->setTextColor(PALETTE_TEXT);
    tft_->setTextSize(2);

    logDisplayStage("hw_init");

    // Initialize OpenFontRender via the font manager.
    fontMgr_.init(tft_);
    logDisplayStage("font_init");

    frequencyRasterCache_.begin(static_cast<uint16_t>(DisplayLayout::kFrequencyZoneRect.w),
                                // The OFR clear strip intentionally includes vertical pad and is
                                // taller than the primary zone rect.
                                static_cast<uint16_t>(DisplayLayout::FREQUENCY_OFR_FONT_SIZE + 16));
    logDisplayStage("freq_cache_init");
    frequencyDigitAtlas_.begin(72,
                               // Match the numeric OFR clear strip height.
                               static_cast<uint16_t>(DisplayLayout::FREQUENCY_OFR_FONT_SIZE + 16));
    logDisplayStage("freq_digit_atlas_init");

    // Log representative glyph bounds for display diagnostics.
    if (fontMgr_.segment7Ready) {
        int oneMin = 0, oneMax = 0;
        int twoMin = 0, twoMax = 0;
        int eightMin = 0, eightMax = 0;
        if (fontMgr_.getTopCounterBounds('1', false, oneMin, oneMax) &&
            fontMgr_.getTopCounterBounds('2', false, twoMin, twoMax) &&
            fontMgr_.getTopCounterBounds('8', false, eightMin, eightMax)) {
            Serial.printf("[Display] TopCounter OFR bounds @%dpx: '1'=%d..%d '2'=%d..%d '8'=%d..%d\n",
                          TOP_COUNTER_FONT_SIZE, oneMin, oneMax, twoMin, twoMax, eightMin, eightMax);
        }
    }

    Serial.printf("[Display] OK %dx%d, font(seg7)=%d\n", SCREEN_WIDTH, SCREEN_HEIGHT, fontMgr_.segment7Ready);

    // Load color theme from settings
    updateColorTheme();
    prewarmFrequencyDigitAtlas();
    prewarmFrequencyRasterCache();
    logDisplayStage("freq_cache_prewarm");
    logDisplayStage("ready");

    return true;
}

void V1Display::setBrightness(uint8_t level) {
    waveshare_349::hardware().setBrightness(level);
}

void V1Display::clear() {
    tft_->fillScreen(PALETTE_BG);
    DISPLAY_FLUSH();
    bleProxyDrawn_ = false;
}

void V1Display::setBleContext(const DisplayBleContext& ctx) {
    bleCtx_ = ctx;
    bleCtxUpdatedAtMs_ = millis();
}

bool V1Display::hasFreshBleContext(uint32_t nowMs) const {
    return DisplayBleFreshness::isFresh(bleCtxUpdatedAtMs_, nowMs);
}

void V1Display::setBLEProxyStatus(bool proxyEnabled, bool clientConnected, bool receivingData) {
#if defined(DISPLAY_WAVESHARE_349)
    // Detect app disconnect - was connected, now isn't
    // Reset VOL 0 warning state immediately so it can trigger again
    if (bleProxyClientConnected_ && !clientConnected) {
        volZeroWarn_.reset();
    }

    // Check if proxy client connection changed - update RSSI display
    bool proxyChanged = (clientConnected != bleProxyClientConnected_);

    // Check if receiving state changed (for heartbeat visual)
    bool receivingChanged = (receivingData != bleReceivingData_);

    if (bleProxyDrawn_ && proxyEnabled == bleProxyEnabled_ && clientConnected == bleProxyClientConnected_ &&
        !receivingChanged) {
        return; // No visual change needed
    }

    bleProxyEnabled_ = proxyEnabled;
    bleProxyClientConnected_ = clientConnected;
    bleReceivingData_ = receivingData;
    drawBLEProxyIndicator();

    // Update RSSI display when proxy connection changes
    if (proxyChanged) {
        drawRssiIndicator(bleCtx_.v1Rssi);
    }

    // No flush here — the display pipeline owns strip flushing.
    // Drawing to the canvas is safe; the next pipeline iteration
    // will include the left strip in its flush.
#endif
}

void V1Display::flush() {
    DISPLAY_FLUSH();
}

void V1Display::flushRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
    // Constrain region to logical framebuffer bounds
    if (!tft_ || !gfxPanel_)
        return;
    int16_t maxW = tft_->width();  // 640 (logical landscape width)
    int16_t maxH = tft_->height(); // 172 (logical landscape height)
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (w <= 0 || h <= 0)
        return;
    if (x >= maxW || y >= maxH)
        return;
    if (x + w > maxW)
        w = maxW - x;
    if (y + h > maxH)
        h = maxH - y;
    const uint32_t areaPx = static_cast<uint32_t>(w) * static_cast<uint32_t>(h);

    uint16_t* fb = tft_->getFramebuffer();
    if (!fb) {
        DISPLAY_FLUSH();
        return;
    }

    // Canvas is Arduino_Canvas(172, 640, ..., rotation=1).
    // The raw (physical) framebuffer is 172 px wide × 640 px tall; stride = 172.
    // tft_->width() returns the LOGICAL width (640) after rotation — using it as
    // a stride produces wrong pointer arithmetic and sends garbage to the panel.
    //
    // Rotation-1 transform: logical(lx, ly) → physical(px = 171-ly, py = lx)
    //
    // Logical rect (x=lx0, y=ly0, w=lw, h=lh) maps to physical rect:
    //   py0 = lx0 = x           ph = lw = w
    //   px0 = (CANVAS_WIDTH - ly0 - lh) = (CANVAS_WIDTH - y - h)
    //   pw  = lh  = h
    //
    const int16_t kRawStride = CANVAS_WIDTH; // 172
    const int16_t phys_py0 = x;
    const int16_t phys_ph = w;
    const int16_t phys_px0 = kRawStride - y - h;
    const int16_t phys_pw = h;

    // Input clamping guarantees y+h ∈ [1, CANVAS_WIDTH], so phys_px0 remains
    // within [0, 171]. Retain a runtime guard against buffer overruns.
    if (phys_px0 < 0 || phys_px0 + phys_pw > kRawStride) {
        // Skip the flush if the clamping invariant is violated.
        Serial.printf("[Display] WARN flushRegion phys bounds: px0=%d pw=%d stride=%d (logical x=%d y=%d w=%d h=%d)\n",
                      phys_px0, phys_pw, kRawStride, x, y, w, h);
        return;
    }

    // Row-by-row blit remains the known-good path on hardware. One-call
    // multi-row partial batches were re-tested with contiguous/packed sources
    // and still produced wrong-region artifacts on this panel path.
    for (int16_t row = 0; row < phys_ph; ++row) {
        uint16_t* rowPtr = fb + static_cast<uint32_t>(phys_py0 + row) * static_cast<uint32_t>(kRawStride) +
                           static_cast<uint32_t>(phys_px0);
        gfxPanel_->draw16bitRGBBitmap(phys_px0, phys_py0 + row, rowPtr, phys_pw, 1);
    }
}

const char* V1Display::bandToString(Band band) {
    return bandName(band);
}

uint16_t V1Display::getBandColor(Band band) {
    const V1Settings& s = settings_.get();
    switch (band) {
    case BAND_LASER:
        return s.colorBandL;
    case BAND_KA:
        return s.colorBandKa;
    case BAND_K:
        return s.colorBandK;
    case BAND_KU:
        return s.colorBandK; // No dedicated Ku color; share K's swatch.
    case BAND_X:
        return s.colorBandX;
    default:
        return PALETTE_TEXT;
    }
}

void V1Display::updateColorTheme() {
    // The base palette is fixed; settings provide per-element overrides.
    currentPalette_ = ColorThemes::STANDARD();
    const V1Settings& s = settings_.get();
    currentPalette_.colorMuted = s.colorMuted;
    currentPalette_.colorPersisted = s.colorPersisted;
    ++paletteRevision_;
}

// The inline implementation in display.h remains available to native tests
// that include the leaf renderers directly. A shared clock keeps arrow and
// band flashes in lockstep for the same V1 signal.

// resetChangeTracking() defined in display_screens.cpp (deferred dirty-flag pattern)
