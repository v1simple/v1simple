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
#include "display_ble_context.h"
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

class ObdRuntimeModule;
class AlpRuntimeModule;
class BatteryManager;
class GpsRuntimeModule;
class SettingsManager;
class WiFiManager;

class V1Display {
  public:
    explicit V1Display(SettingsManager& settings);
    ~V1Display();

    bool begin();

    void update(const DisplayState& state);
    // Multi-alert display: shows the priority alert and eligible secondary-alert cards.
    void update(const AlertData& priority, const AlertData* allAlerts, int alertCount, const DisplayState& state);

    // Persisted alert display (shows last alert in dark grey after V1 clears it)
    void updatePersisted(const AlertData& alert, const DisplayState& state);

    // Unified render entry fed by the display pipeline's composed frame.
    void renderFrame(const RenderFrame& frame);

    bool isPersistedMode() const { return persistedMode_; }
    // Pending grey-card expiry; live cards never request a timed refresh.
    bool secondaryCardRefreshDue(uint32_t nowMs) const;

    void showDisconnected();
    // Empty ipAddress means unavailable; stationMode selects STA/DHCP vs setup AP.
    void showMaintenanceMode(const char* ipAddress = nullptr, bool stationMode = false);
    void showResting(bool forceRedraw = false);
    void showScanning();
    void showStealth(float speedMph, bool speedValid);

    // Force next update() call to fully redraw (use after settings change)
    void forceNextRedraw();

    // Call on V1 disconnect so reconnect cannot reuse stale render state.
    void resetChangeTracking();

    void showBootSplash();
    void showShutdown();
    void showLowBattery();

    // Set brightness (0-255)
    void setBrightness(uint8_t level);

