#include "display_preview_module.h"

#include "display_visual_contract.h"
#include "modules/alp/alp_runtime_module.h" // AlpState enum
#include "perf_metrics.h"

#include <algorithm>
#include <cstring>

// ============================================================================
// Step table — the comprehensive display test sequence
// ============================================================================
//
// Phase 1: Band + direction sweep (each band: front → side → rear)
//          Signal strength ramps per direction to show bar range.
// Phase 2: Multi-alert combos with priority-only visuals
// Phase 3: ALP state cycling with gun abbreviation frequency override
// Phase 4: Status indicator cycling (bogey counter, mode, OBD, BLE, volume)
//
// Step duration: 2 seconds each. Total ~80 seconds.
//
// Shorthand for the step struct (keeps table readable):
//   {band, dir, freq, fBars, rBars, flags,
//    secBand, secDir, secFreq, secFBars, secRBars,
//    thirdBand, thirdDir, thirdFreq, thirdFBars, thirdRBars,
//    bogeyChar, modeChar, profileSlot,
//    alpState, alpHb, obdState, bleState,
//    mainVol, muteVol, alpGunAbbrev}

#define NO_SEC BAND_NONE, DIR_NONE, 0, 0, 0
#define NO_THIRD BAND_NONE, DIR_NONE, 0, 0, 0
#define NO_CHG -1  // Don't change carry-forward state
#define AUTO_BC -1 // Auto bogey counter from alert count

