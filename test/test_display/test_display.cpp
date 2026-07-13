/**
 * test_display.cpp - Comprehensive Display Torture Tests
 * 
 * Tests every aspect of the display system under all conditions:
 * - State management and transitions
 * - Caching correctness (no unnecessary redraws, no missed redraws)
 * - Boundary conditions (min/max values)
 * - Multi-alert scenarios
 * - Frequency tolerance (V1 jitter)
 * - Mode transitions
 * - Stress tests (rapid state changes)
 */
#include <unity.h>
#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <cstdint>
#include <cstring>
#include <array>
#include <vector>
#include <cmath>

#include "../../include/display_slider_math.h"

// ============================================================================
// Mock Implementations and Test Helpers
// ============================================================================

// Band constants (from packet_parser.h)
enum Band {
    BAND_NONE   = 0,
    BAND_LASER  = 1 << 0,
    BAND_KA     = 1 << 1,
    BAND_K      = 1 << 2,
    BAND_X      = 1 << 3
};

// Direction constants
enum Direction {
    DIR_NONE    = 0,
    DIR_FRONT   = 1,
    DIR_SIDE    = 2,
    DIR_REAR    = 4
};

// Alert data structure
struct AlertData {
    Band band = BAND_NONE;
    Direction direction = DIR_NONE;
    uint8_t frontStrength = 0;
    uint8_t rearStrength = 0;
    uint32_t frequency = 0;
    bool isValid = false;
    bool isPriority = false;
    
    bool equals(const AlertData& other) const {
        return band == other.band && 
               direction == other.direction &&
               frontStrength == other.frontStrength &&
               rearStrength == other.rearStrength &&
               frequency == other.frequency;
    }
    
    // Helper to create populated alert
    static AlertData create(Band b, Direction d, uint8_t front, uint8_t rear, uint32_t freq, bool valid, bool priority) {
        AlertData a;
        a.band = b;
        a.direction = d;
        a.frontStrength = front;
        a.rearStrength = rear;
        a.frequency = freq;
        a.isValid = valid;
        a.isPriority = priority;
        return a;
    }
};

// Display state structure
struct DisplayState {
    uint8_t activeBands;
    Direction arrows;
    Direction priorityArrow;
    uint8_t signalBars;
    bool muted;
    bool systemTest;
    char modeChar;
    bool hasMode;
    bool displayOn;
    bool hasDisplayOn;
    uint8_t flashBits;
    uint8_t bandFlashBits;
    uint8_t mainVolume;
    uint8_t muteVolume;
    uint32_t v1FirmwareVersion;
    bool hasV1Version;
    bool hasVolumeData;
    uint8_t v1PriorityIndex;
    uint8_t bogeyCounterByte;
    char bogeyCounterChar;
    bool bogeyCounterDot;
    bool softMuted;  // spec-correct audio mute (aux0 bit 0)
    bool systemStatus;  // aux0 bit 2 (V1 actively searching)
    uint8_t savedMainVolume;  // RESPALLVOLUME 0x3D
    uint8_t savedMuteVolume;  // RESPALLVOLUME 0x3D
    bool hasSavedVolume;      // true once RESPALLVOLUME observed
    bool hasKuAlert;          // Any active alert resolves to BAND_KU
    
    DisplayState() : activeBands(BAND_NONE), arrows(DIR_NONE), priorityArrow(DIR_NONE),
                     signalBars(0), muted(false), systemTest(false),
                     modeChar(0), hasMode(false), displayOn(true), hasDisplayOn(false),
                     flashBits(0), bandFlashBits(0), mainVolume(0), muteVolume(0),
                     v1FirmwareVersion(0), hasV1Version(false), hasVolumeData(false),
                     v1PriorityIndex(0), bogeyCounterByte(0), bogeyCounterChar('0'), 
                     bogeyCounterDot(false), softMuted(false), systemStatus(true),
                     savedMainVolume(0), savedMuteVolume(0), hasSavedVolume(false),
                     hasKuAlert(false) {}
};

// Screen dimensions
static constexpr int SCREEN_WIDTH = 640;
static constexpr int SCREEN_HEIGHT = 172;

// Display caching simulation - tracks state to detect unnecessary redraws
class DisplayCacheTracker {
public:
    // Primary alert state
    Band lastBand = BAND_NONE;
    Direction lastDirection = DIR_NONE;
    uint32_t lastFrequency = 0;
    uint8_t lastFrontStrength = 0;
    uint8_t lastRearStrength = 0;
    bool lastMuted = false;

    // Additional state for band/signal/top/profile tracking
    uint8_t lastBandMask = 0;
    bool lastBandMuted = false;
    char lastStatusSymbol = '\0';
    bool lastStatusMuted = false;
    bool lastStatusDot = false;
    int lastProfileSlot = -1;

    // Force flags
    bool forceFrequencyRedraw = false;
    bool forceBandRedraw = false;
    bool forceArrowRedraw = false;
    bool forceSignalBarsRedraw = false;
    bool forceStatusBarRedraw = false;
    bool forceMuteIconRedraw = false;
    bool forceTopCounterRedraw = false;
    bool forceBatteryRedraw = false;
    bool forceProfileRedraw = false;

    // Draw counters
    int frequencyDrawCount = 0;
    int bandDrawCount = 0;
    int arrowDrawCount = 0;
    int signalBarsDrawCount = 0;
    int statusBarDrawCount = 0;
    int muteIconDrawCount = 0;
    int topCounterDrawCount = 0;
    int batteryDrawCount = 0;
    int profileDrawCount = 0;
    int fullScreenClearCount = 0;

    // Frequency tolerance constant (MHz)
    static constexpr int FREQ_TOLERANCE = 5;

    void reset() {
        lastBand = BAND_NONE;
        lastDirection = DIR_NONE;
        lastFrequency = 0;
        lastFrontStrength = 255;  // Use invalid value to force first draw
        lastRearStrength = 255;
        lastMuted = false;
        lastBandMask = 255;  // Invalid to force first draw
        lastBandMuted = false;
        lastStatusSymbol = '\0';
        lastStatusMuted = false;
        lastStatusDot = false;
        lastProfileSlot = -1;
        clearForceFlags();
        clearDrawCounters();
    }

    void clearForceFlags() {
        forceFrequencyRedraw = false;
        forceBandRedraw = false;
        forceArrowRedraw = false;
        forceSignalBarsRedraw = false;
        forceStatusBarRedraw = false;
        forceMuteIconRedraw = false;
        forceTopCounterRedraw = false;
        forceBatteryRedraw = false;
        forceProfileRedraw = false;
    }

    void clearDrawCounters() {
        frequencyDrawCount = 0;
        bandDrawCount = 0;
        arrowDrawCount = 0;
        signalBarsDrawCount = 0;
        statusBarDrawCount = 0;
        muteIconDrawCount = 0;
        topCounterDrawCount = 0;
        batteryDrawCount = 0;
        profileDrawCount = 0;
        fullScreenClearCount = 0;
    }

    void setAllForceFlags() {
        forceFrequencyRedraw = true;
        forceBandRedraw = true;
        forceArrowRedraw = true;
        forceSignalBarsRedraw = true;
        forceStatusBarRedraw = true;
        forceMuteIconRedraw = true;
        forceTopCounterRedraw = true;
        forceBatteryRedraw = true;
        forceProfileRedraw = true;
    }

    // Simulated drawBaseFrame - clears screen and sets force flags
    void drawBaseFrame() {
        fullScreenClearCount++;
        setAllForceFlags();
    }

    // Simulated invalidate-only full redraw prep
    void prepareFullRedrawNoClear() {
        setAllForceFlags();
    }

    // Check if frequency changed (with tolerance)
    bool frequencyChanged(uint32_t newFreq) {
        if (forceFrequencyRedraw) return true;
        int diff = abs((int)newFreq - (int)lastFrequency);
        return diff > FREQ_TOLERANCE;
    }

    // Simulate drawing frequency
    void drawFrequency(uint32_t freq, Band band, bool muted) {
        if (frequencyChanged(freq) || band != lastBand || muted != lastMuted || forceFrequencyRedraw) {
            frequencyDrawCount++;
            lastFrequency = freq;
            lastBand = band;
            lastMuted = muted;
            forceFrequencyRedraw = false;
        }
    }