    // Settings adjustment overlay (brightness + voice volume)
    void showSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel);
    void updateSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel, int activeSlider);
    void hideBrightnessSlider(); // Caller restores the normal display after clearing.
    int getActiveSliderFromTouch(int16_t touchY); // Returns 0=brightness, 1=volume, -1=none

    void clear();

    const char* bandToString(Band band);
    uint16_t getBandColor(Band band);

    void updateColorTheme();
    const ColorPalette& getCurrentPalette() const { return currentPalette_; }

    void drawProfileIndicator(int slot); // 0=Default, 1=Highway, 2=Comfort
    void setProfileIndicatorSlot(int slot);

    // Battery indicator (only shows when on battery power)
    void drawBatteryIndicator();

    // Speed-vol zero active flag — suppresses VOL 0 warning when speed mute
    // intentionally set the V1 volume to 0.
    void setSpeedVolZeroActive(bool active);

    // Snapshot supplied by loop orchestration; freshness is tracked internally.
    void setBleContext(const DisplayBleContext& ctx);

    // Dim the connected icon when V1 packets stop arriving.
    void setBLEProxyStatus(bool proxyEnabled, bool clientConnected, bool receivingData = true);

    void drawWiFiIndicator();
    void refreshObdIndicator(uint32_t nowMs);
    void setObdRuntimeModule(ObdRuntimeModule* m);
    void setObdAttention(bool attention);

    void setAlpRuntimeModule(AlpRuntimeModule* m);
    void setSystemStatusSources(BatteryManager& battery, WiFiManager& wifi, GpsRuntimeModule& gps) {
        battery_ = &battery;
        wifi_ = &wifi;
        gpsRtMod_ = &gps;
    }

    // Preview-mode direct setters — bypass runtime modules for display test
    void setPreviewIndicatorOverridesActive(bool active);
    void setAlpPreviewState(bool enabled, uint8_t state, uint8_t hbByte1);
    void setObdPreviewState(bool enabled, bool connected, bool scanAttention);

    // ev.active gates live badge/arrow colors; inactive tails may retain gun text
    // and direction. Transient UNKNOWN gun updates retain identity within a live session.
    void setAlpLaserEvent(const AlpLaserEvent& ev);

    // Preview-only gun text; live rendering uses setAlpLaserEvent().
    // lidActive selects LID (byte1=04) vs DLI/detect-only color.
    void setAlpFrequencyOverride(const char* gunAbbrev, bool lidActive = true);
    void clearAlpFrequencyOverride();

    void flush();
    void flushRegion(int16_t x, int16_t y, int16_t w, int16_t h); // Partial flush to reduce SPI traffic

    // Covers arrow drawing and clearing; raisedLayout must match dirty_.multiAlert.
    static DisplayLayout::DisplayRect arrowBoundingRect(bool raisedLayout);

  private:
    SettingsManager& settings_;
    enum class ScreenMode { Unknown, Resting, Scanning, Disconnected, Maintenance, Live, Persisted, Stealth };

    std::unique_ptr<Arduino_ESP32QSPI> bus_;
    std::unique_ptr<Arduino_AXS15231B> gfxPanel_;
    std::unique_ptr<Arduino_Canvas> tft_; // Canvas for rotation/buffering

    DisplayState lastState_;

    ColorPalette currentPalette_;

    bool drawBandIndicators(uint8_t bandMask, bool muted, uint8_t bandFlashBits = 0);

    void drawFrequency(uint32_t freqMHz, Band band = BAND_NONE, bool muted = false, uint8_t photoType = 0);
    void drawFrequencySegment7(uint32_t freqMHz, Band band, bool muted, uint8_t photoType = 0);
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
    FrequencyPresentation resolveFrequencyPresentation(uint32_t freqMHz, Band band, bool muted,
                                                       uint8_t photoType);
    void renderFrequencyPresentation(const FrequencyPresentation& presentation);
    void renderFrequencyFallback(const FrequencyPresentation& presentation);
    void prewarmFrequencyDigitAtlas();
    void prewarmFrequencyRasterCache();
    void drawVolumeZeroWarning(); // Flash "VOL 0" warning when volume=0 and no app connected
    void drawStatusText(const char* text, uint16_t color);
    void drawBLEProxyIndicator();
    void drawDirectionArrow(Direction dir, bool muted, uint8_t flashBits = 0, uint16_t frontColorOverride = 0);
    void drawVerticalSignalBars(uint8_t frontStrength, uint8_t rearStrength, Band band = BAND_KA, bool muted = false);
    void drawBaseFrame();
    void prepareFullRedrawNoClear();
    void drawTopCounter(char symbol, bool muted, bool showDot);
    void drawTopCounterSegment7(char symbol, bool muted, bool showDot);
    void drawTopCounterPair(char primary, bool muted, bool primaryDot, char secondary = '\0',
                            bool secondaryDot = false);
    void drawStatusStrip(const DisplayState& state, char topChar, bool topMuted, bool topDot);

    void drawVolumeIndicator(uint8_t mainVol, uint8_t muteVol); // "5V  0M" style
    void drawRssiIndicator(int rssi);                           // BLE RSSI in dBm
    void drawMuteIcon(bool muted);
    void drawObdIndicator();
    void drawGpsIndicator();
    void drawAlpIndicator();
    void syncTopIndicators(uint32_t nowMs);
    void setObdStatus(bool enabled, bool connected, bool scanAttention = false);
    bool hasFreshBleContext(uint32_t nowMs) const;
    int measureSevenSegmentText(const char* text, float scale) const;
    int drawSevenSegmentText(const char* text, int x, int y, float scale, uint16_t onColor, uint16_t offColor);
    void drawSevenSegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor, uint16_t offColor);
    void draw14SegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor, uint16_t offColor);
    int draw14SegmentText(const char* text, int x, int y, float scale, uint16_t onColor, uint16_t offColor);

    void drawSecondaryAlertCards(const AlertData* alerts, int alertCount, const AlertData& priority,
                                 bool muted = false);
    static constexpr int SECONDARY_ROW_HEIGHT = DisplayLayout::SECONDARY_ROW_HEIGHT;

    int currentProfileSlot_ = 0;
    ScreenMode currentScreen_ = ScreenMode::Unknown; // Track current screen to avoid redundant full redraws
    uint32_t paletteRevision_ = 0;                   // Incremented on theme change to trigger redraws
    uint32_t lastRestingPaletteRevision_ = 0;
    int lastRestingProfileSlot_ = -1;
    uint32_t lastStealthPaletteRevision_ = 0;
    int lastStealthRoundedMph_ = -1;                 // Last centered stealth speed text (-1 = "-- MPH")
    bool lastStealthSpeedValid_ = false;

    uint32_t profileChangedTime_ = 0;
    int lastProfileSlot_ = -1;
    bool bleProxyEnabled_ = false;
    bool bleProxyClientConnected_ = false;
    bool bleReceivingData_ = true;         // True when V1 packets received recently (heartbeat)
    bool bleProxyDrawn_ = false;           // Track if icon has been drawn at least once
    bool multiAlertMode_ = false;          // True while rendering a live multi-alert frame
    bool persistedMode_ = false;           // True when drawing persisted alerts (uses PALETTE_PERSISTED)
    bool speedVolZeroActive_ = false;      // Suppress VOL 0 warning during speed-mute vol 0
    // Leaf renderers record pixels actually painted after their cache checks.
    // Live, resting and persisted frames use this to skip unchanged canvases;
    // changed frames and pending external draws use a full-panel transfer.
    DrawnRegion drawnRegion_;

