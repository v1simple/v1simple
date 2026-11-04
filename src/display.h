/**
 * Waveshare ESP32-S3-Touch-LCD-3.49 display driver.
 * Drawing is main-loop-only and limited to 20 Hz. Production owns one global
 * instance, including singleton-scoped hot-path render tracking.
 */

#pragma once
#ifndef DISPLAY_H
#define DISPLAY_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "display_driver.h"
#include "packet_parser.h"
#include "modules/alp/alp_laser_event.h"
#include "render_frame.h"
#include "color_themes.h"
#include "display_layout.h"
#include "display_presentation.h"
#include "display_vol_warn.h"
#include "display_dirty_flags.h"
#include "display_drawn_region.h"
#include "display_element_caches.h"
#include "display_frequency_digit_atlas.h"
#include "display_frequency_raster_cache.h"
#include "display_font_manager.h"

static_assert(SIGNAL_BAR_COLOR_COUNT == DisplayLayout::MAIN_SIGNAL_BAR_COUNT,
              "Main signal-bar colors must match the physical display segments");
static_assert(SIGNAL_BAR_COLOR_COUNT == DisplayLayout::CARD_METER_BAR_COUNT,
              "Card meter colors must match the physical display segments");


class V1Display {
  public:
    V1Display();
    ~V1Display();

    bool begin(const DisplayPaintContext& context);

    // Unified render entry fed by the display pipeline's composed frame.
    void renderFrame(const RenderFrame& frame, const DisplayPaintContext& context, int visibleCardCount = -1);

    bool isPersistedMode() const { return persistedMode_; }
    // Pending grey-card expiry; live cards never request a timed refresh.
    bool secondaryCardRefreshDue(uint32_t nowMs) const;

    void showDisconnected(const DisplayPaintContext& context);
    // Empty ipAddress means unavailable; stationMode selects STA/DHCP vs setup AP.
    void showMaintenanceMode(const DisplayPaintContext& context, const char* ipAddress = nullptr, bool stationMode = false);
    void showResting(const DisplayPaintContext& context, bool forceRedraw = false);
    void showScanning(const DisplayPaintContext& context, bool forceRedraw = true);
    void showStealth(const DisplayPaintContext& context, float speedMph, bool speedValid);

    // Force next frame to fully redraw (use after settings change)
    void forceNextRedraw();

    // Call on V1 disconnect so reconnect cannot reuse stale render state.
    void resetChangeTracking();

    void showBootSplash(const DisplayPaintContext& context);
    void showShutdown(const DisplayPaintContext& context);
    void showLowBattery(const DisplayPaintContext& context);

    // Set brightness (0-255)
    void setBrightness(uint8_t level);

    // Settings adjustment overlay (brightness + voice volume)
    void showSettingsSliders(const DisplayPaintContext& context, uint8_t brightnessLevel, uint8_t volumeLevel);
    void updateSettingsSliders(const DisplayPaintContext& context, uint8_t brightnessLevel, uint8_t volumeLevel, int activeSlider);
    void hideBrightnessSlider(const DisplayPaintContext& context); // Caller restores the normal display after clearing.
    int getActiveSliderFromTouch(int16_t touchY); // Returns 0=brightness, 1=volume, -1=none

    void clear(const DisplayPaintContext& context);

    const char* bandToString(Band band);
    uint16_t getBandColor(const ResolvedPaintStatus& paint, Band band);

    void updateColorTheme(const DisplayStyleView& style);
    const ColorPalette& getCurrentPalette() const { return currentPalette_; }

    void drawProfileIndicator(const DisplayPaintContext& context); // 0=Default, 1=Highway, 2=Comfort
    void setProfileIndicatorSlot(int slot, uint32_t selectedAtMs);

    // Battery indicator (only shows when on battery power)
    void drawBatteryIndicator(const DisplayPaintContext& context);

    void refreshBleIndicator(const DisplayPaintContext& context);
    void refreshObdIndicator(const DisplayPaintContext& context);
    void drawWiFiIndicator(const DisplayPaintContext& context);
    void drawConnectionIndicators(const DisplayPaintContext& context);

    void flush();
    void flushRegion(int16_t x, int16_t y, int16_t w, int16_t h); // Partial flush to reduce SPI traffic

    // Covers arrow drawing and clearing; raisedLayout must match dirty_.multiAlert.
    static DisplayLayout::DisplayRect arrowBoundingRect(bool raisedLayout);