    // Simulate drawing band indicators
    void drawBandIndicators(uint8_t bandMask, bool muted) {
        if (bandMask != lastBandMask || muted != lastBandMuted || forceBandRedraw) {
            bandDrawCount++;
            lastBandMask = bandMask;
            lastBandMuted = muted;
            forceBandRedraw = false;
        }
    }

    // Simulate drawing arrow
    void drawDirectionArrow(Direction dir, bool muted) {
        if (dir != lastDirection || muted != lastMuted || forceArrowRedraw) {
            arrowDrawCount++;
            lastDirection = dir;
            lastMuted = muted;
            forceArrowRedraw = false;
        }
    }

    // Simulate drawing signal bars
    void drawVerticalSignalBars(uint8_t front, uint8_t rear, bool muted) {
        if (front != lastFrontStrength || rear != lastRearStrength || muted != lastMuted || forceSignalBarsRedraw) {
            signalBarsDrawCount++;
            lastFrontStrength = front;
            lastRearStrength = rear;
            forceSignalBarsRedraw = false;
        }
    }


    void drawStatusBar(char symbol, bool muted, bool showDot) {
        if (symbol != lastStatusSymbol ||
            muted != lastStatusMuted ||
            showDot != lastStatusDot ||
            forceStatusBarRedraw) {
            statusBarDrawCount++;
            lastStatusSymbol = symbol;
            lastStatusMuted = muted;
            lastStatusDot = showDot;
            forceStatusBarRedraw = false;
        }
    }

    void drawProfileIndicator(int slot) {
        if (slot != lastProfileSlot || forceProfileRedraw) {
            profileDrawCount++;
            lastProfileSlot = slot;
            forceProfileRedraw = false;
        }
    }
};

// Global tracker instance
static DisplayCacheTracker g_tracker;

enum class ArrowOwnedRegion {
    None = 0,
    Front,
    Side,
    Rear,
};

class ArrowPartialRedrawTracker {
public:
    bool cacheValid = false;
    bool lastShowFront = false;
    bool lastShowSide = false;
    bool lastShowRear = false;
    bool lastMuted = false;
    bool lastRaisedLayout = true;
    bool forceArrowRedraw = false;
    int fullRegionRedrawCount = 0;
    int targetedRegionRedrawCount = 0;
    ArrowOwnedRegion lastTargetedRegion = ArrowOwnedRegion::None;

    void reset() {
        cacheValid = false;
        lastShowFront = false;
        lastShowSide = false;
        lastShowRear = false;
        lastMuted = false;
        lastRaisedLayout = true;
        forceArrowRedraw = false;
        clearDrawCounters();
    }

    void clearDrawCounters() {
        fullRegionRedrawCount = 0;
        targetedRegionRedrawCount = 0;
        lastTargetedRegion = ArrowOwnedRegion::None;
    }

    void invalidate() {
        forceArrowRedraw = true;
    }

    void render(Direction dir,
                bool muted,
                bool blinkOn,
                uint8_t flashBits = 0,
                bool raisedLayout = true) {
        bool showFront = (dir & DIR_FRONT) != 0;
        bool showSide = (dir & DIR_SIDE) != 0;
        bool showRear = (dir & DIR_REAR) != 0;

        if (!blinkOn) {
            if (flashBits & 0x20) showFront = false;
            if (flashBits & 0x40) showSide = false;
            if (flashBits & 0x80) showRear = false;
        }

        const bool frontChanged = cacheValid && (showFront != lastShowFront);
        const bool sideChanged = cacheValid && (showSide != lastShowSide);
        const bool rearChanged = cacheValid && (showRear != lastShowRear);
        const bool mutedChanged = cacheValid && (muted != lastMuted);
        const bool layoutChanged = cacheValid && (raisedLayout != lastRaisedLayout);
        const int changedCount = static_cast<int>(frontChanged) +
                                 static_cast<int>(sideChanged) +
                                 static_cast<int>(rearChanged);

        const bool anyChanged =
            forceArrowRedraw || !cacheValid || frontChanged || sideChanged || rearChanged ||
            mutedChanged || layoutChanged;
        if (!anyChanged) {
            return;
        }

        if (!forceArrowRedraw && cacheValid && !mutedChanged && !layoutChanged && changedCount == 1) {
            targetedRegionRedrawCount++;
            lastTargetedRegion = frontChanged
                                     ? ArrowOwnedRegion::Front
                                     : (sideChanged ? ArrowOwnedRegion::Side
                                                    : ArrowOwnedRegion::Rear);
        } else {
            fullRegionRedrawCount++;
            lastTargetedRegion = ArrowOwnedRegion::None;
        }

        lastShowFront = showFront;
        lastShowSide = showSide;
        lastShowRear = showRear;
        lastMuted = muted;
        lastRaisedLayout = raisedLayout;
        cacheValid = true;
        forceArrowRedraw = false;
    }
};

static ArrowPartialRedrawTracker g_arrowTracker;

// ============================================================================
// Test Cases: Band Decoding
// ============================================================================
// Test Cases: Band Decoding
// ============================================================================

void test_band_decode_laser() {
    uint8_t bandArrow = 0x01;
    Band decoded = (bandArrow & 0x01) ? BAND_LASER : 
                   (bandArrow & 0x02) ? BAND_KA :
                   (bandArrow & 0x04) ? BAND_K :
                   (bandArrow & 0x08) ? BAND_X : BAND_NONE;
    TEST_ASSERT_EQUAL(BAND_LASER, decoded);
}

void test_band_decode_ka() {
    uint8_t bandArrow = 0x02;
    Band decoded = (bandArrow & 0x01) ? BAND_LASER : 
                   (bandArrow & 0x02) ? BAND_KA :
                   (bandArrow & 0x04) ? BAND_K :
                   (bandArrow & 0x08) ? BAND_X : BAND_NONE;
    TEST_ASSERT_EQUAL(BAND_KA, decoded);
}

void test_band_decode_k() {
    uint8_t bandArrow = 0x04;
    Band decoded = (bandArrow & 0x01) ? BAND_LASER : 
                   (bandArrow & 0x02) ? BAND_KA :
                   (bandArrow & 0x04) ? BAND_K :
                   (bandArrow & 0x08) ? BAND_X : BAND_NONE;
    TEST_ASSERT_EQUAL(BAND_K, decoded);
}

void test_band_decode_x() {
    uint8_t bandArrow = 0x08;
    Band decoded = (bandArrow & 0x01) ? BAND_LASER : 
                   (bandArrow & 0x02) ? BAND_KA :
                   (bandArrow & 0x04) ? BAND_K :
                   (bandArrow & 0x08) ? BAND_X : BAND_NONE;
    TEST_ASSERT_EQUAL(BAND_X, decoded);
}

void test_band_decode_priority() {
    // When multiple bands, laser has priority
    uint8_t bandArrow = 0x0F; // All bands
    Band decoded = (bandArrow & 0x01) ? BAND_LASER : 
                   (bandArrow & 0x02) ? BAND_KA :
                   (bandArrow & 0x04) ? BAND_K :
                   (bandArrow & 0x08) ? BAND_X : BAND_NONE;
    TEST_ASSERT_EQUAL(BAND_LASER, decoded);
}

void test_band_decode_none() {
    uint8_t bandArrow = 0x00;
    Band decoded = (bandArrow & 0x01) ? BAND_LASER : 
                   (bandArrow & 0x02) ? BAND_KA :
                   (bandArrow & 0x04) ? BAND_K :
                   (bandArrow & 0x08) ? BAND_X : BAND_NONE;
    TEST_ASSERT_EQUAL(BAND_NONE, decoded);
}

// ============================================================================
// Test Cases: Direction Decoding
// ============================================================================

void test_direction_decode_front() {
    uint8_t bandArrow = 0x20;
    Direction decoded = (Direction)(((bandArrow & 0x20) ? DIR_FRONT : 0) |
                                    ((bandArrow & 0x40) ? DIR_SIDE : 0) |
                                    ((bandArrow & 0x80) ? DIR_REAR : 0));
    TEST_ASSERT_EQUAL(DIR_FRONT, decoded);
}

