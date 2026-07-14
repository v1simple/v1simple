/**
 * Display Driver for V1 Gen2 Radar Detector Interface
 * Target: Waveshare ESP32-S3-Touch-LCD-3.49 (172x640 LCD, AXS15231B)
 *
 * Features:
 * - Multiple color themes (Standard/HighContrast/Stealth/Business)
 * - Custom 7-segment and 14-segment displays
 * - Live alert visualization with signal bars
 * - Status indicators (WiFi, BLE proxy, mute)
 *
 * Display Modes:
 * - Idle/Resting: Logo or blank screen
 * - Alert: Frequency, band, signal strength, direction
 * - Status: Connection info, bogey count
 *
 * Threading: All draw operations must be called from main thread
 * Display updates throttled to ~10 FPS max for performance
 *
 * Ownership: production firmware uses a single global V1Display instance.
 * Some hot-path render tracking is still singleton-scoped behind this class
 * so runtime behavior is singleton-oriented even though the API is object-shaped.
 */

#pragma once
#ifndef DISPLAY_H
#define DISPLAY_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

// Include display driver abstraction (Arduino_GFX only)
#include "display_driver.h"
#include "packet_parser.h"
#include "modules/alp/alp_laser_event.h"
#include "render_frame.h"
#include "color_themes.h"
#include "display_layout.h" // Centralized layout constants
#include "display_ble_context.h"
#include "display_vol_warn.h"
#include "display_dirty_flags.h"
#include "display_drawn_region.h"
#include "display_element_caches.h"
#include "display_frequency_digit_atlas.h"
#include "display_frequency_raster_cache.h"
#include "display_font_manager.h"

enum class PerfDisplayScreen : uint8_t;
class ObdRuntimeModule;
class AlpRuntimeModule;

class V1Display {
  public:
    V1Display();
    ~V1Display();

    // Initialize display
    bool begin();

    // Update display with current state
    void update(const DisplayState& state);
    // Multi-alert display: shows the priority alert; secondary alerts remain non-visual context
    void update(const AlertData& priority, const AlertData* allAlerts, int alertCount, const DisplayState& state);

    // Persisted alert display (shows last alert in dark grey after V1 clears it)
    void updatePersisted(const AlertData& alert, const DisplayState& state);

    // Unified render entry fed by the display pipeline's composed frame.
    void renderFrame(const RenderFrame& frame);

    // Check if currently in persisted mode (for color selection)
    bool isPersistedMode() const { return persistedMode_; }

    // Show connection status
    void showDisconnected();
    void showMaintenanceMode();
    void showResting(bool forceRedraw = false);        // idle/rest screen
    void showScanning();                               // scanning screen (like resting but with SCAN text)
    void showStealth(float speedMph, bool speedValid); // stealth: blank screen with OBD speed

    // Force next update() call to fully redraw (use after settings change)
    void forceNextRedraw();

    // Reset singleton-scoped render tracking (call on V1 disconnect to ensure
    // the single production display path reconnects with a clean redraw state).
    void resetChangeTracking();

    void showBootSplash();
    void showShutdown();   // Shutdown screen with goodbye message
    void showLowBattery(); // Critical low battery warning

    // Set brightness (0-255)
    void setBrightness(uint8_t level);