  private:
    ResolvedPaintStatus resolvePaintStatus(const DisplayPaintContext& context, const RenderFrame* frame);
    void setAlpLaserEvent(const AlpLaserEvent& event);
    void update(const ResolvedPaintStatus& paint, const DisplayState& state);
    // Multi-alert display: shows the priority alert and eligible secondary-alert cards.
    void update(const ResolvedPaintStatus& paint, const AlertData& priority, const AlertData* allAlerts, int alertCount, const DisplayState& state);

    // Persisted alert display (shows last alert in dark grey after V1 clears it)
    void updatePersisted(const ResolvedPaintStatus& paint, const AlertData& alert, const DisplayState& state);

    void showDisconnected(const ResolvedPaintStatus& paint);
    void showMaintenanceMode(const ResolvedPaintStatus& paint, const char* ipAddress, bool stationMode);
    void showResting(const ResolvedPaintStatus& paint, bool forceRedraw);
    void showScanning(const ResolvedPaintStatus& paint, bool forceRedraw);
    void showStealth(const ResolvedPaintStatus& paint, float speedMph, bool speedValid);
    void showBootSplash(const ResolvedPaintStatus& paint);
    void showShutdown(const ResolvedPaintStatus& paint);
    void showLowBattery(const ResolvedPaintStatus& paint);
    void showSettingsSliders(const ResolvedPaintStatus& paint, uint8_t brightnessLevel, uint8_t volumeLevel);
    void updateSettingsSliders(const ResolvedPaintStatus& paint, uint8_t brightnessLevel, uint8_t volumeLevel, int activeSlider);
    void drawProfileIndicator(const ResolvedPaintStatus& paint);
    void drawBatteryIndicator(const ResolvedPaintStatus& paint);
    void drawWiFiIndicator(const ResolvedPaintStatus& paint);
    enum class ScreenMode { Unknown, Resting, Scanning, Disconnected, Maintenance, Live, Persisted, Stealth };

    std::unique_ptr<Arduino_ESP32QSPI> bus_;
    std::unique_ptr<Arduino_AXS15231B> gfxPanel_;
    std::unique_ptr<Arduino_Canvas> tft_; // Canvas for rotation/buffering

    DisplayState lastState_;

    ColorPalette currentPalette_;

    bool drawBandIndicators(const ResolvedPaintStatus& paint, uint8_t bandMask, bool muted, uint8_t bandFlashBits = 0);

    void drawFrequency(const ResolvedPaintStatus& paint, uint32_t freqMHz, Band band = BAND_NONE, bool muted = false, uint8_t photoType = 0);
    void drawFrequencySegment7(const ResolvedPaintStatus& paint, uint32_t freqMHz, Band band, bool muted, uint8_t photoType = 0);
    struct FrequencyPresentation {
        char text[16] = "";
        uint16_t color = 0;
        Band band = BAND_NONE;
        bool muted = false;
        bool photoRadar = false;
        bool hasFrequency = false;
        bool alpOverride = false;
        bool useOfr = false;
    };
    FrequencyPresentation resolveFrequencyPresentation(const ResolvedPaintStatus& paint, uint32_t freqMHz, Band band, bool muted,
                                                       uint8_t photoType);
    void renderFrequencyPresentation(const FrequencyPresentation& presentation);
    void renderFrequencyFallback(const FrequencyPresentation& presentation);
    void prewarmFrequencyDigitAtlas();
    void prewarmFrequencyRasterCache(const ResolvedPaintStatus& paint);
    void drawVolumeZeroWarning(const ResolvedPaintStatus& paint); // Flash "VOL 0" warning when volume=0 and no app connected
    void drawStatusText(const char* text, uint16_t color);
    void drawBLEProxyIndicator(const ResolvedPaintStatus& paint);
    void drawDirectionArrow(const ResolvedPaintStatus& paint, Direction dir, bool muted, uint8_t flashBits = 0, uint16_t frontColorOverride = 0);
    void drawVerticalSignalBars(const ResolvedPaintStatus& paint, uint8_t frontStrength, uint8_t rearStrength, Band band = BAND_KA, bool muted = false);
    void drawBaseFrame(const ResolvedPaintStatus& paint);
    void prepareFullRedrawNoClear(const ResolvedPaintStatus& paint);
    void drawTopCounter(const ResolvedPaintStatus& paint, char symbol, bool muted, bool showDot);
    void drawTopCounterSegment7(const ResolvedPaintStatus& paint, char symbol, bool muted, bool showDot);
    void drawTopCounterPair(const ResolvedPaintStatus& paint, char primary, bool muted, bool primaryDot, char secondary = '\0',
                            bool secondaryDot = false);
    void drawStatusStrip(const ResolvedPaintStatus& paint, const DisplayState& state, char topChar, bool topMuted, bool topDot);