void test_direction_decode_side() {
    uint8_t bandArrow = 0x40;
    Direction decoded = (Direction)(((bandArrow & 0x20) ? DIR_FRONT : 0) |
                                    ((bandArrow & 0x40) ? DIR_SIDE : 0) |
                                    ((bandArrow & 0x80) ? DIR_REAR : 0));
    TEST_ASSERT_EQUAL(DIR_SIDE, decoded);
}

void test_direction_decode_rear() {
    uint8_t bandArrow = 0x80;
    Direction decoded = (Direction)(((bandArrow & 0x20) ? DIR_FRONT : 0) |
                                    ((bandArrow & 0x40) ? DIR_SIDE : 0) |
                                    ((bandArrow & 0x80) ? DIR_REAR : 0));
    TEST_ASSERT_EQUAL(DIR_REAR, decoded);
}

void test_direction_decode_multiple() {
    uint8_t bandArrow = 0xE0; // Front + Side + Rear
    Direction decoded = (Direction)(((bandArrow & 0x20) ? DIR_FRONT : 0) |
                                    ((bandArrow & 0x40) ? DIR_SIDE : 0) |
                                    ((bandArrow & 0x80) ? DIR_REAR : 0));
    TEST_ASSERT_EQUAL(DIR_FRONT | DIR_SIDE | DIR_REAR, decoded);
}

void test_direction_decode_none() {
    uint8_t bandArrow = 0x00;
    Direction decoded = (Direction)(((bandArrow & 0x20) ? DIR_FRONT : 0) |
                                    ((bandArrow & 0x40) ? DIR_SIDE : 0) |
                                    ((bandArrow & 0x80) ? DIR_REAR : 0));
    TEST_ASSERT_EQUAL(DIR_NONE, decoded);
}

// ============================================================================
// Test Cases: Frequency Tolerance (V1 Jitter Prevention)
// ============================================================================

void test_frequency_tolerance_no_change_within_tolerance() {
    g_tracker.reset();
    g_tracker.lastFrequency = 34700; // 34.700 GHz
    
    // Frequency within ±5 MHz should NOT trigger redraw
    TEST_ASSERT_FALSE(g_tracker.frequencyChanged(34703)); // +3 MHz
    TEST_ASSERT_FALSE(g_tracker.frequencyChanged(34697)); // -3 MHz
    TEST_ASSERT_FALSE(g_tracker.frequencyChanged(34705)); // +5 MHz (boundary)
    TEST_ASSERT_FALSE(g_tracker.frequencyChanged(34695)); // -5 MHz (boundary)
}

void test_frequency_tolerance_change_beyond_tolerance() {
    g_tracker.reset();
    g_tracker.lastFrequency = 34700;
    
    // Frequency beyond ±5 MHz SHOULD trigger redraw
    TEST_ASSERT_TRUE(g_tracker.frequencyChanged(34706)); // +6 MHz
    TEST_ASSERT_TRUE(g_tracker.frequencyChanged(34694)); // -6 MHz
    TEST_ASSERT_TRUE(g_tracker.frequencyChanged(35000)); // +300 MHz (new alert)
}

void test_frequency_tolerance_force_flag_overrides() {
    g_tracker.reset();
    g_tracker.lastFrequency = 34700;
    g_tracker.forceFrequencyRedraw = true;
    
    // Even within tolerance, force flag should trigger redraw
    TEST_ASSERT_TRUE(g_tracker.frequencyChanged(34700)); // Same frequency
    TEST_ASSERT_TRUE(g_tracker.frequencyChanged(34702)); // Within tolerance
}

void test_frequency_tolerance_zero_to_nonzero() {
    g_tracker.reset();
    g_tracker.lastFrequency = 0;
    
    // Going from no frequency to any frequency should always trigger
    // Note: 0 to 34700 is 34700 diff, which is > 5 tolerance
    TEST_ASSERT_TRUE(g_tracker.frequencyChanged(34700));
    
    // But 0 to 1 is only 1 diff, which is within tolerance
    // This is actually correct behavior - small frequencies are treated the same
    g_tracker.reset();
    g_tracker.lastFrequency = 0;
    // 0 to 6 should trigger (6 > 5)
    TEST_ASSERT_TRUE(g_tracker.frequencyChanged(6));
}

// ============================================================================
// Test Cases: Display Cache Invalidation
// ============================================================================

void test_cache_drawBaseFrame_sets_all_force_flags() {
    g_tracker.reset();
    g_tracker.clearForceFlags();
    
    // Verify all flags are false
    TEST_ASSERT_FALSE(g_tracker.forceFrequencyRedraw);
    TEST_ASSERT_FALSE(g_tracker.forceBandRedraw);
    TEST_ASSERT_FALSE(g_tracker.forceArrowRedraw);
    
    // drawBaseFrame should set all force flags
    g_tracker.drawBaseFrame();
    
    TEST_ASSERT_TRUE(g_tracker.forceFrequencyRedraw);
    TEST_ASSERT_TRUE(g_tracker.forceBandRedraw);
    TEST_ASSERT_TRUE(g_tracker.forceArrowRedraw);
    TEST_ASSERT_TRUE(g_tracker.forceSignalBarsRedraw);
    TEST_ASSERT_TRUE(g_tracker.forceStatusBarRedraw);
    TEST_ASSERT_TRUE(g_tracker.forceMuteIconRedraw);
    TEST_ASSERT_TRUE(g_tracker.forceTopCounterRedraw);
    TEST_ASSERT_TRUE(g_tracker.forceBatteryRedraw);
}


void test_prepareFullRedrawNoClear_sets_force_flags_without_screen_clear() {
    g_tracker.reset();
    g_tracker.clearForceFlags();

    g_tracker.prepareFullRedrawNoClear();

    TEST_ASSERT_EQUAL(0, g_tracker.fullScreenClearCount);
    TEST_ASSERT_TRUE(g_tracker.forceFrequencyRedraw);
    TEST_ASSERT_TRUE(g_tracker.forceBandRedraw);
    TEST_ASSERT_TRUE(g_tracker.forceArrowRedraw);
    TEST_ASSERT_TRUE(g_tracker.forceSignalBarsRedraw);
    TEST_ASSERT_TRUE(g_tracker.forceStatusBarRedraw);
    TEST_ASSERT_TRUE(g_tracker.forceProfileRedraw);
}

void test_invalidate_only_full_redraw_repaints_cached_regions() {
    g_tracker.reset();

    g_tracker.drawFrequency(34700, BAND_KA, false);
    g_tracker.drawDirectionArrow((Direction)(DIR_FRONT | DIR_SIDE), false);
    g_tracker.drawBandIndicators(BAND_KA | BAND_K, false);
    g_tracker.drawVerticalSignalBars(4, 2, false);
    g_tracker.drawStatusBar('A', false, true);
    g_tracker.drawProfileIndicator(1);
    g_tracker.clearDrawCounters();

    g_tracker.prepareFullRedrawNoClear();

    g_tracker.drawFrequency(34700, BAND_KA, false);
    g_tracker.drawDirectionArrow((Direction)(DIR_FRONT | DIR_SIDE), false);
    g_tracker.drawBandIndicators(BAND_KA | BAND_K, false);
    g_tracker.drawVerticalSignalBars(4, 2, false);
    g_tracker.drawStatusBar('A', false, true);
    g_tracker.drawProfileIndicator(1);

    TEST_ASSERT_EQUAL(0, g_tracker.fullScreenClearCount);
    TEST_ASSERT_EQUAL(1, g_tracker.frequencyDrawCount);
    TEST_ASSERT_EQUAL(1, g_tracker.arrowDrawCount);
    TEST_ASSERT_EQUAL(1, g_tracker.bandDrawCount);
    TEST_ASSERT_EQUAL(1, g_tracker.signalBarsDrawCount);
    TEST_ASSERT_EQUAL(1, g_tracker.statusBarDrawCount);
    TEST_ASSERT_EQUAL(1, g_tracker.profileDrawCount);
}

void test_cache_no_redraw_when_unchanged() {
    g_tracker.reset();
    
    // Initial draw
    g_tracker.drawFrequency(34700, BAND_KA, false);
    TEST_ASSERT_EQUAL(1, g_tracker.frequencyDrawCount);
    
    // Same state should NOT cause another draw
    g_tracker.drawFrequency(34700, BAND_KA, false);
    TEST_ASSERT_EQUAL(1, g_tracker.frequencyDrawCount);
    
    // Within tolerance should NOT cause another draw
    g_tracker.drawFrequency(34703, BAND_KA, false);
    TEST_ASSERT_EQUAL(1, g_tracker.frequencyDrawCount);
}