const DisplayPreviewModule::PreviewStep DisplayPreviewModule::STEPS[] = {

    // ════════════════════════════════════════════════════════════════════
    // PHASE 1: Band + Direction Sweep
    // ════════════════════════════════════════════════════════════════════
    // The top-counter symbols deliberately sweep the protocol domain across
    // these otherwise independent frames. This makes the physical bench cover
    // every glyph that decodeBogeyCounterByte() can emit while preserving the
    // existing 44-step sequence and transition discovery.

    // X band (10.525 GHz) — front, side, rear
    {BAND_X, DIR_FRONT, 10525, 2, 0, FLAG_NONE, NO_SEC, NO_THIRD, '0', 'A',
     0, // bogey=0 | mode=All Bogeys | profile=Default
     NO_CHG, NO_CHG, NO_CHG, NO_CHG, 5, 0, nullptr},

    {BAND_X, DIR_SIDE, 10525, 4, 0, FLAG_NONE, NO_SEC, NO_THIRD, '6', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_X, DIR_REAR, 10525, 0, 5, FLAG_NONE, NO_SEC, NO_THIRD, '7', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // K band (24.150 GHz) — front, side, rear
    {BAND_K, DIR_FRONT, 24150, 3, 0, FLAG_NONE, NO_SEC, NO_THIRD, '8', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_K, DIR_SIDE, 24150, 5, 0, FLAG_NONE, NO_SEC, NO_THIRD, '9', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_K, DIR_REAR, 24150, 0, 6, FLAG_NONE, NO_SEC, NO_THIRD, '&', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // Ka band (33.800 GHz) — front, side, rear
    {BAND_KA, DIR_FRONT, 33800, 2, 0, FLAG_NONE, NO_SEC, NO_THIRD, 'u', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_KA, DIR_SIDE, 33800, 4, 0, FLAG_NONE, NO_SEC, NO_THIRD, 'C', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_KA, DIR_REAR, 33800, 0, 6, FLAG_NONE, NO_SEC, NO_THIRD, 'U', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // Ka band (34.700 GHz) — front, side, rear
    {BAND_KA, DIR_FRONT, 34700, 3, 0, FLAG_NONE, NO_SEC, NO_THIRD, '#', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_KA, DIR_SIDE, 34700, 5, 0, FLAG_NONE, NO_SEC, NO_THIRD, 'c', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_KA, DIR_REAR, 34700, 0, 5, FLAG_NONE, NO_SEC, NO_THIRD, 'd', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // Ka band (35.500 GHz) — front, side, rear
    {BAND_KA, DIR_FRONT, 35500, 4, 0, FLAG_NONE, NO_SEC, NO_THIRD, 'F', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_KA, DIR_SIDE, 35500, 6, 0, FLAG_NONE, NO_SEC, NO_THIRD, 'A', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_KA, DIR_REAR, 35500, 0, 6, FLAG_FLASH_ARROW, NO_SEC, NO_THIRD, 'E', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, nullptr},

    // Laser — front, side, rear (no frequency, full bars)
    {BAND_LASER, DIR_FRONT, 0, 6, 0, FLAG_NONE, NO_SEC, NO_THIRD, 'b', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_LASER, DIR_SIDE, 0, 6, 0, FLAG_NONE, NO_SEC, NO_THIRD, '1', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_LASER, DIR_REAR, 0, 0, 6, FLAG_NONE, NO_SEC, NO_THIRD, '1', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // ════════════════════════════════════════════════════════════════════
    // PHASE 2: Multi-Alert Combos with Cards
    // ════════════════════════════════════════════════════════════════════

    // Ka 34.700 front (priority) + K 24.150 side (card)
    {BAND_KA,  DIR_FRONT, 34700,  5,      0,      FLAG_NONE, BAND_K, DIR_SIDE, 24150,  3,      0,
     NO_THIRD, '2',       NO_CHG, NO_CHG, NO_CHG, NO_CHG,    NO_CHG, NO_CHG,   NO_CHG, NO_CHG, nullptr},

    // Ka 35.500 rear (priority) + K 24.150 front (card)
    {BAND_KA,  DIR_REAR, 35500,  0,      6,      FLAG_NONE, BAND_K, DIR_FRONT, 24150,  4,      0,
     NO_THIRD, '2',      NO_CHG, NO_CHG, NO_CHG, NO_CHG,    NO_CHG, NO_CHG,    NO_CHG, NO_CHG, nullptr},

    // Ka 34.700 front (priority) + Ka 35.500 rear (card) + X 10.525 side (card)
    {BAND_KA, DIR_FRONT, 34700, 6,   0,      FLAG_NONE, BAND_KA, DIR_REAR, 35500,  0,      4,      BAND_X, DIR_SIDE,
     10525,   2,         0,     '3', NO_CHG, NO_CHG,    NO_CHG,  NO_CHG,   NO_CHG, NO_CHG, NO_CHG, NO_CHG, nullptr},

    // Ka 33.800 front (priority, MUTED) + K 24.150 rear (card)
    {BAND_KA,  DIR_FRONT, 33800,  4,      0,      FLAG_MUTED, BAND_K, DIR_REAR, 24150,  0,      3,
     NO_THIRD, '2',       NO_CHG, NO_CHG, NO_CHG, NO_CHG,     NO_CHG, NO_CHG,   NO_CHG, NO_CHG, nullptr},

    // Photo radar: Ka 34.700 front (priority, PHOTO) + K 24.150 side (card)
    {BAND_KA,  DIR_FRONT, 34700,  5,      0,      FLAG_PHOTO, BAND_K, DIR_SIDE, 24150,  3,      0,
     NO_THIRD, 'P',       NO_CHG, NO_CHG, NO_CHG, NO_CHG,     NO_CHG, NO_CHG,   NO_CHG, NO_CHG, nullptr},

    // Junk K: K 24.199 front (priority) — bogey=J
    {BAND_K, DIR_FRONT, 24199, 2, 0, FLAG_NONE, NO_SEC, NO_THIRD, 'J', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // Multiple X bogeys (store doors) — distinct frequencies keep the two
    // secondary sources separate under the card renderer's identity window.
    {BAND_X, DIR_FRONT, 10525, 3,   0,      FLAG_NONE, BAND_X, DIR_SIDE, 10515,  2,      0,      BAND_X, DIR_REAR,
     10535,  0,         1,     '5', NO_CHG, NO_CHG,    NO_CHG, NO_CHG,   NO_CHG, NO_CHG, NO_CHG, NO_CHG, nullptr},

    // ════════════════════════════════════════════════════════════════════
    // PHASE 3: ALP State Cycling
    // ════════════════════════════════════════════════════════════════════
    // Show a Ka alert as backdrop while cycling ALP badge states

    // ALP OFF (grey badge hidden)
    {BAND_KA, DIR_FRONT, 35500, 4, 0, FLAG_NONE, NO_SEC, NO_THIRD, '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::OFF), 0x00, NO_CHG, NO_CHG, NO_CHG, NO_CHG, nullptr},

    // ALP IDLE (grey badge)
    {BAND_KA, DIR_FRONT, 35500, 4, 0, FLAG_NONE, NO_SEC, NO_THIRD, '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::IDLE), 0x00, NO_CHG, NO_CHG, NO_CHG, NO_CHG, nullptr},

    // ALP LISTENING warm-up (green badge, hb=02)
    {BAND_KA, DIR_FRONT, 35500, 4, 0, FLAG_NONE, NO_SEC, NO_THIRD, '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::LISTENING), 0x02, NO_CHG, NO_CHG, NO_CHG, NO_CHG, nullptr},

    // ALP LISTENING DLI active (orange badge, hb=03 — below LID speed limit)
    {BAND_KA, DIR_FRONT, 35500, 4, 0, FLAG_NONE, NO_SEC, NO_THIRD, '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::LISTENING), 0x03, NO_CHG, NO_CHG, NO_CHG, NO_CHG, nullptr},

    // ALP LISTENING LID active (blue badge, hb=04 — above LID speed limit)
    {BAND_KA, DIR_FRONT, 35500, 5, 0, FLAG_NONE, NO_SEC, NO_THIRD, '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::LISTENING), 0x04, NO_CHG, NO_CHG, NO_CHG, NO_CHG, nullptr},

    // ALP ALERT_ACTIVE (blue badge) + Laser front + gun "truSPd" override
    {BAND_LASER, DIR_FRONT, 0, 6, 0, FLAG_NONE, NO_SEC, NO_THIRD, '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::ALERT_ACTIVE), 0x01, NO_CHG, NO_CHG, NO_CHG, NO_CHG, "truSPd"},

    // ALP NOISE_WINDOW (blue badge) + Laser front + gun "drgEYE" override
    {BAND_LASER, DIR_FRONT, 0, 6, 0, FLAG_NONE, NO_SEC, NO_THIRD, '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::NOISE_WINDOW), 0x01, NO_CHG, NO_CHG, NO_CHG, NO_CHG, "drgEYE"},

    // ALP TEARDOWN (orange badge) — re-alert after alert
    {BAND_KA, DIR_FRONT, 35500, 3, 0, FLAG_NONE, NO_SEC, NO_THIRD, '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::TEARDOWN), 0x00, NO_CHG, NO_CHG, NO_CHG, NO_CHG, nullptr},

    // ALP back to LID active (blue, hb=04) — clear gun override
    {BAND_KA, DIR_FRONT, 35500, 4, 0, FLAG_NONE, NO_SEC, NO_THIRD, '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::LISTENING), 0x04, NO_CHG, NO_CHG, NO_CHG, NO_CHG, nullptr},

    // ════════════════════════════════════════════════════════════════════
    // PHASE 4: Status Indicator Cycling
    // ════════════════════════════════════════════════════════════════════

    // OBD connected (green badge) + BLE proxy advertising (blue icon)
    {BAND_KA, DIR_FRONT, 34700, 4, 0, FLAG_NONE, NO_SEC, NO_THIRD, '1', 'A', 0, static_cast<int8_t>(AlpState::OFF),
     0x00,
     1, // OBD connected
     1, // BLE advertising
     7, 2, nullptr},

    // OBD scanning (red badge) + BLE proxy client connected (green icon)
    {BAND_KA, DIR_FRONT, 34700, 5, 0, FLAG_NONE, NO_SEC, NO_THIRD, '1', NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     2, // OBD scanning
     2, // BLE client connected
     NO_CHG, NO_CHG, nullptr},

    // OBD off + BLE off — mode=Logic ('l')
    {BAND_KA, DIR_SIDE, 34700, 4, 0, FLAG_NONE, NO_SEC, NO_THIRD, '1', 'l', NO_CHG, NO_CHG, NO_CHG,
     0, // OBD off
     0, // BLE off
     NO_CHG, NO_CHG, nullptr},

    // Mode=Advanced Logic ('L') + profile=Highway
    {BAND_KA, DIR_FRONT, 35500, 6, 0, FLAG_NONE, NO_SEC, NO_THIRD, '1', 'L', 1, // Advanced Logic | Highway profile
     NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, nullptr},

    // Profile=Comfort + volume 9/5
    {BAND_K, DIR_FRONT, 24150, 3, 0, FLAG_NONE, NO_SEC, NO_THIRD, '1', 'A', 2, // All Bogeys | Comfort profile
     NO_CHG, NO_CHG, NO_CHG, NO_CHG, 9, 5, nullptr},

    // Muted alert + volume 0/0
    {BAND_KA, DIR_FRONT, 34700, 5, 0, FLAG_MUTED, NO_SEC, NO_THIRD, '1', NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     0, 0, nullptr},

    // Bogey counter cycle: '2' with direction breakdown
    {BAND_KA, DIR_FRONT, 35500,  5,        0,   FLAG_NONE, BAND_K, DIR_REAR,
     24150,   0,         3,      NO_THIRD, '2', 'A',       0, // Back to default profile
     NO_CHG,  NO_CHG,    NO_CHG, NO_CHG,   5,   0,         nullptr},

    // Bogey counter: '4' — nest of falses with two distinct card identities
    {BAND_X, DIR_FRONT, 10525, 2,   0,      FLAG_NONE, BAND_X, DIR_SIDE, 10515,  1,      0,      BAND_X, DIR_REAR,
     10535,  0,         1,     '4', NO_CHG, NO_CHG,    NO_CHG, NO_CHG,   NO_CHG, NO_CHG, NO_CHG, NO_CHG, nullptr},

    // Bogey counter: 'L' (Logic mode display)
    {BAND_KA, DIR_FRONT, 34700, 6, 0, FLAG_FLASH_ARROW | FLAG_FLASH_BAND, NO_SEC, NO_THIRD, 'L', 'L', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG, NO_CHG, nullptr},

    // Final frame: clean single Ka alert, all indicators normal
    {BAND_KA, DIR_FRONT, 35500, 6, 0, FLAG_NONE, NO_SEC, NO_THIRD, '1', 'A', 0, static_cast<int8_t>(AlpState::OFF),
     0x00, 0, 0, // OBD off | BLE off
     5, 0, nullptr},
};

const int DisplayPreviewModule::STEP_COUNT =
    sizeof(DisplayPreviewModule::STEPS) / sizeof(DisplayPreviewModule::STEPS[0]);

namespace {

uint8_t maxPreviewBars(uint8_t frontBars, uint8_t rearBars) {
    return std::max(frontBars, rearBars);
}

void copyText(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0)
        return;
    std::strncpy(dst, src ? src : "", dstSize);
    dst[dstSize - 1] = '\0';
}

void fillResolvedAlert(DisplayPreviewModule::ResolvedAlert& out, Band band, Direction dir, uint32_t freqMHz,
                       uint8_t frontBars, uint8_t rearBars, const char* alpGunAbbrev = nullptr) {
    out = DisplayPreviewModule::ResolvedAlert{};
    out.present = (band != BAND_NONE);
    out.band = band;
    out.dir = dir;
    out.freqMHz = freqMHz;
    out.frontBars = frontBars;
    out.rearBars = rearBars;
    out.cardBarCount = maxPreviewBars(frontBars, rearBars);

    char buffer[sizeof(out.frequencyText)] = "";
    const char* text =
        DisplayVisualContract::frequencyTextForAlert(band, freqMHz, alpGunAbbrev, buffer, sizeof(buffer));
    copyText(out.frequencyText, sizeof(out.frequencyText), text);
}

void buildResolvedStepFromCarry(int index, const DisplayPreviewModule::PreviewStep& step,
                                const DisplayPreviewModule::PreviewCarryState& carry,
                                DisplayPreviewModule::ResolvedStep& out) {
    out = DisplayPreviewModule::ResolvedStep{};
    out.index = index;
    out.raw = step;
    out.flags = step.flags;
    out.muted = (step.flags & DisplayPreviewModule::FLAG_MUTED) != 0;
    out.photo = (step.flags & DisplayPreviewModule::FLAG_PHOTO) != 0;

    fillResolvedAlert(out.primary, step.band, step.dir, step.freqMHz, step.frontBars, step.rearBars, step.alpGunAbbrev);
    if (step.secBand != BAND_NONE) {
        fillResolvedAlert(out.secondary, step.secBand, step.secDir, step.secFreqMHz, step.secFrontBars,
                          step.secRearBars);
    }
    if (step.thirdBand != BAND_NONE) {
        fillResolvedAlert(out.third, step.thirdBand, step.thirdDir, step.thirdFreqMHz, step.thirdFrontBars,
                          step.thirdRearBars);
    }

    out.alertCount = 1;
    out.activeBandMask = static_cast<uint8_t>(step.band);
    if (out.secondary.present) {
        ++out.alertCount;
        out.activeBandMask = static_cast<uint8_t>(out.activeBandMask | static_cast<uint8_t>(step.secBand));
    }
    if (out.third.present) {
        ++out.alertCount;
        out.activeBandMask = static_cast<uint8_t>(out.activeBandMask | static_cast<uint8_t>(step.thirdBand));
    }

    out.activeDirectionMask = static_cast<uint8_t>(step.dir);
    if (step.flags & DisplayPreviewModule::FLAG_FLASH_ARROW) {
        if (step.dir & DIR_FRONT)
            out.flashMask |= 0x20;
        if (step.dir & DIR_SIDE)
            out.flashMask |= 0x40;
        if (step.dir & DIR_REAR)
            out.flashMask |= 0x80;
    }
    if (step.flags & DisplayPreviewModule::FLAG_FLASH_BAND) {
        out.bandFlashMask = static_cast<uint8_t>(step.band);
    }

    out.mainMeterCount =
        DisplayVisualContract::scalePreviewBarsToMainMeter(maxPreviewBars(step.frontBars, step.rearBars));

    out.status.bogeyChar =
        (step.bogeyChar == AUTO_BC) ? static_cast<char>('0' + out.alertCount) : static_cast<char>(step.bogeyChar);
    out.status.modeChar = (carry.modeChar > 0) ? static_cast<char>(carry.modeChar) : 0;
    out.status.hasMode = carry.modeChar > 0;
    out.status.profileSlot = carry.profileSlot;
    out.status.alpState = carry.alpState;
    out.status.alpHbByte1 = static_cast<uint8_t>(carry.alpHbByte1);
    out.status.obdState = carry.obdState;
    out.status.bleState = carry.bleState;
    out.status.mainVolume = static_cast<uint8_t>(carry.mainVolume);
    out.status.muteVolume = static_cast<uint8_t>(carry.muteVolume);
}

} // namespace

// ============================================================================
// Lifecycle
// ============================================================================

DisplayPreviewModule::DisplayPreviewModule() {
    // display set in begin()
}

void DisplayPreviewModule::begin(V1Display* disp) {
    display_ = disp;
}

void DisplayPreviewModule::requestHold(uint32_t durationMs) {
    previewActive_ = true;
    visualPinned_ = false;
    visualPinnedStep_ = -1;
    visualPinnedRenderSeq_ = 0;
    previewStartMs_ = millis();
    const uint32_t autoDurationMs = (static_cast<uint32_t>(STEP_COUNT) * STEP_DURATION_MS) + PREVIEW_TAIL_MS;
    // A zero duration keeps the manual "run the full diagnostic sequence"
    // behavior. Non-zero durations are caller-owned holds, such as the short
    // color-save preview, and must release display ownership when requested.
    previewDurationMs_ = (durationMs == 0) ? autoDurationMs : durationMs;
    loopSequence_ = (durationMs != 0) && (durationMs > autoDurationMs);
    previewStep_ = 0;
    previewEnded_ = false;
    resetCarryState();
    if (display_) {
        display_->setPreviewIndicatorOverridesActive(true);
    }
}

void DisplayPreviewModule::cancel() {
    if (previewActive_ || visualPinned_) {
        previewActive_ = false;
        visualPinned_ = false;
        visualPinnedStep_ = -1;
        visualPinnedRenderSeq_ = 0;
        previewEnded_ = true;
        cleanupPreviewOverrides();
    }
}

bool DisplayPreviewModule::consumeEnded() {
    if (previewEnded_) {
        previewEnded_ = false;
        return true;
    }
    return false;
}

void DisplayPreviewModule::resetCarryState() {
    PreviewCarryState carry;
    resetCarryState(carry);
    currentModeChar_ = carry.modeChar;
    currentProfileSlot_ = carry.profileSlot;
    currentAlpState_ = carry.alpState;
    currentAlpHb_ = carry.alpHbByte1;
    currentObdState_ = carry.obdState;
    currentBleState_ = carry.bleState;
    currentMainVol_ = carry.mainVolume;
    currentMuteVol_ = carry.muteVolume;
}

int DisplayPreviewModule::stepCount() {
    return STEP_COUNT;
}

bool DisplayPreviewModule::rawStep(int index, PreviewStep& out) {
    if (index < 0 || index >= STEP_COUNT) {
        return false;
    }
    out = STEPS[index];
    return true;
}

void DisplayPreviewModule::resetCarryState(PreviewCarryState& state) {
    state.modeChar = 0;
    state.profileSlot = 0;
    state.alpState = static_cast<int8_t>(AlpState::OFF);
    state.alpHbByte1 = 0;
    state.obdState = 0;
    state.bleState = 0;
    state.mainVolume = 5;
    state.muteVolume = 0;
}

void DisplayPreviewModule::applyCarryState(PreviewCarryState& state, const PreviewStep& step) {
    if (step.modeChar != NO_CHG)
        state.modeChar = step.modeChar;
    if (step.profileSlot != NO_CHG)
        state.profileSlot = step.profileSlot;
    if (step.alpState != NO_CHG) {
        state.alpState = step.alpState;
        state.alpHbByte1 = step.alpHbByte1;
    }
    if (step.obdState != NO_CHG)
        state.obdState = step.obdState;
    if (step.bleState != NO_CHG)
        state.bleState = step.bleState;
    if (step.mainVolume != NO_CHG)
        state.mainVolume = step.mainVolume;
    if (step.muteVolume != NO_CHG)
        state.muteVolume = step.muteVolume;
}

bool DisplayPreviewModule::resolveStep(int index, ResolvedStep& out) {
    if (index < 0 || index >= STEP_COUNT) {
        return false;
    }
    PreviewCarryState carry;
    resetCarryState(carry);
    for (int i = 0; i <= index; ++i) {
        applyCarryState(carry, STEPS[i]);
    }
    buildResolvedStepFromCarry(index, STEPS[index], carry, out);
    return true;
}

void DisplayPreviewModule::cleanupPreviewOverrides() {
    if (!display_) {
        return;
    }
    display_->clearAlpFrequencyOverride();
    display_->setAlpPreviewState(false, 0, 0);
    display_->setObdPreviewState(false, false, false);
    display_->setBLEProxyStatus(false, false);
    display_->setPreviewIndicatorOverridesActive(false);
}

void DisplayPreviewModule::clearVisualPin() {
    visualPinned_ = false;
    visualPinnedStep_ = -1;
    visualPinnedRenderSeq_ = 0;
    previewActive_ = false;
    if (display_) {
        display_->disableVisualFlushShadow();
    }
    cleanupPreviewOverrides();
}

// ============================================================================
// Main update loop
// ============================================================================

void DisplayPreviewModule::update() {
    if (!previewActive_ || !display_)
        return;

    unsigned long now = millis();
    unsigned long elapsed = now - previewStartMs_;

    if (elapsed >= previewDurationMs_) {
        previewActive_ = false;
        previewEnded_ = true;
        cleanupPreviewOverrides();
        return;
    }

    // Determine which timed frame we should be on.  Short previews and the
    // manual diagnostic run once through the table.  Long qualification holds
    // keep cycling the same visual test instead of parking on the final frame.
    int targetStep = static_cast<int>(elapsed / STEP_DURATION_MS);
    if (!loopSequence_ && targetStep >= STEP_COUNT)
        targetStep = STEP_COUNT - 1;

    // Render at most one preview frame per loop.  If low-priority preview work
    // was skipped under load, drop the missed visual steps and render only the
    // current step; never catch up by issuing multiple display flushes in one
    // loop.  Carry-forward state is still advanced for skipped steps so the
    // rendered step matches the table state it would have had after a normal
    // progression, without paying the display cost for obsolete frames.
    const bool canRenderStep = loopSequence_ || previewStep_ < STEP_COUNT;
    if (targetStep >= previewStep_ && canRenderStep) {
        for (int skipped = previewStep_; skipped < targetStep; ++skipped) {
            applyCarryState(STEPS[skipped % STEP_COUNT]);
        }
        renderStep(targetStep % STEP_COUNT, previewStep_ == 0);
        previewStep_ = targetStep + 1;
    }
}

// ============================================================================
// Step renderer
// ============================================================================

void DisplayPreviewModule::applyCarryState(const PreviewStep& step) {
    PreviewCarryState carry = currentCarryState();
    applyCarryState(carry, step);
    currentModeChar_ = carry.modeChar;
    currentProfileSlot_ = carry.profileSlot;
    currentAlpState_ = carry.alpState;
    currentAlpHb_ = carry.alpHbByte1;
    currentObdState_ = carry.obdState;
    currentBleState_ = carry.bleState;
    currentMainVol_ = carry.mainVolume;
    currentMuteVol_ = carry.muteVolume;
}

DisplayPreviewModule::PreviewCarryState DisplayPreviewModule::currentCarryState() const {
    PreviewCarryState carry;
    carry.modeChar = currentModeChar_;
    carry.profileSlot = currentProfileSlot_;
    carry.alpState = currentAlpState_;
    carry.alpHbByte1 = currentAlpHb_;
    carry.obdState = currentObdState_;
    carry.bleState = currentBleState_;
    carry.mainVolume = currentMainVol_;
    carry.muteVolume = currentMuteVol_;
    return carry;
}

static AlertData buildAlertData(const DisplayPreviewModule::ResolvedAlert& resolved, bool priority) {
    AlertData primary{};
    primary.band = resolved.band;
    primary.direction = resolved.dir;
    primary.frontStrength = resolved.frontBars;
    primary.rearStrength = resolved.rearBars;
    primary.frequency = resolved.freqMHz;
    primary.isValid = resolved.present;
    primary.isPriority = priority;
    return primary;
}

void DisplayPreviewModule::renderStep(int stepIndex, bool firstFrame) {
    if (stepIndex < 0 || stepIndex >= STEP_COUNT)
        return;
    const PreviewStep& step = STEPS[stepIndex];

    applyCarryState(step);

    ResolvedStep resolved;
    buildResolvedStepFromCarry(stepIndex, step, currentCarryState(), resolved);
    renderResolvedStep(resolved, firstFrame);
}

void DisplayPreviewModule::renderResolvedStep(const ResolvedStep& resolved, bool firstFrame) {
    if (!display_) {
        return;
    }

    // ── Build alert array ───────────────────────────────────────────

    AlertData allAlerts[3];
    int alertCount = 1;
    AlertData primary = buildAlertData(resolved.primary, true);

    if (resolved.photo) {
        primary.photoType = 1; // Generic photo type
    }

    allAlerts[0] = primary;

    if (resolved.secondary.present) {
        allAlerts[alertCount] = buildAlertData(resolved.secondary, false);
        alertCount++;
    }

    if (resolved.third.present) {
        allAlerts[alertCount] = buildAlertData(resolved.third, false);
        alertCount++;
    }

    // ── Build display state ─────────────────────────────────────────

    DisplayState state{};
    state.activeBands = resolved.activeBandMask;
    state.arrows = resolved.primary.dir;
    state.priorityArrow = resolved.primary.dir;
    state.signalBars = resolved.mainMeterCount;
    state.muted = resolved.muted;
    state.displayOn = true;
    state.hasDisplayOn = true;
    state.hasVolumeData = true;
    state.mainVolume = resolved.status.mainVolume;
    state.muteVolume = resolved.status.muteVolume;

    // Mode
    if (resolved.status.hasMode) {
        state.modeChar = resolved.status.modeChar;
        state.hasMode = true;
    }

    state.flashBits = resolved.flashMask;
    state.bandFlashBits = resolved.bandFlashMask;
    state.bogeyCounterChar = resolved.status.bogeyChar;

    // Photo flag
    if (resolved.photo) {
        state.hasPhotoAlert = true;
    }

    // ── Set indicator overrides ─────────────────────────────────────

    // ALP badge
    bool alpOn = (resolved.status.alpState != static_cast<int8_t>(AlpState::OFF));
    display_->setAlpPreviewState(alpOn, static_cast<uint8_t>(resolved.status.alpState), resolved.status.alpHbByte1);

    // ALP gun frequency override
    if (resolved.raw.alpGunAbbrev) {
        display_->setAlpFrequencyOverride(resolved.raw.alpGunAbbrev);
    } else {
        display_->clearAlpFrequencyOverride();
    }

    // OBD badge
    switch (resolved.status.obdState) {
    case 1:
        display_->setObdPreviewState(true, true, false);
        break; // Connected
    case 2:
        display_->setObdPreviewState(true, false, true);
        break; // Scanning
    default:
        display_->setObdPreviewState(false, false, false);
        break; // Off
    }

    // BLE proxy indicator
    switch (resolved.status.bleState) {
    case 1:
        display_->setBLEProxyStatus(true, false);
        break; // Advertising (blue)
    case 2:
        display_->setBLEProxyStatus(true, true);
        break; // Client connected (green)
    default:
        display_->setBLEProxyStatus(false, false);
        break; // Off
    }

    // Profile
    display_->setProfileIndicatorSlot(resolved.status.profileSlot);

    // ── Render ──────────────────────────────────────────────────────

    perfSetDisplayRenderScenario(firstFrame ? PerfDisplayRenderScenario::PreviewFirstFrame
                                            : PerfDisplayRenderScenario::PreviewSteadyFrame);
    const unsigned long renderStartUs = micros();
    display_->update(primary, allAlerts, alertCount, state);
    perfRecordDisplayScenarioRenderUs(micros() - renderStartUs);
    perfClearDisplayRenderScenario();
}

bool DisplayPreviewModule::pinStep(int index, bool clear, uint32_t* renderSeqOut) {
    if (!display_ || index < 0 || index >= STEP_COUNT) {
        return false;
    }

    ResolvedStep resolved;
    if (!resolveStep(index, resolved)) {
        return false;
    }

    previewActive_ = false;
    loopSequence_ = false;
    previewStep_ = index + 1;
    previewEnded_ = false;
    visualPinned_ = true;
    visualPinnedStep_ = index;
    display_->setPreviewIndicatorOverridesActive(true);
    display_->setVisualTestBlinkPhase(true, millis());
    // Bench-only: mirror every flush while pinned so the host can prove the
    // panel received exactly what the framebuffer holds. Best effort — the
    // flushshadow route reports 503 if the allocation failed.
    display_->enableVisualFlushShadow();

    if (clear) {
        display_->resetChangeTracking();
        display_->forceNextRedraw();
        display_->clear();
    }

    renderResolvedStep(resolved, clear);
    visualPinnedRenderSeq_ = display_->renderSequenceId();
    if (renderSeqOut) {
        *renderSeqOut = visualPinnedRenderSeq_;
    }
    return true;
}