    void drawVolumeIndicator(const ResolvedPaintStatus& paint, uint8_t mainVol, uint8_t muteVol); // "5V  0M" style
    void drawRssiIndicator(const ResolvedPaintStatus& paint, int rssi);                           // BLE RSSI in dBm
    void drawMuteIcon(bool muted);
    void drawObdIndicator(const ResolvedPaintStatus& paint);
    void drawGpsIndicator(const ResolvedPaintStatus& paint);
    void drawAlpIndicator(const ResolvedPaintStatus& paint);
    int measureSevenSegmentText(const char* text, float scale) const;
    int drawSevenSegmentText(const char* text, int x, int y, float scale, uint16_t onColor, uint16_t offColor);
    void drawSevenSegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor, uint16_t offColor);
    void draw14SegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor, uint16_t offColor);
    int draw14SegmentText(const char* text, int x, int y, float scale, uint16_t onColor, uint16_t offColor);

    void drawSecondaryAlertCards(const ResolvedPaintStatus& paint, const AlertData* alerts, int alertCount, const AlertData& priority,
                                 bool muted = false);
    static constexpr int SECONDARY_ROW_HEIGHT = DisplayLayout::SECONDARY_ROW_HEIGHT;

    int currentProfileSlot_ = 0;
    ScreenMode currentScreen_ = ScreenMode::Unknown; // Track current screen to avoid redundant full redraws
    uint32_t paletteRevision_ = 0;                   // Incremented on theme change to trigger redraws
    uint32_t lastRestingPaletteRevision_ = 0;
    uint32_t lastScanningPaletteRevision_ = 0;
    int lastRestingProfileSlot_ = -1;
    uint32_t lastStealthPaletteRevision_ = 0;
    int lastStealthRoundedMph_ = -1;                 // Last centered stealth speed text (-1 = "-- MPH")
    bool lastStealthSpeedValid_ = false;

    uint32_t profileChangedTime_ = 0;
    int lastProfileSlot_ = -1;
    bool bleProxyDrawn_ = false;           // Track if icon has been drawn at least once
    bool persistedMode_ = false;           // True when drawing persisted alerts (uses PALETTE_PERSISTED)
    // Leaf renderers record pixels actually painted after their cache checks.
    // Live, resting and persisted frames use this to skip unchanged canvases;
    // changed frames and pending external draws use a full-panel transfer.
    DrawnRegion drawnRegion_;