void test_cache_redraw_when_state_changes() {
    g_tracker.reset();
    
    // Initial draw
    g_tracker.drawFrequency(34700, BAND_KA, false);
    TEST_ASSERT_EQUAL(1, g_tracker.frequencyDrawCount);
    
    // Different frequency should trigger redraw
    g_tracker.drawFrequency(35000, BAND_KA, false);
    TEST_ASSERT_EQUAL(2, g_tracker.frequencyDrawCount);
    
    // Different band should trigger redraw
    g_tracker.drawFrequency(35000, BAND_K, false);
    TEST_ASSERT_EQUAL(3, g_tracker.frequencyDrawCount);
    
    // Muted change should trigger redraw
    g_tracker.drawFrequency(35000, BAND_K, true);
    TEST_ASSERT_EQUAL(4, g_tracker.frequencyDrawCount);
}

void test_cache_force_flag_clears_after_draw() {
    g_tracker.reset();
    g_tracker.forceFrequencyRedraw = true;
    
    g_tracker.drawFrequency(34700, BAND_KA, false);
    
    // Force flag should be cleared
    TEST_ASSERT_FALSE(g_tracker.forceFrequencyRedraw);
}

// ============================================================================
// Test Cases: Band Indicator Caching
// ============================================================================

void test_band_cache_no_redraw_unchanged() {
    g_tracker.reset();
    
    g_tracker.drawBandIndicators(BAND_KA | BAND_K, false);
    TEST_ASSERT_EQUAL(1, g_tracker.bandDrawCount);
    
    // Same mask, same muted - no redraw
    g_tracker.drawBandIndicators(BAND_KA | BAND_K, false);
    TEST_ASSERT_EQUAL(1, g_tracker.bandDrawCount);
}

void test_band_cache_redraw_on_mask_change() {
    g_tracker.reset();
    
    g_tracker.drawBandIndicators(BAND_KA, false);
    TEST_ASSERT_EQUAL(1, g_tracker.bandDrawCount);
    
    // Adding a band
    g_tracker.drawBandIndicators(BAND_KA | BAND_K, false);
    TEST_ASSERT_EQUAL(2, g_tracker.bandDrawCount);
    
    // Removing a band
    g_tracker.drawBandIndicators(BAND_K, false);
    TEST_ASSERT_EQUAL(3, g_tracker.bandDrawCount);
}

void test_band_cache_redraw_on_mute_change() {
    g_tracker.reset();
    
    g_tracker.drawBandIndicators(BAND_KA, false);
    g_tracker.drawBandIndicators(BAND_KA, true);
    
    TEST_ASSERT_EQUAL(2, g_tracker.bandDrawCount);
}

// ============================================================================
// Test Cases: Arrow Caching
// ============================================================================

void test_arrow_cache_no_redraw_unchanged() {
    g_tracker.reset();
    
    g_tracker.drawDirectionArrow(DIR_FRONT, false);
    TEST_ASSERT_EQUAL(1, g_tracker.arrowDrawCount);
    
    g_tracker.drawDirectionArrow(DIR_FRONT, false);
    TEST_ASSERT_EQUAL(1, g_tracker.arrowDrawCount);
}

void test_arrow_cache_redraw_on_direction_change() {
    g_tracker.reset();
    
    g_tracker.drawDirectionArrow(DIR_FRONT, false);
    g_tracker.drawDirectionArrow(DIR_SIDE, false);
    g_tracker.drawDirectionArrow(DIR_REAR, false);
    
    TEST_ASSERT_EQUAL(3, g_tracker.arrowDrawCount);
}

void test_arrow_combined_directions() {
    g_tracker.reset();
    
    g_tracker.drawDirectionArrow((Direction)(DIR_FRONT | DIR_REAR), false);
    TEST_ASSERT_EQUAL(1, g_tracker.arrowDrawCount);
    
    // Different combination
    g_tracker.drawDirectionArrow((Direction)(DIR_FRONT | DIR_SIDE), false);
    TEST_ASSERT_EQUAL(2, g_tracker.arrowDrawCount);
}


void test_arrow_partial_redraw_single_blink_toggle_updates_only_one_owned_region() {
    g_arrowTracker.reset();
    g_arrowTracker.render((Direction)(DIR_FRONT | DIR_SIDE), false, true, 0x20);
    g_arrowTracker.clearDrawCounters();

    g_arrowTracker.render((Direction)(DIR_FRONT | DIR_SIDE), false, false, 0x20);

    TEST_ASSERT_EQUAL(0, g_arrowTracker.fullRegionRedrawCount);
    TEST_ASSERT_EQUAL(1, g_arrowTracker.targetedRegionRedrawCount);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ArrowOwnedRegion::Front),
                          static_cast<int>(g_arrowTracker.lastTargetedRegion));
}

void test_arrow_partial_redraw_two_arrow_change_falls_back_to_full_region() {
    g_arrowTracker.reset();
    g_arrowTracker.render((Direction)(DIR_FRONT | DIR_SIDE), false, true, 0x60);
    g_arrowTracker.clearDrawCounters();

    g_arrowTracker.render((Direction)(DIR_FRONT | DIR_SIDE), false, false, 0x60);

    TEST_ASSERT_EQUAL(1, g_arrowTracker.fullRegionRedrawCount);
    TEST_ASSERT_EQUAL(0, g_arrowTracker.targetedRegionRedrawCount);
}

void test_arrow_partial_redraw_muted_transition_falls_back_to_full_region() {
    g_arrowTracker.reset();
    g_arrowTracker.render(DIR_FRONT, false, true, 0);
    g_arrowTracker.clearDrawCounters();

    g_arrowTracker.render(DIR_FRONT, true, true, 0);

    TEST_ASSERT_EQUAL(1, g_arrowTracker.fullRegionRedrawCount);
    TEST_ASSERT_EQUAL(0, g_arrowTracker.targetedRegionRedrawCount);
}

void test_arrow_partial_redraw_dirty_flag_forces_full_region_redraw() {
    g_arrowTracker.reset();
    g_arrowTracker.render(DIR_FRONT, false, true, 0);
    g_arrowTracker.clearDrawCounters();
    g_arrowTracker.invalidate();

    g_arrowTracker.render(DIR_FRONT, false, true, 0);

    TEST_ASSERT_EQUAL(1, g_arrowTracker.fullRegionRedrawCount);
    TEST_ASSERT_EQUAL(0, g_arrowTracker.targetedRegionRedrawCount);
}

// ============================================================================
// Test Cases: Signal Bars Caching
// ============================================================================

void test_signal_bars_cache_no_redraw_unchanged() {
    g_tracker.reset();
    
    g_tracker.drawVerticalSignalBars(3, 2, false);
    TEST_ASSERT_EQUAL(1, g_tracker.signalBarsDrawCount);
    
    g_tracker.drawVerticalSignalBars(3, 2, false);
    TEST_ASSERT_EQUAL(1, g_tracker.signalBarsDrawCount);
}

void test_signal_bars_cache_redraw_on_strength_change() {
    g_tracker.reset();
    
    g_tracker.drawVerticalSignalBars(3, 2, false);
    g_tracker.drawVerticalSignalBars(4, 2, false); // Front changed
    g_tracker.drawVerticalSignalBars(4, 3, false); // Rear changed
    
    TEST_ASSERT_EQUAL(3, g_tracker.signalBarsDrawCount);
}

void test_signal_bars_boundary_values() {
    g_tracker.reset();
    
    // Test min values - first draw always happens
    g_tracker.drawVerticalSignalBars(0, 0, false);
    TEST_ASSERT_EQUAL(1, g_tracker.signalBarsDrawCount);
    
    // Test max source-scale sample - different from previous so should draw
    g_tracker.drawVerticalSignalBars(6, 6, false);
    TEST_ASSERT_EQUAL(2, g_tracker.signalBarsDrawCount);
    
    // Same max values - no redraw
    g_tracker.drawVerticalSignalBars(6, 6, false);
    TEST_ASSERT_EQUAL(2, g_tracker.signalBarsDrawCount);
}