    // Settings adjustment overlay (brightness + voice volume)
    void showSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel);
    void updateSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel, int activeSlider);
    void hideBrightnessSlider();                  // Hide slider and restore display
    int getActiveSliderFromTouch(int16_t touchY); // Returns 0=brightness, 1=volume, -1=none

    // Clear screen
    void clear();

    // Utility
    const char* bandToString(Band band);
    uint16_t getBandColor(Band band);

    // Color theme helpers
    void updateColorTheme(); // Update colors from settings
    const ColorPalette& getCurrentPalette() const { return currentPalette_; }

    // Profile indicator
    void drawProfileIndicator(int slot); // 0=Default, 1=Highway, 2=Comfort
    void setProfileIndicatorSlot(int slot);

    // Battery indicator (only shows when on battery power)
    void drawBatteryIndicator();

    // Speed-vol zero active flag — suppresses VOL 0 warning when speed mute
    // intentionally set the V1 volume to 0.
    void setSpeedVolZeroActive(bool active);

    // BLE context snapshot — populated by the loop orchestration path so display
    // files never depend on extern V1BLEClient. Freshness is tracked internally.
    void setBleContext(const DisplayBleContext& ctx);

    // BLE proxy indicator (blue = advertising/no client, green = client connected)
    // receivingData dims the icon when connected but no V1 packets received recently
    void setBLEProxyStatus(bool proxyEnabled, bool clientConnected, bool receivingData = true);

    // WiFi indicator (shows when connected to STA network)
    void drawWiFiIndicator();
    void refreshObdIndicator(uint32_t nowMs);
    void setObdRuntimeModule(ObdRuntimeModule* m);
    void setObdAttention(bool attention);

    // ALP indicator (shows current ALP status left of MUTED badge)
    void refreshAlpIndicator(uint32_t nowMs);
    void setAlpRuntimeModule(AlpRuntimeModule* m);

    // Preview-mode direct setters — bypass runtime modules for display test
    void setPreviewIndicatorOverridesActive(bool active);
    void setAlpPreviewState(bool enabled, uint8_t state, uint8_t hbByte1);
    void setObdPreviewState(bool enabled, bool connected, bool scanAttention);

    // ALP laser event — atomic setter for all ALP-owned display fields.
    // ev.active controls only the live-alert visual state (badge red / arrow
    // color override). Gun/direction may still be provided with active=false
    // so a persisted ALP tail can retain gun text without masquerading as a
    // live alert. During a continuous live event, a transient gun=UNKNOWN
    // update keeps the last known gun text instead of clearing it mid-alert.
    void setAlpLaserEvent(const AlpLaserEvent& ev);

    // ALP frequency override — PREVIEW-ONLY.
    // Live rendering uses setAlpLaserEvent(); display_preview_module.cpp uses
    // this helper to show sample gun text without a real ALP runtime event.
    // lidActive=true when ALP is LID active (byte1=04, above LID speed limit) — color
    // reflects active-jam rendering; lidActive=false renders DLI/detect-only color.
    void setAlpFrequencyOverride(const char* gunAbbrev, bool lidActive = true);
    void clearAlpFrequencyOverride();

    // Flush canvas to physical display
    void flush();
    void flushRegion(int16_t x, int16_t y, int16_t w, int16_t h); // Partial flush to reduce SPI traffic
    uint32_t renderSequenceId() const { return renderSeq_; }
    void setVisualTestBlinkPhase(bool image1Phase, unsigned long epochMs);
    const uint8_t* rawFramebufferBytes() const;
    size_t rawFramebufferByteLength() const;
    bool rawFramebufferAvailable() const;
    static constexpr uint16_t rawFramebufferWidth() { return CANVAS_WIDTH; }
    static constexpr uint16_t rawFramebufferHeight() { return CANVAS_HEIGHT; }
    static constexpr uint16_t logicalFramebufferWidth() { return SCREEN_WIDTH; }
    static constexpr uint16_t logicalFramebufferHeight() { return SCREEN_HEIGHT; }
    static constexpr const char* rawFramebufferFormat() { return "RGB565LE"; }
    static constexpr const char* rawFramebufferTransform() { return "canvas-rotation-1"; }

    // Visual-test flush shadow — maintenance/bench-only mirror of every byte
    // pushed to the panel (full flushes and partial region flushes alike).
    // Enabled by DisplayPreviewModule::pinStep() while a visual pin is active;
    // freed by clearVisualPin(). The host verifier asserts shadow ==
    // framebuffer after each pin: a mismatch means pixels were painted but
    // never flushed (dirty-window under-coverage) or flushed into the wrong
    // region — the defect class a framebuffer capture alone cannot see.
    // See docs/DISPLAY_VISUAL_REGRESSION.md (flush-shadow amendment).
    bool enableVisualFlushShadow();
    void disableVisualFlushShadow();
    bool flushShadowAvailable() const { return flushShadow_ != nullptr; }
    const uint8_t* flushShadowBytes() const { return reinterpret_cast<const uint8_t*>(flushShadow_); }

    // Logical bounding rect of the direction-arrow cluster. Exposed so the
    // live-update path can flushRegion() over exactly what drawDirectionArrow's
    // clear/redraw covers. raisedLayout mirrors drawDirectionArrow's internal
    // gate (true when dirty_.multiAlert is set, i.e. in every Live-mode frame).
    //
    // Returns DisplayLayout::DisplayRect (generic rect type shared with the
    // partial-flush region-union work — see include/display_layout.h). The
    // former V1Display::ArrowClusterRect alias is retained so callers and
    // tests don't have to retype ``V1Display::`` scope changes in a single
    // cleanup pass; remove it once all consumers use DisplayLayout::DisplayRect.
    using ArrowClusterRect = DisplayLayout::DisplayRect;
    static DisplayLayout::DisplayRect arrowBoundingRect(bool raisedLayout);

  private:
    enum class ScreenMode { Unknown, Resting, Scanning, Disconnected, Maintenance, Live, Persisted, Stealth };
    static PerfDisplayScreen perfScreenForMode(ScreenMode mode);

    // Display driver (Arduino_GFX)
    std::unique_ptr<Arduino_ESP32QSPI> bus_;
    std::unique_ptr<Arduino_AXS15231B> gfxPanel_;
    std::unique_ptr<Arduino_Canvas> tft_; // Canvas for rotation/buffering

    // Visual-test flush shadow (raw framebuffer layout, PSRAM). Null unless a
    // visual pin is active. Sync helpers are called from DISPLAY_FLUSH() and
    // flushRegion(); both no-op when the shadow is disabled. Defined inline
    // here (not in display.cpp) because DISPLAY_FLUSH() is expanded in
    // display_*.cpp translation units that native suites compile without
    // linking display.cpp — an out-of-line definition would break their link.
    uint16_t* flushShadow_ = nullptr;
    void flushShadowSyncFull_() {
        if (!flushShadow_ || !tft_) {
            return;
        }
        uint16_t* fb = tft_->getFramebuffer();
        if (!fb) {
            return;
        }
        std::memcpy(flushShadow_, fb,
                    static_cast<size_t>(CANVAS_WIDTH) * static_cast<size_t>(CANVAS_HEIGHT) * sizeof(uint16_t));
    }
    void flushShadowSyncRegion_(int16_t physX0, int16_t physRow0, int16_t physW, int16_t physRows) {
        if (!flushShadow_ || !tft_) {
            return;
        }
        uint16_t* fb = tft_->getFramebuffer();
        if (!fb) {
            return;
        }
        // Mirrors flushRegion's row blit exactly — same offsets, same widths —
        // so the shadow holds precisely the bytes the panel was sent, no more.
        for (int16_t row = 0; row < physRows; ++row) {
            const uint32_t offset = static_cast<uint32_t>(physRow0 + row) * static_cast<uint32_t>(CANVAS_WIDTH) +
                                    static_cast<uint32_t>(physX0);
            std::memcpy(flushShadow_ + offset, fb + offset, static_cast<size_t>(physW) * sizeof(uint16_t));
        }
    }

    DisplayState lastState_;
    AlertData lastAlert_;

    // Color palette
    ColorPalette currentPalette_; // Store current theme palette

    // Drawing helpers
    bool drawBandIndicators(uint8_t bandMask, bool muted, uint8_t bandFlashBits = 0);

    void drawFrequency(uint32_t freqMHz, Band band = BAND_NONE, bool muted = false, bool isPhotoRadar = false);
    void drawFrequencySegment7(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar = false); // 7-segment style
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
    void drawTopCounterSegment7(char symbol, bool muted, bool showDot); // 7-segment style
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

    // Multi-alert card row
    void drawSecondaryAlertCards(const AlertData* alerts, int alertCount, const AlertData& priority,
                                 bool muted = false);
    // Use centralized constant from display_layout.h
    static constexpr int SECONDARY_ROW_HEIGHT = DisplayLayout::SECONDARY_ROW_HEIGHT;

    int currentProfileSlot_ = 0;                     // Track current profile for display
    ScreenMode currentScreen_ = ScreenMode::Unknown; // Track current screen to avoid redundant full redraws
    uint32_t paletteRevision_ = 0;                   // Incremented on theme change to trigger redraws
    uint32_t lastRestingPaletteRevision_ = 0;        // Palette revision last used for resting screen
    int lastRestingProfileSlot_ = -1;                // Last profile shown on resting screen
    uint32_t lastStealthPaletteRevision_ = 0;        // Palette revision last used for stealth screen
    int lastStealthRoundedMph_ = -1;                 // Last centered stealth speed text (-1 = "-- MPH")
    bool lastStealthSpeedValid_ = false;             // Last stealth speed text validity

    // Visibility timeout tracking
    uint32_t wifiConnectedTime_ = 0;       // When WiFi became connected
    uint32_t profileChangedTime_ = 0;      // When profile was last changed
    bool wifiWasConnected_ = false;        // Track WiFi connection state changes
    int lastProfileSlot_ = -1;             // Track profile changes
    bool bleProxyEnabled_ = false;         // BLE proxy enabled flag
    bool bleProxyClientConnected_ = false; // BLE proxy client connection flag
    bool bleReceivingData_ = true;         // True when V1 packets received recently (heartbeat)
    bool bleProxyDrawn_ = false;           // Track if icon has been drawn at least once
    bool multiAlertMode_ = false;          // True while rendering a live multi-alert frame
    bool persistedMode_ = false;           // True when drawing persisted alerts (uses PALETTE_PERSISTED)
    bool speedVolZeroActive_ = false;      // Suppress VOL 0 warning during speed-mute vol 0
    // Per-frame render instrumentation (not DisplayDirtyFlags-governed).
    // Each update path resets drawnRegion_ at frame entry and each leaf draw
    // function that actually draws (past its cache early-return) calls
    // drawnRegion_.add(x, y, w, h) with the rect it touched. Live alert frames
    // pick between DISPLAY_FLUSH, cache-hit-skip, or flushRegion(union).
    // Resting/persisted frames consume the same signal only to skip unchanged
    // frames; when any pixels changed they still use the safer full flush.
    // See docs/plans/PARTIAL_FLUSH_REGION_UNION_20260422.md for the
    // bounded-drift Valentine's Law argument.
    DrawnRegion drawnRegion_;
    // Set by drawDirectionArrow() when the live V1 arrow direction set changes
    // (active ↔ resting). Those frames clear and repaint the full arrow glyph
    // cluster; on AXS15231B, delivering that repaint via a small partial window
    // can leave a just-blinked-off arrow black instead of restoring its dim
    // resting glyph. Reset at the top of each live update().
    bool arrowVisibilityForceFullFlush_ = false;
    // Set by drawDirectionArrow() when the arrow cluster repaints this frame.
    // Multi-rect dispatch keeps arrow-bearing frames on the historical
    // union/full path so the known-good arrow behavior remains isolated from
    // item-rect experiments.
    bool arrowPaintedThisFrame_ = false;

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
        uint16_t colorBar1 = 0;
        uint16_t colorBar2 = 0;
        uint16_t colorBar3 = 0;
        uint16_t colorBar4 = 0;
        uint16_t colorBar5 = 0;
        uint16_t colorBar6 = 0;
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

    // Unified blink phase for arrow/band/bogey-counter flash animation.
    //
    // V1 protocol summary: docs/V1_PROTOCOL_REFERENCES.md#infdisplaydata.
    // The V1 blink cadence is a 96 ms image1/image2 toggle; a prior comment
    // here claimed the protocol was silent, which made the display drift from
    // the detector's own LCD cadence.
    // The earlier 125 ms / ~4 Hz constant in this file pre-dated reading the
    // spec PDF and was off by ~30%. Matching V1's own LCD behavior requires
    // 96 ms toggle. Keep one shared phase so arrows, bands, and the bogey
    // counter remain synchronized.
    //
    // Single source of truth, toggled at BLINK_INTERVAL_MS cadence
    // by updateBlinkPhase_() — drawDirectionArrow, drawBandIndicators, and
    // drawStatusStrip (for bogey-counter blink) call it at entry and then
    // read blinkPhase_.
    static constexpr unsigned long BLINK_INTERVAL_MS = 96; // ESP Spec 3.015: 10.416 Hz toggle
    bool blinkPhase_ = true;
    unsigned long lastBlinkToggleMs_ = 0;

  public:
    // D2 fix: orchestrator reads this to decide when to fire a lightweight
    // blink-refresh tick. Returning the renderer's own toggle timestamp
    // (rather than tracking refresh wall-clock) eliminates the beat between
    // refresh cadence and the 96 ms threshold. See
    // docs/ARROW_BEHAVIOR_ANALYSIS_20260429.md D2.
    unsigned long getLastBlinkToggleMs() const { return lastBlinkToggleMs_; }
    static constexpr unsigned long getBlinkIntervalMs() { return BLINK_INTERVAL_MS; }

  private:
    // Defined inline so rendering TUs (display_arrow.cpp, display_bands.cpp) that
    // are linked into native tests without display.cpp still resolve the symbol.
    // millis() is visible here via display_driver.h/Arduino.h (real build) or the
    // test mock Arduino.h (native tests include it before display.h).
    void updateBlinkPhase_() {
        const unsigned long now = millis();

        // First renderer entry establishes the phase epoch.  Starting from
        // image1 avoids an immediate boot-time inversion caused by
        // lastBlinkToggleMs_ defaulting to zero while millis() is already well
        // past 96 ms.
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
            // Advance by exact 96 ms quanta instead of snapping to now.  This
            // keeps the local image1/image2 phase locked to the V1-specified
            // cadence and prevents a late render from permanently drifting the
            // blink schedule.  If two intervals elapsed, the final phase is the
            // same as before and no repaint is forced solely due to missed work.
            lastBlinkToggleMs_ += elapsedIntervals * BLINK_INTERVAL_MS;
        }
    }

    bool obdEnabled_ = false;                      // OBD module enabled
    bool obdConnected_ = false;                    // OBD adapter connected
    bool obdScanAttention_ = false;                // Runtime manual scan / scan-pending state
    bool obdAttention_ = false;                    // Temporary UI hold-time attention
    bool previewIndicatorOverridesActive_ = false; // Display preview owns ALP/OBD badges
    ObdRuntimeModule* obdRtMod_ = nullptr;         // Injected in begin(); used by syncTopIndicators
    AlpRuntimeModule* alpRtMod_ = nullptr;         // Injected in begin(); used by syncTopIndicators
    bool alpEnabled_ = false;                      // ALP module enabled
    uint8_t alpStateRaw_ = 0;                      // AlpState cast to uint8_t for badge color selection
    uint8_t alpHbByte1_ = 0;                       // Last B0 heartbeat byte1 (02=Warm-Up, 03=DLI, 04=LID)
    bool alpHasLaserEvent_ = false; // true when ALP has an active laser session (overrides byte1 → solid red)

    // ALP frequency-area override: when active, gun abbreviation replaces frequency text
    bool alpFreqOverride_ = false;
    char alpFreqText_[16] = "";
    bool alpLidActive_ = false;      // true = LID active (byte1=04, blue), false = DLI (orange)
    AlpLaserEvent alpLaserEvent_{};  // live ALP event-owned direction/gun/lid state for rendering
    DisplayBleContext bleCtx_;       // BLE state snapshot for display DI
    uint32_t bleCtxUpdatedAtMs_ = 0; // When setBleContext() last refreshed bleCtx_
    uint32_t renderSeq_ = 0;         // Monotonic id bumped after every display flush.

    static constexpr uint32_t HIDE_TIMEOUT_MS = 3000; // 3 second display timeout

    // State previously at file scope in display.cpp
    DisplayFontManager fontMgr_;
    DisplayFrequencyDigitAtlas frequencyDigitAtlas_;
    DisplayFrequencyRasterCache frequencyRasterCache_;
    DisplayDirtyFlags dirty_;
    DisplayElementCaches elementCaches_;
    VolumeZeroWarning volZeroWarn_;