#if defined(DISPLAY_WAVESHARE_349)
    struct RestingNoOpKey {
        uint32_t paletteRevision = 0;
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
    RestingNoOpKey buildRestingNoOpKey(const DisplayState& state, uint32_t nowMs, bool bleContextFresh) const;
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
    void updateBlinkPhase_() {
        const unsigned long now = millis();

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

    bool obdEnabled_ = false;
    bool obdConnected_ = false;
    bool obdScanAttention_ = false;                // Runtime manual scan / scan-pending state
    bool obdAttention_ = false;                    // Temporary UI hold-time attention
    bool previewIndicatorOverridesActive_ = false; // Display preview owns ALP/OBD badges
    ObdRuntimeModule* obdRtMod_ = nullptr;         // Injected in begin(); used by syncTopIndicators
    AlpRuntimeModule* alpRtMod_ = nullptr;         // Injected in begin(); used by syncTopIndicators
    BatteryManager* battery_ = nullptr;
    WiFiManager* wifi_ = nullptr;
    GpsRuntimeModule* gpsRtMod_ = nullptr;
    bool alpEnabled_ = false;
    uint8_t alpStateRaw_ = 0;                      // AlpState cast to uint8_t for badge color selection
    uint8_t alpHbByte1_ = 0;                       // Last B0 heartbeat byte1 (02=Warm-Up, 03=DLI, 04=LID)
    bool alpHasLaserEvent_ = false; // true when ALP has an active laser session (overrides byte1 → solid red)

    // ALP frequency-area override: when active, gun abbreviation replaces frequency text
    bool alpFreqOverride_ = false;
    char alpFreqText_[16] = "";
    bool alpLidActive_ = false;      // true = LID active (byte1=04, blue), false = DLI (orange)
    AlpLaserEvent alpLaserEvent_{};  // live ALP event-owned direction/gun/lid state for rendering
    DisplayBleContext bleCtx_;
    uint32_t bleCtxUpdatedAtMs_ = 0;

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
    void ut_drawFrequency(uint32_t frequency, Band band, const char* alpText = nullptr, uint8_t photoType = 0) {
        alpFreqOverride_ = alpText != nullptr;
        snprintf(alpFreqText_, sizeof(alpFreqText_), "%s", alpText ? alpText : "");
        drawFrequency(frequency, band, false, photoType);
    }
    bool ut_drawBandIndicators(uint8_t bandMask, bool muted, uint8_t bandFlashBits = 0) {
        return drawBandIndicators(bandMask, muted, bandFlashBits);
    }
    void ut_drawVerticalSignalBars(uint8_t frontStrength, uint8_t rearStrength, Band band = BAND_KA,
                                   bool muted = false) {
        drawVerticalSignalBars(frontStrength, rearStrength, band, muted);
    }
    void ut_drawTopCounterPair(char primary, bool muted, bool primaryDot, char secondary = '\0',
                               bool secondaryDot = false) {
        drawTopCounterPair(primary, muted, primaryDot, secondary, secondaryDot);
    }
    DisplayFontManager& ut_fontMgr() { return fontMgr_; }
    void ut_setBlinkState(bool phase, unsigned long lastToggleMs) {
        blinkPhase_ = phase;
        lastBlinkToggleMs_ = lastToggleMs;
    }
    void ut_syncTopIndicators(uint32_t nowMs) { syncTopIndicators(nowMs); }
    void ut_drawSecondaryAlertCards(const AlertData* alerts, int alertCount, const AlertData& priority,
                                    bool muted = false) {
        drawSecondaryAlertCards(alerts, alertCount, priority, muted);
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