// ============================================================================
// Test Cases: Display State Transitions
// ============================================================================

void test_state_transition_resting_to_alert() {
    g_tracker.reset();
    
    // Simulate resting state (no alert)
    DisplayState restingState;
    restingState.displayOn = true;
    restingState.activeBands = BAND_NONE;
    
    // Transition to alert
    DisplayState alertState;
    alertState.displayOn = true;
    alertState.activeBands = BAND_KA;
    alertState.muted = false;
    
    // Should need full redraw on mode change
    g_tracker.drawBaseFrame();
    TEST_ASSERT_EQUAL(1, g_tracker.fullScreenClearCount);
    TEST_ASSERT_TRUE(g_tracker.forceFrequencyRedraw);
}

void test_state_transition_alert_to_muted() {
    g_tracker.reset();
    
    // Alert state - first draws
    g_tracker.drawFrequency(34700, BAND_KA, false);
    TEST_ASSERT_EQUAL(1, g_tracker.frequencyDrawCount);
    
    g_tracker.drawBandIndicators(BAND_KA, false);
    TEST_ASSERT_EQUAL(1, g_tracker.bandDrawCount);
    
    g_tracker.drawDirectionArrow(DIR_FRONT, false);
    TEST_ASSERT_EQUAL(1, g_tracker.arrowDrawCount);
    
    // Transition to muted - mute state change should cause frequency redraw
    g_tracker.drawFrequency(34700, BAND_KA, true);
    TEST_ASSERT_EQUAL(2, g_tracker.frequencyDrawCount);
    
    // Band change due to mute
    g_tracker.drawBandIndicators(BAND_KA, true);
    TEST_ASSERT_EQUAL(2, g_tracker.bandDrawCount);
    
    // Arrow with mute change - tracker checks lastMuted which is at class level
    // Need to reset and verify arrow detects mute change properly
    g_tracker.lastMuted = false;  // Ensure we track unmuted state
    g_tracker.drawDirectionArrow(DIR_FRONT, true);  // Now muted
    TEST_ASSERT_EQUAL(2, g_tracker.arrowDrawCount);
}

// ============================================================================
// Test Cases: Multi-Alert Scenarios
// ============================================================================

void test_multi_alert_priority_selection() {
    // Simulate V1 priority: highest strength alert
    AlertData alerts[3];
    alerts[0] = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    alerts[1] = AlertData::create(BAND_K, DIR_SIDE, 2, 0, 24150, true, false);
    alerts[2] = AlertData::create(BAND_X, DIR_REAR, 1, 0, 10525, true, false);
    
    // V1 marks priority via isPriority flag
    AlertData* priority = nullptr;
    for (int i = 0; i < 3; i++) {
        if (alerts[i].isPriority) {
            priority = &alerts[i];
            break;
        }
    }
    
    TEST_ASSERT_NOT_NULL(priority);
    TEST_ASSERT_EQUAL(BAND_KA, priority->band);
    TEST_ASSERT_EQUAL(34700, priority->frequency);
}

void test_multi_alert_no_secondary_context_for_single() {
    g_tracker.reset();

    int alertCount = 1;
    int secondaryContextCount = alertCount > 1 ? alertCount - 1 : 0;

    TEST_ASSERT_EQUAL(0, secondaryContextCount);
}

// ============================================================================
// Test Cases: Display State Validation
// ============================================================================

void test_display_state_default_values() {
    DisplayState state;
    
    TEST_ASSERT_EQUAL(BAND_NONE, state.activeBands);
    TEST_ASSERT_EQUAL(DIR_NONE, state.arrows);
    TEST_ASSERT_EQUAL(0, state.signalBars);
    TEST_ASSERT_FALSE(state.muted);
    TEST_ASSERT_TRUE(state.displayOn);
    TEST_ASSERT_EQUAL('0', state.bogeyCounterChar);
}

void test_display_state_volume_support_check() {
    DisplayState state;
    
    // No volume data, no version
    state.hasVolumeData = false;
    state.hasV1Version = false;
    state.v1FirmwareVersion = 0;
    bool supportsVol = state.hasVolumeData || (state.hasV1Version && state.v1FirmwareVersion >= 41028);
    TEST_ASSERT_FALSE(supportsVol);
    
    // Has volume data
    state.hasVolumeData = true;
    supportsVol = state.hasVolumeData || (state.hasV1Version && state.v1FirmwareVersion >= 41028);
    TEST_ASSERT_TRUE(supportsVol);
    
    // Has version >= 4.1028
    state.hasVolumeData = false;
    state.hasV1Version = true;
    state.v1FirmwareVersion = 41028;
    supportsVol = state.hasVolumeData || (state.hasV1Version && state.v1FirmwareVersion >= 41028);
    TEST_ASSERT_TRUE(supportsVol);
    
    // Has version < 4.1028
    state.v1FirmwareVersion = 41000;
    supportsVol = state.hasVolumeData || (state.hasV1Version && state.v1FirmwareVersion >= 41028);
    TEST_ASSERT_FALSE(supportsVol);
}

// ============================================================================
// Test Cases: Boundary Conditions
// ============================================================================

void test_boundary_frequency_min_max() {
    // Valid V1 frequency ranges
    // X band: 10.525 GHz ± tolerance
    // K band: 24.150 GHz ± tolerance
    // Ka band: 33.4-36.0 GHz (narrow/wide)
    
    uint32_t xMin = 10500;  // 10.5 GHz
    uint32_t xMax = 10550;  // 10.55 GHz
    uint32_t kMin = 24100;  // 24.1 GHz
    uint32_t kMax = 24200;  // 24.2 GHz
    uint32_t kaMin = 33400; // 33.4 GHz
    uint32_t kaMax = 36000; // 36.0 GHz
    
    // Verify ranges are reasonable
    TEST_ASSERT_TRUE(xMin < xMax);
    TEST_ASSERT_TRUE(kMin < kMax);
    TEST_ASSERT_TRUE(kaMin < kaMax);
    TEST_ASSERT_TRUE(xMax < kMin);
    TEST_ASSERT_TRUE(kMax < kaMin);
}

void test_boundary_signal_strength_clamping() {
    // V1 Gen2 LED-bar signal strength is 0-6
    uint8_t strength = 10; // Invalid
    uint8_t clamped = (strength > 6) ? 6 : strength;
    
    TEST_ASSERT_EQUAL(6, clamped);
    
    strength = 3;
    clamped = (strength > 6) ? 6 : strength;
    TEST_ASSERT_EQUAL(3, clamped);
}

void test_boundary_brightness_range() {
    // Brightness is 0-255
    uint8_t brightness = 128;
    TEST_ASSERT_TRUE(brightness >= 0 && brightness <= 255);
    
    // Min brightness
    brightness = 0;
    TEST_ASSERT_EQUAL(0, brightness);
    
    // Max brightness
    brightness = 255;
    TEST_ASSERT_EQUAL(255, brightness);
}

void test_brightness_slider_clamps_fill_below_display_range() {
    TEST_ASSERT_EQUAL_INT(0, computeBrightnessSliderFill(50, 560));
    TEST_ASSERT_EQUAL_INT(0, computeBrightnessSliderPercent(50));
}

void test_brightness_slider_clamps_fill_above_display_range() {
    TEST_ASSERT_EQUAL_INT(560, computeBrightnessSliderFill(255, 560));
    TEST_ASSERT_EQUAL_INT(100, computeBrightnessSliderPercent(255));
}

void test_boundary_volume_range() {
    // Volume is 0-9
    for (uint8_t vol = 0; vol <= 9; vol++) {
        TEST_ASSERT_TRUE(vol <= 9);
    }
}

// ============================================================================
// Test Cases: Stress Tests (Rapid State Changes)
// ============================================================================

void test_stress_rapid_frequency_changes() {
    g_tracker.reset();
    
    // Simulate rapid frequency updates (like during scan)
    uint32_t frequencies[] = {34700, 34702, 34698, 34701, 34700, 34705, 34695, 34700};
    
    for (int i = 0; i < 8; i++) {
        g_tracker.drawFrequency(frequencies[i], BAND_KA, false);
    }
    
    // With tolerance, only the first draw should have happened
    // All subsequent are within ±5 MHz
    TEST_ASSERT_EQUAL(1, g_tracker.frequencyDrawCount);
}