#ifdef UNIT_TEST
  public:
    // Test seam: inject a recording canvas and expose it for assertions.
    // The display takes ownership; caller must allocate with new.
    void setTestCanvas(Arduino_Canvas* canvas) { tft_.reset(canvas); }
    Arduino_Canvas* testCanvas() { return tft_.get(); }
    // Test seam for element caches
    DisplayElementCaches& ut_elementCaches() { return elementCaches_; }
    // Test seam: simulate a color-theme change. Production path is
    // updateColorTheme(), which increments the same counter.
    void ut_bumpPaletteRevision() { ++paletteRevision_; }
    DisplayFrequencyDigitAtlas& ut_frequencyDigitAtlas() { return frequencyDigitAtlas_; }
    // Public wrappers for private rendering methods (native integration tests only)
    void ut_drawFrequency(uint32_t freqMHz, Band band = BAND_NONE, bool muted = false, bool isPhotoRadar = false) {
        drawFrequency(freqMHz, band, muted, isPhotoRadar);
    }
    bool ut_drawBandIndicators(uint8_t bandMask, bool muted, uint8_t bandFlashBits = 0) {
        return drawBandIndicators(bandMask, muted, bandFlashBits);
    }
    void ut_drawVerticalSignalBars(uint8_t frontStrength, uint8_t rearStrength, Band band = BAND_KA,
                                   bool muted = false) {
        drawVerticalSignalBars(frontStrength, rearStrength, band, muted);
    }
    void ut_drawDirectionArrow(Direction dir, bool muted, uint8_t flashBits = 0, uint16_t frontColorOverride = 0) {
        drawDirectionArrow(dir, muted, flashBits, frontColorOverride);
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
    void ut_drawObdIndicator() { drawObdIndicator(); }
    void ut_drawBaseFrame() { drawBaseFrame(); }
    void ut_syncTopIndicators(uint32_t nowMs) { syncTopIndicators(nowMs); }
    void ut_setObdStatus(bool enabled, bool connected, bool scanAttention = false) {
        setObdStatus(enabled, connected, scanAttention);
    }
    void ut_drawVolumeIndicator(uint8_t mainVol, uint8_t muteVol) { drawVolumeIndicator(mainVol, muteVol); }
    void ut_drawRssiIndicator(int rssi) { drawRssiIndicator(rssi); }
    void ut_drawBatteryIndicator() { drawBatteryIndicator(); }
    void ut_drawBLEProxyIndicator() { drawBLEProxyIndicator(); }
    void ut_drawWiFiIndicator() { drawWiFiIndicator(); }
    void ut_drawSecondaryAlertCards(const AlertData* alerts, int alertCount, const AlertData& priority,
                                    bool muted = false) {
        drawSecondaryAlertCards(alerts, alertCount, priority, muted);
    }
    bool ut_drawnRegionEmpty() const { return drawnRegion_.empty(); }
    int16_t ut_drawnRegionX() const { return drawnRegion_.x(); }
    int16_t ut_drawnRegionY() const { return drawnRegion_.y(); }
    int16_t ut_drawnRegionW() const { return drawnRegion_.w(); }
    int16_t ut_drawnRegionH() const { return drawnRegion_.h(); }
    uint8_t ut_drawnRegionRectCount() const { return drawnRegion_.rectCount(); }
    void ut_resetDrawnRegion() { drawnRegion_.reset(); }
    bool ut_alpHasLaserEvent() const { return alpHasLaserEvent_; }
    bool ut_alpFreqOverride() const { return alpFreqOverride_; }
    const char* ut_alpFreqText() const { return alpFreqText_; }
    bool ut_alpLidActive() const { return alpLidActive_; }
    uint8_t ut_alpLaserDirectionRaw() const { return static_cast<uint8_t>(alpLaserEvent_.direction); }
#endif
};

// Global display instance (defined in main.cpp)
extern V1Display display;
#endif // DISPLAY_H
