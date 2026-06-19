#include "display_preview_module.h"

#include "modules/alp/alp_runtime_module.h"  // AlpState enum
#include "perf_metrics.h"

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

#define NO_SEC   BAND_NONE, DIR_NONE, 0, 0, 0
#define NO_THIRD BAND_NONE, DIR_NONE, 0, 0, 0
#define NO_CHG   -1          // Don't change carry-forward state
#define AUTO_BC  -1          // Auto bogey counter from alert count

const DisplayPreviewModule::PreviewStep DisplayPreviewModule::STEPS[] = {

    // ════════════════════════════════════════════════════════════════════
    // PHASE 1: Band + Direction Sweep
    // ════════════════════════════════════════════════════════════════════

    // X band (10.525 GHz) — front, side, rear
    {BAND_X, DIR_FRONT, 10525, 2, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', 'A', 0,       // bogey=1 | mode=All Bogeys | profile=Default
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     5, 0, nullptr},

    {BAND_X, DIR_SIDE, 10525, 4, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_X, DIR_REAR, 10525, 0, 5, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // K band (24.150 GHz) — front, side, rear
    {BAND_K, DIR_FRONT, 24150, 3, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_K, DIR_SIDE, 24150, 5, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_K, DIR_REAR, 24150, 0, 6, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // Ka band (33.800 GHz) — front, side, rear
    {BAND_KA, DIR_FRONT, 33800, 2, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_KA, DIR_SIDE, 33800, 4, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_KA, DIR_REAR, 33800, 0, 6, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // Ka band (34.700 GHz) — front, side, rear
    {BAND_KA, DIR_FRONT, 34700, 3, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_KA, DIR_SIDE, 34700, 5, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_KA, DIR_REAR, 34700, 0, 5, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // Ka band (35.500 GHz) — front, side, rear
    {BAND_KA, DIR_FRONT, 35500, 4, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_KA, DIR_SIDE, 35500, 6, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_KA, DIR_REAR, 35500, 0, 6, FLAG_FLASH_ARROW,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // Laser — front, side, rear (no frequency, full bars)
    {BAND_LASER, DIR_FRONT, 0, 6, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_LASER, DIR_SIDE, 0, 6, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    {BAND_LASER, DIR_REAR, 0, 0, 6, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // ════════════════════════════════════════════════════════════════════
    // PHASE 2: Multi-Alert Combos with Cards
    // ════════════════════════════════════════════════════════════════════

    // Ka 34.700 front (priority) + K 24.150 side (card)
    {BAND_KA, DIR_FRONT, 34700, 5, 0, FLAG_NONE,
     BAND_K, DIR_SIDE, 24150, 3, 0,
     NO_THIRD,
     '2', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // Ka 35.500 rear (priority) + K 24.150 front (card)
    {BAND_KA, DIR_REAR, 35500, 0, 6, FLAG_NONE,
     BAND_K, DIR_FRONT, 24150, 4, 0,
     NO_THIRD,
     '2', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // Ka 34.700 front (priority) + Ka 35.500 rear (card) + X 10.525 side (card)
    {BAND_KA, DIR_FRONT, 34700, 6, 0, FLAG_NONE,
     BAND_KA, DIR_REAR, 35500, 0, 4,
     BAND_X, DIR_SIDE, 10525, 2, 0,
     '3', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // Ka 33.800 front (priority, MUTED) + K 24.150 rear (card)
    {BAND_KA, DIR_FRONT, 33800, 4, 0, FLAG_MUTED,
     BAND_K, DIR_REAR, 24150, 0, 3,
     NO_THIRD,
     '2', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // Photo radar: Ka 34.700 front (priority, PHOTO) + K 24.150 side (card)
    {BAND_KA, DIR_FRONT, 34700, 5, 0, FLAG_PHOTO,
     BAND_K, DIR_SIDE, 24150, 3, 0,
     NO_THIRD,
     'P', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // Junk K: K 24.199 front (priority) — bogey=J
    {BAND_K, DIR_FRONT, 24199, 2, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     'J', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // Multiple X bogeys (store doors) — X front + X side + X rear
    {BAND_X, DIR_FRONT, 10525, 3, 0, FLAG_NONE,
     BAND_X, DIR_SIDE, 10525, 2, 0,
     BAND_X, DIR_REAR, 10525, 0, 1,
     '5', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // ════════════════════════════════════════════════════════════════════
    // PHASE 3: ALP State Cycling
    // ════════════════════════════════════════════════════════════════════
    // Show a Ka alert as backdrop while cycling ALP badge states

    // ALP OFF (grey badge hidden)
    {BAND_KA, DIR_FRONT, 35500, 4, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::OFF), 0x00, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // ALP IDLE (grey badge)
    {BAND_KA, DIR_FRONT, 35500, 4, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::IDLE), 0x00, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // ALP LISTENING warm-up (green badge, hb=02)
    {BAND_KA, DIR_FRONT, 35500, 4, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::LISTENING), 0x02, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // ALP LISTENING DLI active (orange badge, hb=03 — below LID speed limit)
    {BAND_KA, DIR_FRONT, 35500, 4, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::LISTENING), 0x03, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // ALP LISTENING LID active (blue badge, hb=04 — above LID speed limit)
    {BAND_KA, DIR_FRONT, 35500, 5, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::LISTENING), 0x04, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // ALP ALERT_ACTIVE (blue badge) + Laser front + gun "truSPd" override
    {BAND_LASER, DIR_FRONT, 0, 6, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::ALERT_ACTIVE), 0x01, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, "truSPd"},

    // ALP NOISE_WINDOW (blue badge) + Laser front + gun "drgEYE" override
    {BAND_LASER, DIR_FRONT, 0, 6, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::NOISE_WINDOW), 0x01, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, "drgEYE"},

    // ALP TEARDOWN (orange badge) — re-alert after alert
    {BAND_KA, DIR_FRONT, 35500, 3, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::TEARDOWN), 0x00, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // ALP back to LID active (blue, hb=04) — clear gun override
    {BAND_KA, DIR_FRONT, 35500, 4, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     static_cast<int8_t>(AlpState::LISTENING), 0x04, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // ════════════════════════════════════════════════════════════════════
    // PHASE 4: Status Indicator Cycling
    // ════════════════════════════════════════════════════════════════════

    // OBD connected (green badge) + BLE proxy advertising (blue icon)
    {BAND_KA, DIR_FRONT, 34700, 4, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', 'A', 0,
     static_cast<int8_t>(AlpState::OFF), 0x00,
     1,   // OBD connected
     1,   // BLE advertising
     7, 2, nullptr},

    // OBD scanning (red badge) + BLE proxy client connected (green icon)
    {BAND_KA, DIR_FRONT, 34700, 5, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG,
     2,   // OBD scanning
     2,   // BLE client connected
     NO_CHG, NO_CHG, nullptr},

    // OBD off + BLE off — mode=Logic ('l')
    {BAND_KA, DIR_SIDE, 34700, 4, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', 'l', NO_CHG,
     NO_CHG, NO_CHG,
     0,   // OBD off
     0,   // BLE off
     NO_CHG, NO_CHG, nullptr},

    // Mode=Advanced Logic ('L') + profile=Highway
    {BAND_KA, DIR_FRONT, 35500, 6, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', 'L', 1,       // Advanced Logic | Highway profile
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // Profile=Comfort + volume 9/5
    {BAND_K, DIR_FRONT, 24150, 3, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', 'A', 2,       // All Bogeys | Comfort profile
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     9, 5, nullptr},

    // Muted alert + volume 0/0
    {BAND_KA, DIR_FRONT, 34700, 5, 0, FLAG_MUTED,
     NO_SEC, NO_THIRD,
     '1', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     0, 0, nullptr},

    // Bogey counter cycle: '2' with direction breakdown
    {BAND_KA, DIR_FRONT, 35500, 5, 0, FLAG_NONE,
     BAND_K, DIR_REAR, 24150, 0, 3,
     NO_THIRD,
     '2', 'A', 0,       // Back to default profile
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     5, 0, nullptr},

    // Bogey counter: '4' — nest of falses
    {BAND_X, DIR_FRONT, 10525, 2, 0, FLAG_NONE,
     BAND_X, DIR_SIDE, 10525, 1, 0,
     BAND_X, DIR_REAR, 10525, 0, 1,
     '4', NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // Bogey counter: 'L' (Logic mode display)
    {BAND_KA, DIR_FRONT, 34700, 6, 0, FLAG_FLASH_ARROW | FLAG_FLASH_BAND,
     NO_SEC, NO_THIRD,
     'L', 'L', NO_CHG,
     NO_CHG, NO_CHG, NO_CHG, NO_CHG,
     NO_CHG, NO_CHG, nullptr},

    // Final frame: clean single Ka alert, all indicators normal
    {BAND_KA, DIR_FRONT, 35500, 6, 0, FLAG_NONE,
     NO_SEC, NO_THIRD,
     '1', 'A', 0,
     static_cast<int8_t>(AlpState::OFF), 0x00,
     0, 0,   // OBD off | BLE off
     5, 0, nullptr},
};

const int DisplayPreviewModule::STEP_COUNT =
    sizeof(DisplayPreviewModule::STEPS) / sizeof(DisplayPreviewModule::STEPS[0]);

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
    previewStartMs_ = millis();
    const uint32_t autoDurationMs =
        (static_cast<uint32_t>(STEP_COUNT) * STEP_DURATION_MS) + PREVIEW_TAIL_MS;
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
    if (previewActive_) {
        previewActive_ = false;
        previewEnded_ = true;
        // Clear any ALP frequency override we may have set
        if (display_) {
            display_->clearAlpFrequencyOverride();
            // Reset ALP/OBD to off state
            display_->setAlpPreviewState(false, 0, 0);
            display_->setObdPreviewState(false, false, false);
            display_->setBLEProxyStatus(false, false);
            display_->setPreviewIndicatorOverridesActive(false);
        }
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
    currentModeChar_ = 0;
    currentProfileSlot_ = 0;
    currentAlpState_ = static_cast<int8_t>(AlpState::OFF);
    currentAlpHb_ = 0;
    currentObdState_ = 0;
    currentBleState_ = 0;
    currentMainVol_ = 5;
    currentMuteVol_ = 0;
}

// ============================================================================
// Main update loop
// ============================================================================

void DisplayPreviewModule::update() {
    if (!previewActive_ || !display_) return;

    unsigned long now = millis();
    unsigned long elapsed = now - previewStartMs_;

    if (elapsed >= previewDurationMs_) {
        previewActive_ = false;
        previewEnded_ = true;
        // Clean up display overrides
        display_->clearAlpFrequencyOverride();
        display_->setAlpPreviewState(false, 0, 0);
        display_->setObdPreviewState(false, false, false);
        display_->setBLEProxyStatus(false, false);
        display_->setPreviewIndicatorOverridesActive(false);
        return;
    }

    // Determine which timed frame we should be on.  Short previews and the
    // manual diagnostic run once through the table.  Long qualification holds
    // keep cycling the same visual test instead of parking on the final frame.
    int targetStep = static_cast<int>(elapsed / STEP_DURATION_MS);
    if (!loopSequence_ && targetStep >= STEP_COUNT) targetStep = STEP_COUNT - 1;

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
    if (step.modeChar != NO_CHG) currentModeChar_ = step.modeChar;
    if (step.profileSlot != NO_CHG) currentProfileSlot_ = step.profileSlot;
    if (step.alpState != NO_CHG) {
        currentAlpState_ = step.alpState;
        currentAlpHb_ = step.alpHbByte1;
    }
    if (step.obdState != NO_CHG) currentObdState_ = step.obdState;
    if (step.bleState != NO_CHG) currentBleState_ = step.bleState;
    if (step.mainVolume != NO_CHG) currentMainVol_ = step.mainVolume;
    if (step.muteVolume != NO_CHG) currentMuteVol_ = step.muteVolume;
}

void DisplayPreviewModule::renderStep(int stepIndex, bool firstFrame) {
    if (stepIndex < 0 || stepIndex >= STEP_COUNT) return;
    const PreviewStep& step = STEPS[stepIndex];

    // ── Update carry-forward state ──────────────────────────────────

    applyCarryState(step);

    // ── Build primary alert ─────────────────────────────────────────

    AlertData primary{};
    primary.band = step.band;
    primary.direction = step.dir;
    primary.frontStrength = step.frontBars;
    primary.rearStrength = step.rearBars;
    primary.frequency = step.freqMHz;
    primary.isValid = true;
    primary.isPriority = true;

    if (step.flags & FLAG_PHOTO) {
        primary.photoType = 1;  // Generic photo type
    }

    // ── Build alert array ───────────────────────────────────────────

    AlertData allAlerts[3];
    int alertCount = 1;
    allAlerts[0] = primary;

    if (step.secBand != BAND_NONE) {
        AlertData& sec = allAlerts[alertCount];
        sec = AlertData{};
        sec.band = step.secBand;
        sec.direction = step.secDir;
        sec.frontStrength = step.secFrontBars;
        sec.rearStrength = step.secRearBars;
        sec.frequency = step.secFreqMHz;
        sec.isValid = true;
        alertCount++;
    }

    if (step.thirdBand != BAND_NONE) {
        AlertData& third = allAlerts[alertCount];
        third = AlertData{};
        third.band = step.thirdBand;
        third.direction = step.thirdDir;
        third.frontStrength = step.thirdFrontBars;
        third.rearStrength = step.thirdRearBars;
        third.frequency = step.thirdFreqMHz;
        third.isValid = true;
        alertCount++;
    }

    // ── Build display state ─────────────────────────────────────────

    DisplayState state{};
    state.activeBands = static_cast<Band>(step.band);
    state.arrows = step.dir;
    state.priorityArrow = step.dir;
    state.signalBars = (step.frontBars > 0) ? step.frontBars : step.rearBars;
    state.muted = (step.flags & FLAG_MUTED) != 0;
    state.displayOn = true;
    state.hasDisplayOn = true;
    state.hasVolumeData = true;
    state.mainVolume = static_cast<uint8_t>(currentMainVol_);
    state.muteVolume = static_cast<uint8_t>(currentMuteVol_);

    // Mode
    if (currentModeChar_ > 0) {
        state.modeChar = static_cast<char>(currentModeChar_);
        state.hasMode = true;
    }

    // Flash bits
    if (step.flags & FLAG_FLASH_ARROW) {
        // Set flash on the direction bits (same encoding as image1)
        if (step.dir & DIR_FRONT) state.flashBits |= 0x20;
        if (step.dir & DIR_SIDE)  state.flashBits |= 0x40;
        if (step.dir & DIR_REAR)  state.flashBits |= 0x80;
    }
    if (step.flags & FLAG_FLASH_BAND) {
        state.bandFlashBits = step.band;
    }

    // Bogey counter
    if (step.bogeyChar == AUTO_BC) {
        state.bogeyCounterChar = '0' + static_cast<char>(alertCount);
    } else {
        state.bogeyCounterChar = static_cast<char>(step.bogeyChar);
    }

    // Photo flag
    if (step.flags & FLAG_PHOTO) {
        state.hasPhotoAlert = true;
    }

    // Add secondary bands to activeBands
    if (step.secBand != BAND_NONE) {
        state.activeBands = static_cast<Band>(state.activeBands | step.secBand);
    }
    if (step.thirdBand != BAND_NONE) {
        state.activeBands = static_cast<Band>(state.activeBands | step.thirdBand);
    }

    // ── Set indicator overrides ─────────────────────────────────────

    // ALP badge
    bool alpOn = (currentAlpState_ != static_cast<int8_t>(AlpState::OFF));
    display_->setAlpPreviewState(alpOn,
                                 static_cast<uint8_t>(currentAlpState_),
                                 static_cast<uint8_t>(currentAlpHb_));

    // ALP gun frequency override
    if (step.alpGunAbbrev) {
        display_->setAlpFrequencyOverride(step.alpGunAbbrev);
    } else {
        display_->clearAlpFrequencyOverride();
    }

    // OBD badge
    switch (currentObdState_) {
        case 1: display_->setObdPreviewState(true, true, false);  break;  // Connected
        case 2: display_->setObdPreviewState(true, false, true);  break;  // Scanning
        default: display_->setObdPreviewState(false, false, false); break; // Off
    }

    // BLE proxy indicator
    switch (currentBleState_) {
        case 1: display_->setBLEProxyStatus(true, false);  break;  // Advertising (blue)
        case 2: display_->setBLEProxyStatus(true, true);   break;  // Client connected (green)
        default: display_->setBLEProxyStatus(false, false); break;  // Off
    }

    // Profile
    display_->setProfileIndicatorSlot(currentProfileSlot_);

    // ── Render ──────────────────────────────────────────────────────

    perfSetDisplayRenderScenario(
        firstFrame
            ? PerfDisplayRenderScenario::PreviewFirstFrame
            : PerfDisplayRenderScenario::PreviewSteadyFrame);
    const unsigned long renderStartUs = micros();
    display_->update(primary, allAlerts, alertCount, state);
    perfRecordDisplayScenarioRenderUs(micros() - renderStartUs);
    perfClearDisplayRenderScenario();
}