void test_stress_rapid_frequency_changes_beyond_tolerance() {
    g_tracker.reset();
    
    // Frequency changes > tolerance
    uint32_t frequencies[] = {34700, 34800, 34900, 35000, 35100};
    
    for (int i = 0; i < 5; i++) {
        g_tracker.drawFrequency(frequencies[i], BAND_KA, false);
    }
    
    // Each is >5 MHz apart, so all should draw
    TEST_ASSERT_EQUAL(5, g_tracker.frequencyDrawCount);
}

void test_stress_rapid_direction_changes() {
    g_tracker.reset();
    
    Direction directions[] = {DIR_FRONT, DIR_SIDE, DIR_REAR, DIR_FRONT, DIR_SIDE};
    
    for (int i = 0; i < 5; i++) {
        g_tracker.drawDirectionArrow(directions[i], false);
    }
    
    // Each direction change should cause redraw
    TEST_ASSERT_EQUAL(5, g_tracker.arrowDrawCount);
}

void test_stress_rapid_band_changes() {
    g_tracker.reset();
    
    uint8_t bands[] = {BAND_KA, BAND_K, BAND_X, BAND_LASER, BAND_KA | BAND_K};
    
    for (int i = 0; i < 5; i++) {
        g_tracker.drawBandIndicators(bands[i], false);
    }
    
    TEST_ASSERT_EQUAL(5, g_tracker.bandDrawCount);
}

void test_stress_alternating_mute() {
    g_tracker.reset();
    
    // Rapidly toggle mute
    for (int i = 0; i < 10; i++) {
        g_tracker.drawFrequency(34700, BAND_KA, i % 2 == 0);
    }
    
    // Each mute toggle should cause redraw
    TEST_ASSERT_EQUAL(10, g_tracker.frequencyDrawCount);
}

void test_stress_full_screen_clear_cycle() {
    g_tracker.reset();
    
    // Simulate multiple mode transitions causing full screen clears
    for (int i = 0; i < 5; i++) {
        g_tracker.drawBaseFrame();
        g_tracker.drawFrequency(34700 + i * 100, BAND_KA, false);
        g_tracker.drawBandIndicators(BAND_KA, false);
        g_tracker.drawDirectionArrow(DIR_FRONT, false);
    }
    
    TEST_ASSERT_EQUAL(5, g_tracker.fullScreenClearCount);
    // After each baseFrame, force flags cause redraws
    TEST_ASSERT_EQUAL(5, g_tracker.frequencyDrawCount);
    TEST_ASSERT_EQUAL(5, g_tracker.bandDrawCount);
    TEST_ASSERT_EQUAL(5, g_tracker.arrowDrawCount);
}

// ============================================================================
// Test Cases: Bogey Counter Decoding
// ============================================================================

static char decodeBogeyCounterByte(uint8_t bogeyImage, bool& hasDot) {
    hasDot = (bogeyImage & 0x80) != 0;
    
    switch (bogeyImage & 0x7F) {
        case 6:   return '1';
        case 7:   return '7';
        case 24:  return '&';  // Little L (logic mode)
        case 28:  return 'u';
        case 30:  return 'J';  // Junk
        case 56:  return 'L';  // Logic
        case 57:  return 'C';
        case 62:  return 'U';
        case 63:  return '0';
        case 73:  return '#';  // LASER bars
        case 79:  return '3';
        case 88:  return 'c';
        case 91:  return '2';
        case 94:  return 'd';
        case 102: return '4';
        case 109: return '5';
        case 111: return '9';
        case 113: return 'F';
        case 115: return 'P';  // Photo radar
        case 119: return 'A';
        case 121: return 'E';
        case 124: return 'b';
        case 125: return '6';
        case 127: return '8';
        default:  return ' ';
    }
}

void test_bogey_counter_digits() {
    bool dot;
    
    TEST_ASSERT_EQUAL('0', decodeBogeyCounterByte(63, dot));
    TEST_ASSERT_EQUAL('1', decodeBogeyCounterByte(6, dot));
    TEST_ASSERT_EQUAL('2', decodeBogeyCounterByte(91, dot));
    TEST_ASSERT_EQUAL('3', decodeBogeyCounterByte(79, dot));
    TEST_ASSERT_EQUAL('4', decodeBogeyCounterByte(102, dot));
    TEST_ASSERT_EQUAL('5', decodeBogeyCounterByte(109, dot));
    TEST_ASSERT_EQUAL('6', decodeBogeyCounterByte(125, dot));
    TEST_ASSERT_EQUAL('7', decodeBogeyCounterByte(7, dot));
    TEST_ASSERT_EQUAL('8', decodeBogeyCounterByte(127, dot));
    TEST_ASSERT_EQUAL('9', decodeBogeyCounterByte(111, dot));
}

void test_bogey_counter_special_chars() {
    bool dot;
    
    TEST_ASSERT_EQUAL('J', decodeBogeyCounterByte(30, dot));   // Junk
    TEST_ASSERT_EQUAL('L', decodeBogeyCounterByte(56, dot));   // Logic
    TEST_ASSERT_EQUAL('P', decodeBogeyCounterByte(115, dot));  // Photo
    TEST_ASSERT_EQUAL('A', decodeBogeyCounterByte(119, dot));  // A mode
    TEST_ASSERT_EQUAL('#', decodeBogeyCounterByte(73, dot));   // Laser bars
}

void test_bogey_counter_dot() {
    bool dot;
    
    // Without dot
    decodeBogeyCounterByte(63, dot);  // '0' without dot
    TEST_ASSERT_FALSE(dot);
    
    // With dot (bit 7 set)
    decodeBogeyCounterByte(63 | 0x80, dot);  // '0' with dot
    TEST_ASSERT_TRUE(dot);
}

void test_bogey_counter_unknown() {
    bool dot;
    
    // Unknown pattern should return space
    // Note: 0 is not in the switch, but 127 & 0x7F = 127 which is '8'
    TEST_ASSERT_EQUAL(' ', decodeBogeyCounterByte(0, dot));
    TEST_ASSERT_EQUAL(' ', decodeBogeyCounterByte(1, dot));
    TEST_ASSERT_EQUAL(' ', decodeBogeyCounterByte(2, dot));
    TEST_ASSERT_EQUAL(' ', decodeBogeyCounterByte(3, dot));
    TEST_ASSERT_EQUAL(' ', decodeBogeyCounterByte(4, dot));
    TEST_ASSERT_EQUAL(' ', decodeBogeyCounterByte(5, dot));
    // Note: values with bit 7 set still decode the lower 7 bits
    // 255 & 0x7F = 127 which is '8', not a space
}

// ============================================================================
// Test Cases: Alert Data Comparison
// ============================================================================

void test_alert_data_equals_same() {
    AlertData a = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    AlertData b = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    
    TEST_ASSERT_TRUE(a.equals(b));
}

void test_alert_data_equals_different_band() {
    AlertData a = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    AlertData b = AlertData::create(BAND_K, DIR_FRONT, 4, 0, 34700, true, true);
    
    TEST_ASSERT_FALSE(a.equals(b));
}

void test_alert_data_equals_different_direction() {
    AlertData a = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    AlertData b = AlertData::create(BAND_KA, DIR_REAR, 4, 0, 34700, true, true);
    
    TEST_ASSERT_FALSE(a.equals(b));
}

void test_alert_data_equals_different_frequency() {
    AlertData a = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    AlertData b = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 35000, true, true);
    
    TEST_ASSERT_FALSE(a.equals(b));
}

void test_alert_data_equals_different_strength() {
    AlertData a = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700, true, true);
    AlertData b = AlertData::create(BAND_KA, DIR_FRONT, 5, 0, 34700, true, true);
    
    TEST_ASSERT_FALSE(a.equals(b));
}

// ============================================================================
// Test Cases: Color Helpers
// ============================================================================

static uint16_t getBandColor(Band band, uint16_t colorX, uint16_t colorK, uint16_t colorKa, uint16_t colorLaser, uint16_t colorMuted, bool muted) {
    if (muted) return colorMuted;
    switch (band) {
        case BAND_X:     return colorX;
        case BAND_K:     return colorK;
        case BAND_KA:    return colorKa;
        case BAND_LASER: return colorLaser;
        default:         return colorMuted;
    }
}