#if defined(DISPLAY_WAVESHARE_349)
    struct RestingNoOpKey {
        uint32_t paletteRevision = 0;
        uint32_t configurationRevision = 0;
        CardPersistencePolicy cardPersistence = CardPersistencePolicy::LiveGrace;
        uint32_t firmwareVersion = 0;
        uint32_t batteryMinuteBucket = 0;
        uint16_t colorBogey = 0;
        uint16_t colorVolumeMain = 0;
        uint16_t colorVolumeMute = 0;
        uint16_t colorRssiV1 = 0;
        uint16_t colorRssiProxy = 0;
        uint16_t colorFrequency = 0;
        uint16_t colorArrowFront = 0;
        uint16_t colorArrowSide = 0;
        uint16_t colorArrowRear = 0;
        uint16_t colorBandL = 0;
        uint16_t colorBandKa = 0;
        uint16_t colorBandK = 0;
        uint16_t colorBandX = 0;
        uint16_t colorBandPhoto = 0;
        uint16_t colorBars[SIGNAL_BAR_COLOR_COUNT] = {};
        uint16_t colorWifi = 0;
        uint16_t resolvedBleColor = 0;
        uint16_t colorBleConnected = 0;
        uint16_t colorBleDisconnected = 0;
        uint16_t colorObd = 0;
        uint16_t colorMuted = 0;
        uint16_t colorAlpConnected = 0;
        uint16_t colorAlpDli = 0;
        uint16_t colorAlpLidActive = 0;
        uint16_t colorAlpAlert = 0;
        int v1Rssi = 0;
        int proxyRssi = 0;
        int currentProfileSlot = -1;
        uint32_t currentProfileNameHash = 0;
        uint16_t currentProfileColor = 0;
        uint8_t activeBands = 0;
        uint8_t signalBars = 0;
        uint8_t flashBits = 0;
        uint8_t bandFlashBits = 0;
        uint8_t mainVolume = 0;
        uint8_t muteVolume = 0;
        uint8_t alpStateRaw = 0;
        uint8_t alpHbByte1 = 0;
        uint8_t batteryPct = 0;
        uint8_t batteryVoltageBand = 0;
        bool batteryShowOnUsb = true;
        char gunLabel[16] = {};
        bool gunLabelLidActive = false;
        uint8_t gpsSats = 0;
        Direction arrows = DIR_NONE;
        Direction priorityArrow = DIR_NONE;
        char bogeyCounterChar = '0';
        char bogeyCounterChar2 = ' ';
        bool bogeyCounterDot = false;
        bool bogeyCounterDot2 = false;
        bool hasVolumeData = false;
        bool hasV1Version = false;
        bool hasKuAlert = false;
        bool bleFresh = false;
        bool v1Connected = false;
        bool proxyConnected = false;
        bool bleProxyEnabled = false;
        bool bleProxyClientConnected = false;
        bool bleReceivingData = false;
        bool wifiServiceActive = false;
        bool wifiConnected = false;
        bool wifiGaveUp = false;
        bool hasBattery = false;
        bool showBatteryPercent = false;
        bool hideBatteryIcon = false;
        bool hideVolumeIndicator = false;
        bool hideRssiIndicator = false;
        bool hideWifiIcon = false;
        bool hideBleIcon = false;
        bool hideProfileIndicator = false;
        bool freqUseBandColor = false;
        bool profileFlashActive = false;
        bool obdEnabled = false;
        bool obdConnected = false;
        bool obdAttention = false;
        bool obdScanAttention = false;
        bool alpEnabled = false;
        bool alpHasLaserEvent = false;
        bool gpsShown = false;
    };

    RestingNoOpKey lastRestingNoOpKey_{};
    bool lastRestingNoOpKeyValid_ = false;
    RestingNoOpKey buildRestingNoOpKey(const ResolvedPaintStatus& paint, const DisplayState& state) const;
    bool canSkipRestingNoOp(const RestingNoOpKey& key) const;
    void rememberRestingNoOpKey(const RestingNoOpKey& key);
    void invalidateRestingNoOpKey();
#endif

    // Shared image1/image2 phase for arrows, bands and counter.
    // ESP Specification 3.015 p.26 recommends toggling every 96 ms (10.416 Hz).
    static constexpr unsigned long BLINK_INTERVAL_MS = 96;
    bool blinkPhase_ = true;
    unsigned long lastBlinkToggleMs_ = 0;

  public:
    // The orchestrator derives lightweight refresh timing from the renderer's
    // last phase transition to avoid competing cadence clocks.
    unsigned long getLastBlinkToggleMs() const { return lastBlinkToggleMs_; }
    static constexpr unsigned long getBlinkIntervalMs() { return BLINK_INTERVAL_MS; }
    bool isStealthScreen() const { return currentScreen_ == ScreenMode::Stealth; }

  private:
    // Inline so renderer tests can link without display.cpp.
    void updateBlinkPhase_(uint32_t now) {

        // Start at image1 even if boot took longer than one blink interval.
        if (lastBlinkToggleMs_ == 0) {
            lastBlinkToggleMs_ = now;
            return;
        }

        const unsigned long delta = now - lastBlinkToggleMs_;
        if (delta >= BLINK_INTERVAL_MS) {
            const unsigned long elapsedIntervals = delta / BLINK_INTERVAL_MS;
            if ((elapsedIntervals & 0x01UL) != 0) {
                blinkPhase_ = !blinkPhase_;
            }
            // Whole intervals prevent cadence drift after a late render.
            // Missing an even number of intervals leaves the phase unchanged.
            lastBlinkToggleMs_ += elapsedIntervals * BLINK_INTERVAL_MS;
        }
    }

    bool batteryShowOnUsb_ = true;
    bool lastProxyClientConnected_ = false;
    bool alpHasLaserEvent_ = false; // true when ALP has an active laser session (overrides byte1 → solid red)

    // ALP frequency-area override: when active, gun abbreviation replaces frequency text
    bool alpFreqOverride_ = false;
    char alpFreqText_[16] = "";
    bool alpLidActive_ = false;      // true = LID active (byte1=04, blue), false = DLI (orange)
    AlpLaserEvent alpLaserEvent_{};  // live ALP event-owned direction/gun/lid state for rendering

    static constexpr uint32_t HIDE_TIMEOUT_MS = 3000;

    DisplayFontManager fontMgr_;
    DisplayFrequencyDigitAtlas frequencyDigitAtlas_;
    DisplayFrequencyRasterCache frequencyRasterCache_;
    DisplayDirtyFlags dirty_;
    DisplayElementCaches elementCaches_;
    VolumeZeroWarning volZeroWarn_;

