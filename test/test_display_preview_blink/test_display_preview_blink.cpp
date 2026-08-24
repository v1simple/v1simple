/**
 * Regression boundary: preview mode owns its own blink refresh.
 *
 * The display preview renders one frame per two-second step. Blink animation is
 * driven from a lightweight refresh that the orchestrator explicitly suppresses
 * while preview is running, so every declared flash step was drawn once, in
 * whatever blink phase happened to be current, and then held. Camera evidence of
 * the physical panel confirmed it: zero interior toggles across every observed
 * occurrence of the declared flash steps.
 *
 * These tests pin the repair and its boundaries: preview asks V1Display whether
 * the *shared* 96 ms phase is due and re-renders the already-resolved step, and
 * it does nothing else -- no second clock, no phase reset, no scenario advance,
 * no catch-up burst, and no work at all on a step that does not flash.
 */
#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/display.h"
#include "../mocks/packet_parser.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif


#ifndef V1_LINKED_TEST_DISPLAY_PREVIEW_BLINK
#define V1_LINKED_TEST_DISPLAY_PREVIEW_BLINK
#define V1_INLINE_TEST_DISPLAY_PREVIEW_BLINK
#endif
#include "../../src/modules/display/display_preview_module.h"
#ifdef V1_INLINE_TEST_DISPLAY_PREVIEW_BLINK
#include "../../src/modules/display/display_preview_module.cpp"
#endif

// ---------------------------------------------------------------- helpers

namespace {

constexpr unsigned long kStepMs = 2000;
constexpr unsigned long kBlinkMs = 96;
// The declared flash steps, from the preview step table.
constexpr int kArrowFlashStep = 14;   // FLAG_FLASH_ARROW, rear
constexpr int kCombinedFlashStep = 42; // FLAG_FLASH_ARROW | FLAG_FLASH_BAND, Ka

struct Harness {
    V1Display display;
    DisplayPreviewModule preview;

    // `requestHold(0)` runs the table once; a duration longer than the whole
    // table makes the caller-owned preview loop.
    void begin(unsigned long startMs, uint32_t durationMs) {
        mockMillis = startMs;
        display.reset();
        preview.begin(&display);
        preview.requestHold(durationMs);
    }

    // One loop iteration at an absolute time.
    void loopAt(unsigned long nowMs) {
        mockMillis = nowMs;
        preview.update();
    }
};

// Advance to the interior of `stepIndex` and return the absolute time of that
// step's start.
unsigned long enterStep(Harness& h, int stepIndex) {
    const unsigned long start = static_cast<unsigned long>(stepIndex) * kStepMs;
    h.loopAt(start + 1);
    return start;
}

} // namespace

// ---------------------------------------------------------------- tests

void setUp() {}
void tearDown() {}

/** Step 14 must not refresh before the shared phase is due, and must after. */
void test_arrow_flash_step_refreshes_only_when_the_shared_phase_is_due() {
    Harness h;
    h.begin(0, 120000);
    const unsigned long stepStart = enterStep(h, kArrowFlashStep);
    const int afterStepRender = h.display.updateCalls;
    TEST_ASSERT_GREATER_THAN_INT(0, afterStepRender);

    // 95 ms after the renderer's last phase transition: not due.
    h.display.lastBlinkToggleMs = stepStart + 1;
    h.loopAt(stepStart + 1 + (kBlinkMs - 1));
    TEST_ASSERT_EQUAL_INT_MESSAGE(afterStepRender, h.display.updateCalls,
                                  "preview refreshed before the shared blink phase was due");

    // At the interval: due, exactly one extra render.
    h.loopAt(stepStart + 1 + kBlinkMs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(afterStepRender + 1, h.display.updateCalls,
                                  "preview did not refresh when the shared phase came due");
}

/** The refresh must redraw the same declared masks, not a cleared state. */
void test_combined_flash_step_retains_arrow_and_band_masks_on_refresh() {
    Harness h;
    h.begin(0, 200000);
    const unsigned long stepStart = enterStep(h, kCombinedFlashStep);
    const uint8_t stepFlash = h.display.lastAlertDisplayState.flashBits;
    const uint8_t stepBandFlash = h.display.lastAlertDisplayState.bandFlashBits;
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x20, stepFlash, "step 42 must flash the front arrow");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x02, stepBandFlash, "step 42 must flash the Ka band cell");

    h.display.lastBlinkToggleMs = stepStart + 1;
    h.loopAt(stepStart + 1 + kBlinkMs);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(stepFlash, h.display.lastAlertDisplayState.flashBits,
                                    "the blink refresh dropped the arrow flash mask");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(stepBandFlash, h.display.lastAlertDisplayState.bandFlashBits,
                                    "the blink refresh dropped the band flash mask");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x02, h.display.lastAlertDisplayState.activeBands,
                                    "the blink refresh changed which band is active");
}