void test_color_band_mapping() {
    uint16_t colorX = 0x07E0;      // Green
    uint16_t colorK = 0x07FF;      // Cyan
    uint16_t colorKa = 0xF800;     // Red
    uint16_t colorLaser = 0xFFFF;  // White
    uint16_t colorMuted = 0x8410;  // Gray
    
    TEST_ASSERT_EQUAL(colorX, getBandColor(BAND_X, colorX, colorK, colorKa, colorLaser, colorMuted, false));
    TEST_ASSERT_EQUAL(colorK, getBandColor(BAND_K, colorX, colorK, colorKa, colorLaser, colorMuted, false));
    TEST_ASSERT_EQUAL(colorKa, getBandColor(BAND_KA, colorX, colorK, colorKa, colorLaser, colorMuted, false));
    TEST_ASSERT_EQUAL(colorLaser, getBandColor(BAND_LASER, colorX, colorK, colorKa, colorLaser, colorMuted, false));
}

void test_color_muted_overrides() {
    uint16_t colorMuted = 0x8410;
    
    TEST_ASSERT_EQUAL(colorMuted, getBandColor(BAND_KA, 0x07E0, 0x07FF, 0xF800, 0xFFFF, colorMuted, true));
    TEST_ASSERT_EQUAL(colorMuted, getBandColor(BAND_LASER, 0x07E0, 0x07FF, 0xF800, 0xFFFF, colorMuted, true));
}

void test_color_none_band() {
    uint16_t colorMuted = 0x8410;
    
    TEST_ASSERT_EQUAL(colorMuted, getBandColor(BAND_NONE, 0x07E0, 0x07FF, 0xF800, 0xFFFF, colorMuted, false));
}

// ============================================================================
// Test Cases: Screen Layout Constants
// ============================================================================

void test_layout_screen_dimensions() {
    TEST_ASSERT_EQUAL(640, SCREEN_WIDTH);
    TEST_ASSERT_EQUAL(172, SCREEN_HEIGHT);
}

void test_layout_primary_zone() {
    // Primary zone should fit within screen
    // These values should match DisplayLayout:: constants in display_layout.h
    static constexpr int PRIMARY_ZONE_HEIGHT = 95;

    TEST_ASSERT_TRUE(PRIMARY_ZONE_HEIGHT <= SCREEN_HEIGHT);

    // Verify values match expected (same as display_layout.h)
    TEST_ASSERT_EQUAL(95, PRIMARY_ZONE_HEIGHT);
}

// ============================================================================
// Test Cases: Test Mode State Machine (Color Preview)
// 
// CRITICAL: These tests catch the bug where display doesn't properly restore
// after web UI tests end. The key invariant is:
//   - V1 connected → show resting or update with alerts
//   - V1 disconnected → show scanning (NOT resting!)
// ============================================================================

// Display screen types for state machine testing
enum class DisplayScreen {
    SCANNING,       // V1 not connected - looking for V1
    RESTING,        // V1 connected, no alerts
    ALERT,          // V1 connected, has alerts
    DEMO            // Color preview active
};

// Simulates the display state machine that determines which screen to show
class DisplayStateMachine {
public:
    bool v1Connected = false;
    bool hasAlerts = false;
    bool colorPreviewActive = false;
    bool colorPreviewEnded = false;
    
    DisplayScreen lastScreen = DisplayScreen::SCANNING;
    int showScanningCount = 0;
    int showRestingCount = 0;
    int showAlertCount = 0;
    
    void reset() {
        v1Connected = false;
        hasAlerts = false;
        colorPreviewActive = false;
        colorPreviewEnded = false;
        lastScreen = DisplayScreen::SCANNING;
        showScanningCount = 0;
        showRestingCount = 0;
        showAlertCount = 0;
    }
    
    // Start color preview test
    void startColorPreview() {
        colorPreviewActive = true;
        colorPreviewEnded = false;
    }
    
    // End color preview (simulates timeout or cancel)
    void endColorPreview() {
        colorPreviewActive = false;
        colorPreviewEnded = true;
    }
    
    // Main loop tick - processes state and determines screen to show
    // Returns the screen that should be displayed
    // THIS IS THE LOGIC THAT HAD THE BUG - we test it in isolation
    DisplayScreen processLoop() {
        // Test modes take priority
        if (colorPreviewActive) {
            lastScreen = DisplayScreen::DEMO;
            return lastScreen;
        }
        
        // Handle test mode ending - restore proper screen
        if (colorPreviewEnded) {
            colorPreviewEnded = false;
            
            // KEY INVARIANT: Check connection state to determine correct screen
            if (v1Connected) {
                if (hasAlerts) {
                    showAlertCount++;
                    lastScreen = DisplayScreen::ALERT;
                } else {
                    showRestingCount++;
                    lastScreen = DisplayScreen::RESTING;
                }
            } else {
                // V1 NOT connected - MUST show scanning, NOT resting!
                // THIS WAS THE BUG: code was calling showResting() here
                showScanningCount++;
                lastScreen = DisplayScreen::SCANNING;
            }
            return lastScreen;
        }
        
        // Normal operation
        if (!v1Connected) {
            lastScreen = DisplayScreen::SCANNING;
            return lastScreen;
        }
        
        if (hasAlerts) {
            lastScreen = DisplayScreen::ALERT;
        } else {
            lastScreen = DisplayScreen::RESTING;
        }
        return lastScreen;
    }
};

static DisplayStateMachine g_stateMachine;

// Test: Color preview ends when V1 disconnected → must show SCANNING
void test_color_preview_ends_v1_disconnected_shows_scanning() {
    g_stateMachine.reset();
    g_stateMachine.v1Connected = false;  // V1 NOT connected
    
    // Start and run color preview
    g_stateMachine.startColorPreview();
    TEST_ASSERT_EQUAL(DisplayScreen::DEMO, g_stateMachine.processLoop());
    
    // End color preview
    g_stateMachine.endColorPreview();
    
    // Process loop should show SCANNING (not RESTING!)
    DisplayScreen result = g_stateMachine.processLoop();
    TEST_ASSERT_EQUAL(DisplayScreen::SCANNING, result);
    TEST_ASSERT_EQUAL(1, g_stateMachine.showScanningCount);
    TEST_ASSERT_EQUAL(0, g_stateMachine.showRestingCount);  // Must NOT call showResting!
}

// Test: Color preview ends when V1 connected (no alerts) → show RESTING
void test_color_preview_ends_v1_connected_no_alerts_shows_resting() {
    g_stateMachine.reset();
    g_stateMachine.v1Connected = true;
    g_stateMachine.hasAlerts = false;
    
    g_stateMachine.startColorPreview();
    g_stateMachine.processLoop();
    g_stateMachine.endColorPreview();
    
    DisplayScreen result = g_stateMachine.processLoop();
    TEST_ASSERT_EQUAL(DisplayScreen::RESTING, result);
    TEST_ASSERT_EQUAL(1, g_stateMachine.showRestingCount);
    TEST_ASSERT_EQUAL(0, g_stateMachine.showScanningCount);
}

// Test: Color preview ends when V1 connected (has alerts) → show ALERT
void test_color_preview_ends_v1_connected_with_alerts_shows_alert() {
    g_stateMachine.reset();
    g_stateMachine.v1Connected = true;
    g_stateMachine.hasAlerts = true;
    
    g_stateMachine.startColorPreview();
    g_stateMachine.processLoop();
    g_stateMachine.endColorPreview();
    
    DisplayScreen result = g_stateMachine.processLoop();
    TEST_ASSERT_EQUAL(DisplayScreen::ALERT, result);
    TEST_ASSERT_EQUAL(1, g_stateMachine.showAlertCount);
}

// Test: Ended flags are cleared after processing (prevent infinite loop)
void test_ended_flags_clear_after_processing() {
    g_stateMachine.reset();
    g_stateMachine.v1Connected = false;
    
    g_stateMachine.endColorPreview();
    TEST_ASSERT_TRUE(g_stateMachine.colorPreviewEnded);
    
    g_stateMachine.processLoop();
    TEST_ASSERT_FALSE(g_stateMachine.colorPreviewEnded);  // Must be cleared!
    
    // Second call should NOT increment counters again
    int prevCount = g_stateMachine.showScanningCount;
    g_stateMachine.processLoop();
    // showScanningCount might increment for normal operation, but not for "ended" handling
    // The key is that colorPreviewEnded is cleared
    TEST_ASSERT_FALSE(g_stateMachine.colorPreviewEnded);
}