#ifdef UNIT_TEST
  public:
    // Takes ownership; caller must allocate with new.
    void setTestCanvas(Arduino_Canvas* canvas) { tft_.reset(canvas); }
    Arduino_Canvas* testCanvas() { return tft_.get(); }
    DisplayElementCaches& ut_elementCaches() { return elementCaches_; }
    // Simulate updateColorTheme() invalidation.
    void ut_bumpPaletteRevision() { ++paletteRevision_; }
    void ut_drawFrequency(const DisplayPaintContext& context, uint32_t frequency, Band band,
                          const char* alpText = nullptr, uint8_t photoType = 0) {
        ResolvedPaintStatus paint = resolvePaintStatus(context, nullptr);
        paint.hasGunLabel = alpText != nullptr;
        paint.gunLabel = alpText ? alpText : "";
        paint.gunLabelLidActive = false;
        drawFrequency(paint, frequency, band, false, photoType);
    }
    bool ut_drawBandIndicators(const DisplayPaintContext& context, uint8_t bandMask, bool muted,
                               uint8_t bandFlashBits = 0) {
        const ResolvedPaintStatus paint = resolvePaintStatus(context, nullptr);
        return drawBandIndicators(paint, bandMask, muted, bandFlashBits);
    }
    void ut_drawVerticalSignalBars(const DisplayPaintContext& context, uint8_t frontStrength,
                                   uint8_t rearStrength, Band band = BAND_KA, bool muted = false) {
        const ResolvedPaintStatus paint = resolvePaintStatus(context, nullptr);
        drawVerticalSignalBars(paint, frontStrength, rearStrength, band, muted);
    }
    void ut_drawTopCounterPair(const DisplayPaintContext& context, char primary, bool muted, bool primaryDot,
                              char secondary = '\0', bool secondaryDot = false) {
        const ResolvedPaintStatus paint = resolvePaintStatus(context, nullptr);
        drawTopCounterPair(paint, primary, muted, primaryDot, secondary, secondaryDot);
    }
    ResolvedPaintStatus ut_resolvePaintStatus(const DisplayPaintContext& context, const RenderFrame* frame = nullptr) {
        return resolvePaintStatus(context, frame);
    }
    DisplayFontManager& ut_fontMgr() { return fontMgr_; }
    void ut_setBlinkState(bool phase, unsigned long lastToggleMs) {
        blinkPhase_ = phase;
        lastBlinkToggleMs_ = lastToggleMs;
    }
    void ut_drawSecondaryAlertCards(const DisplayPaintContext& context, const AlertData* alerts,
                                    int alertCount, const AlertData& priority, bool muted = false) {
        const ResolvedPaintStatus paint = resolvePaintStatus(context, nullptr);
        drawSecondaryAlertCards(paint, alerts, alertCount, priority, muted);
    }
    bool ut_drawnRegionEmpty() const { return drawnRegion_.empty(); }
    int16_t ut_drawnRegionX() const { return drawnRegion_.x(); }
    int16_t ut_drawnRegionY() const { return drawnRegion_.y(); }
    int16_t ut_drawnRegionW() const { return drawnRegion_.w(); }
    int16_t ut_drawnRegionH() const { return drawnRegion_.h(); }
    void ut_resetDrawnRegion() { drawnRegion_.reset(); }
    bool ut_alpHasLaserEvent() const { return alpHasLaserEvent_; }
    bool ut_alpFreqOverride() const { return alpFreqOverride_; }
    const char* ut_alpFreqText() const { return alpFreqText_; }
    bool ut_alpLidActive() const { return alpLidActive_; }
    uint8_t ut_alpLaserDirectionRaw() const { return static_cast<uint8_t>(alpLaserEvent_.direction); }
#endif
};

#endif // DISPLAY_H