/** Arrow and band are one phase, so one refresh carries both. */
void test_arrow_and_band_observe_the_same_phase() {
    Harness h;
    h.begin(0, 200000);
    const unsigned long stepStart = enterStep(h, kCombinedFlashStep);
    const int before = h.display.updateCalls;
    h.display.lastBlinkToggleMs = stepStart + 1;
    h.loopAt(stepStart + 1 + kBlinkMs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(before + 1, h.display.updateCalls,
                                  "arrow and band must refresh together, in one render");
    TEST_ASSERT_TRUE_MESSAGE(h.display.lastAlertDisplayState.flashBits != 0 &&
                                 h.display.lastAlertDisplayState.bandFlashBits != 0,
                             "one refresh must carry both flash sources");
}

/** A step that does not flash keeps exactly one render per two-second step. */
void test_non_flashing_step_does_not_refresh() {
    Harness h;
    h.begin(0, 120000);
    const unsigned long stepStart = enterStep(h, 3); // no flash flags
    const int afterStepRender = h.display.updateCalls;
    h.display.lastBlinkToggleMs = stepStart + 1;
    for (unsigned long offset = kBlinkMs; offset < kStepMs - 10; offset += kBlinkMs) {
        h.loopAt(stepStart + 1 + offset);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(afterStepRender, h.display.updateCalls,
                                  "a non-flashing preview step refreshed anyway");
}

/** A due tick and a step boundary in one loop produce one render, not two. */
void test_step_boundary_wins_over_a_due_tick() {
    Harness h;
    h.begin(0, 120000);
    const unsigned long stepStart = enterStep(h, kArrowFlashStep);
    const int before = h.display.updateCalls;
    // The phase is overdue *and* the next step is entered in the same loop.
    h.display.lastBlinkToggleMs = stepStart;
    h.loopAt(stepStart + kStepMs + 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(before + 1, h.display.updateCalls,
                                  "a step transition and a due tick rendered twice in one loop");
}

/** A refresh must not move the scenario on. */
void test_a_refresh_does_not_advance_the_scenario() {
    Harness h;
    h.begin(0, 200000);
    const unsigned long stepStart = enterStep(h, kArrowFlashStep);
    const uint8_t bandsAtStep = h.display.lastAlertDisplayState.activeBands;
    const uint8_t barsAtStep = h.display.lastAlertDisplayState.signalBars;
    h.display.lastBlinkToggleMs = stepStart + 1;
    for (int tick = 1; tick <= 5; ++tick) {
        h.loopAt(stepStart + 1 + static_cast<unsigned long>(tick) * kBlinkMs);
        h.display.lastBlinkToggleMs = stepStart + 1 + static_cast<unsigned long>(tick) * kBlinkMs;
    }
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(bandsAtStep, h.display.lastAlertDisplayState.activeBands,
                                    "a blink refresh advanced the scenario state");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(barsAtStep, h.display.lastAlertDisplayState.signalBars,
                                    "a blink refresh advanced the scenario state");
}

/** A late loop must refresh once, never a burst of missed ticks. */
void test_a_delayed_loop_renders_once_not_a_burst() {
    Harness h;
    h.begin(0, 120000);
    const unsigned long stepStart = enterStep(h, kArrowFlashStep);
    const int before = h.display.updateCalls;
    // Ten intervals of arrears arrive in a single loop.
    h.display.lastBlinkToggleMs = stepStart + 1;
    h.loopAt(stepStart + 1 + kBlinkMs * 10);
    TEST_ASSERT_EQUAL_INT_MESSAGE(before + 1, h.display.updateCalls,
                                  "a late loop issued a catch-up burst of renders");
}

/** Cancel, expiry and inactivity must silence the refresh completely. */
void test_cancelled_or_expired_preview_never_refreshes() {
    Harness h;
    h.begin(0, 120000);
    const unsigned long stepStart = enterStep(h, kArrowFlashStep);
    h.preview.cancel();
    const int afterStop = h.display.updateCalls;
    h.display.lastBlinkToggleMs = stepStart + 1;
    h.loopAt(stepStart + 1 + kBlinkMs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(afterStop, h.display.updateCalls,
                                  "a cancelled preview kept refreshing");

    Harness expired;
    expired.begin(0, static_cast<uint32_t>(kStepMs) * (kArrowFlashStep + 1));
    enterStep(expired, kArrowFlashStep);
    expired.loopAt(kStepMs * (kArrowFlashStep + 1) + 1); // past the declared duration
    const int afterExpiry = expired.display.updateCalls;
    expired.display.lastBlinkToggleMs = kStepMs * (kArrowFlashStep + 1) + 1;
    expired.loopAt(kStepMs * (kArrowFlashStep + 1) + 1 + kBlinkMs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(afterExpiry, expired.display.updateCalls,
                                  "an expired preview kept refreshing");
    TEST_ASSERT_FALSE_MESSAGE(expired.preview.isRunning(),
                              "an expired preview still reports itself running");
}

/** Preview must not toggle or reset the renderer's shared phase itself. */
void test_preview_does_not_own_the_blink_phase() {
    Harness h;
    h.begin(0, 120000);
    const unsigned long stepStart = enterStep(h, kArrowFlashStep);
    h.display.lastBlinkToggleMs = 12345;
    h.loopAt(stepStart + 1 + kBlinkMs);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(12345, h.display.lastBlinkToggleMs,
                                     "preview wrote to the renderer's shared blink phase");
}

/** A short caller-owned hold (the colour-save preview) still blinks, and stops. */
void test_a_short_caller_hold_blinks_then_releases() {
    Harness h;
    // A hold shorter than the whole table: the manual colour preview shape.
    const uint32_t holdMs = static_cast<uint32_t>(kStepMs) * (kArrowFlashStep + 1) + 500;
    h.begin(0, holdMs);
    const unsigned long stepStart = enterStep(h, kArrowFlashStep);
    const int afterStep = h.display.updateCalls;
    h.display.lastBlinkToggleMs = stepStart + 1;
    h.loopAt(stepStart + 1 + kBlinkMs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(afterStep + 1, h.display.updateCalls,
                                  "a caller-owned hold did not blink its flash step");
    TEST_ASSERT_TRUE_MESSAGE(h.preview.isRunning(), "the hold ended early");

    h.loopAt(holdMs + 1);
    TEST_ASSERT_FALSE_MESSAGE(h.preview.isRunning(), "the hold did not release on time");
    const int afterRelease = h.display.updateCalls;
    h.display.lastBlinkToggleMs = holdMs + 1;
    h.loopAt(holdMs + 1 + kBlinkMs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(afterRelease, h.display.updateCalls,
                                  "a released hold kept refreshing");
}

/** Suppressed loops simply do not drive preview, so nothing refreshes. */
void test_a_suppressed_loop_produces_no_refresh() {
    Harness h;
    h.begin(0, 120000);
    const unsigned long stepStart = enterStep(h, kArrowFlashStep);
    const int before = h.display.updateCalls;
    h.display.lastBlinkToggleMs = stepStart + 1;
    // Boot splash, overload and power suppression all withhold update() at the
    // orchestrator. Time passing without that call must change nothing.
    mockMillis = stepStart + 1 + kBlinkMs * 4;
    TEST_ASSERT_EQUAL_INT_MESSAGE(before, h.display.updateCalls,
                                  "preview refreshed without being driven");
}

/** A refresh touches the panel and nothing else. */
void test_a_refresh_has_no_other_side_effect() {
    Harness h;
    h.begin(0, 120000);
    const unsigned long stepStart = enterStep(h, kArrowFlashStep);
    const int clears = h.display.clearCalls;
    const int flushes = h.display.flushCalls;
    const int forced = h.display.forceNextRedrawCalls;
    const int maintenance = h.display.showMaintenanceModeCalls;
    const int overrides = h.display.setPreviewIndicatorOverridesActiveCalls;
    h.display.lastBlinkToggleMs = stepStart + 1;
    h.loopAt(stepStart + 1 + kBlinkMs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(clears, h.display.clearCalls,
                                  "a blink refresh cleared the panel");
    TEST_ASSERT_EQUAL_INT_MESSAGE(flushes, h.display.flushCalls,
                                  "a blink refresh issued its own flush");
    TEST_ASSERT_EQUAL_INT_MESSAGE(forced, h.display.forceNextRedrawCalls,
                                  "a blink refresh forced a full redraw");
    TEST_ASSERT_EQUAL_INT_MESSAGE(maintenance, h.display.showMaintenanceModeCalls,
                                  "a blink refresh disturbed maintenance presentation");
    TEST_ASSERT_EQUAL_INT_MESSAGE(overrides, h.display.setPreviewIndicatorOverridesActiveCalls,
                                  "a blink refresh re-armed preview ownership");
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_arrow_flash_step_refreshes_only_when_the_shared_phase_is_due);
    RUN_TEST(test_combined_flash_step_retains_arrow_and_band_masks_on_refresh);
    RUN_TEST(test_arrow_and_band_observe_the_same_phase);
    RUN_TEST(test_non_flashing_step_does_not_refresh);
    RUN_TEST(test_step_boundary_wins_over_a_due_tick);
    RUN_TEST(test_a_refresh_does_not_advance_the_scenario);
    RUN_TEST(test_a_delayed_loop_renders_once_not_a_burst);
    RUN_TEST(test_cancelled_or_expired_preview_never_refreshes);
    RUN_TEST(test_preview_does_not_own_the_blink_phase);
    RUN_TEST(test_a_short_caller_hold_blinks_then_releases);
    RUN_TEST(test_a_suppressed_loop_produces_no_refresh);
    RUN_TEST(test_a_refresh_has_no_other_side_effect);
    return UNITY_END();
}