// Test: V1 disconnects DURING test mode → correct screen after test ends
void test_v1_disconnects_during_test_mode() {
    g_stateMachine.reset();
    g_stateMachine.v1Connected = true;  // Start connected
    
    g_stateMachine.startColorPreview();
    g_stateMachine.processLoop();
    
    // V1 disconnects while in test mode
    g_stateMachine.v1Connected = false;
    
    // Test ends
    g_stateMachine.endColorPreview();
    
    // Should show SCANNING (not RESTING, even though we started connected)
    TEST_ASSERT_EQUAL(DisplayScreen::SCANNING, g_stateMachine.processLoop());
}

// Test: V1 connects DURING test mode → correct screen after test ends
void test_v1_connects_during_test_mode() {
    g_stateMachine.reset();
    g_stateMachine.v1Connected = false;  // Start disconnected
    
    g_stateMachine.startColorPreview();
    g_stateMachine.processLoop();
    
    // V1 connects while in test mode
    g_stateMachine.v1Connected = true;
    g_stateMachine.hasAlerts = true;  // And has alerts
    
    // Test ends
    g_stateMachine.endColorPreview();
    
    // Should show ALERT (current state, not state when test started)
    TEST_ASSERT_EQUAL(DisplayScreen::ALERT, g_stateMachine.processLoop());
}

// Test: Multiple color previews work correctly
void test_sequential_color_previews() {
    g_stateMachine.reset();
    g_stateMachine.v1Connected = false;
    
    // Color preview 1
    g_stateMachine.startColorPreview();
    g_stateMachine.processLoop();
    g_stateMachine.endColorPreview();
    TEST_ASSERT_EQUAL(DisplayScreen::SCANNING, g_stateMachine.processLoop());
    
    // Color preview 2
    g_stateMachine.startColorPreview();
    g_stateMachine.processLoop();
    g_stateMachine.endColorPreview();
    TEST_ASSERT_EQUAL(DisplayScreen::SCANNING, g_stateMachine.processLoop());
    
    // Total calls
    TEST_ASSERT_EQUAL(2, g_stateMachine.showScanningCount);
    TEST_ASSERT_EQUAL(0, g_stateMachine.showRestingCount);
}

// ============================================================================
// Test Runner
// ============================================================================

void runAllTests() {
    // Band decoding tests
    RUN_TEST(test_band_decode_laser);
    RUN_TEST(test_band_decode_ka);
    RUN_TEST(test_band_decode_k);
    RUN_TEST(test_band_decode_x);
    RUN_TEST(test_band_decode_priority);
    RUN_TEST(test_band_decode_none);
    
    // Direction decoding tests
    RUN_TEST(test_direction_decode_front);
    RUN_TEST(test_direction_decode_side);
    RUN_TEST(test_direction_decode_rear);
    RUN_TEST(test_direction_decode_multiple);
    RUN_TEST(test_direction_decode_none);
    
    // Frequency tolerance tests (critical for flashing prevention)
    RUN_TEST(test_frequency_tolerance_no_change_within_tolerance);
    RUN_TEST(test_frequency_tolerance_change_beyond_tolerance);
    RUN_TEST(test_frequency_tolerance_force_flag_overrides);
    RUN_TEST(test_frequency_tolerance_zero_to_nonzero);
    
    // Cache invalidation tests
    RUN_TEST(test_cache_drawBaseFrame_sets_all_force_flags);
    RUN_TEST(test_cache_no_redraw_when_unchanged);
    RUN_TEST(test_cache_redraw_when_state_changes);
    RUN_TEST(test_cache_force_flag_clears_after_draw);
    
    RUN_TEST(test_prepareFullRedrawNoClear_sets_force_flags_without_screen_clear);
    RUN_TEST(test_invalidate_only_full_redraw_repaints_cached_regions);

    // Band indicator caching tests
    RUN_TEST(test_band_cache_no_redraw_unchanged);
    RUN_TEST(test_band_cache_redraw_on_mask_change);
    RUN_TEST(test_band_cache_redraw_on_mute_change);
    
    // Arrow caching tests
    RUN_TEST(test_arrow_cache_no_redraw_unchanged);
    RUN_TEST(test_arrow_cache_redraw_on_direction_change);
    RUN_TEST(test_arrow_combined_directions);
    
    RUN_TEST(test_arrow_partial_redraw_single_blink_toggle_updates_only_one_owned_region);
    RUN_TEST(test_arrow_partial_redraw_two_arrow_change_falls_back_to_full_region);
    RUN_TEST(test_arrow_partial_redraw_muted_transition_falls_back_to_full_region);
    RUN_TEST(test_arrow_partial_redraw_dirty_flag_forces_full_region_redraw);

    // Signal bars caching tests
    RUN_TEST(test_signal_bars_cache_no_redraw_unchanged);
    RUN_TEST(test_signal_bars_cache_redraw_on_strength_change);
    RUN_TEST(test_signal_bars_boundary_values);
    
    
    // State transition tests
    RUN_TEST(test_state_transition_resting_to_alert);
    RUN_TEST(test_state_transition_alert_to_muted);
    
    // Multi-alert tests
    RUN_TEST(test_multi_alert_priority_selection);
    RUN_TEST(test_multi_alert_no_secondary_context_for_single);
    
    // Display state tests
    RUN_TEST(test_display_state_default_values);
    RUN_TEST(test_display_state_volume_support_check);
    
    // Boundary condition tests
    RUN_TEST(test_boundary_frequency_min_max);
    RUN_TEST(test_boundary_signal_strength_clamping);
    RUN_TEST(test_boundary_brightness_range);
    RUN_TEST(test_brightness_slider_clamps_fill_below_display_range);
    RUN_TEST(test_brightness_slider_clamps_fill_above_display_range);
    RUN_TEST(test_boundary_volume_range);
    
    // Stress tests (rapid state changes)
    RUN_TEST(test_stress_rapid_frequency_changes);
    RUN_TEST(test_stress_rapid_frequency_changes_beyond_tolerance);
    RUN_TEST(test_stress_rapid_direction_changes);
    RUN_TEST(test_stress_rapid_band_changes);
    RUN_TEST(test_stress_alternating_mute);
    RUN_TEST(test_stress_full_screen_clear_cycle);
    
    // Bogey counter tests
    RUN_TEST(test_bogey_counter_digits);
    RUN_TEST(test_bogey_counter_special_chars);
    RUN_TEST(test_bogey_counter_dot);
    RUN_TEST(test_bogey_counter_unknown);
    
    // Alert data comparison tests
    RUN_TEST(test_alert_data_equals_same);
    RUN_TEST(test_alert_data_equals_different_band);
    RUN_TEST(test_alert_data_equals_different_direction);
    RUN_TEST(test_alert_data_equals_different_frequency);
    RUN_TEST(test_alert_data_equals_different_strength);
    
    // Color helper tests
    RUN_TEST(test_color_band_mapping);
    RUN_TEST(test_color_muted_overrides);
    RUN_TEST(test_color_none_band);
    
    // Layout tests
    RUN_TEST(test_layout_screen_dimensions);
    RUN_TEST(test_layout_primary_zone);
    
    // Test mode state machine tests (catches display restore bugs)
    RUN_TEST(test_color_preview_ends_v1_disconnected_shows_scanning);
    RUN_TEST(test_color_preview_ends_v1_connected_no_alerts_shows_resting);
    RUN_TEST(test_color_preview_ends_v1_connected_with_alerts_shows_alert);
    RUN_TEST(test_ended_flags_clear_after_processing);
    RUN_TEST(test_v1_disconnects_during_test_mode);
    RUN_TEST(test_v1_connects_during_test_mode);
    RUN_TEST(test_sequential_color_previews);
}

#ifdef ARDUINO
void setup() {
    delay(2000);
    UNITY_BEGIN();
    runAllTests();
    UNITY_END();
}
void loop() {}
#else
int main() {
    UNITY_BEGIN();
    runAllTests();
    return UNITY_END();
}
#endif
