#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/display.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/display/display_preview_module.cpp"

PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;

namespace {
V1Display testDisplay;
DisplayPreviewModule preview;
PerfDisplayRenderScenario lastScenario = PerfDisplayRenderScenario::None;
int scenarioSetCalls = 0;
int scenarioClearCalls = 0;
int scenarioRenderCalls = 0;
uint32_t lastScenarioRenderUs = 0;
} // namespace

void perfSetDisplayRenderScenario(PerfDisplayRenderScenario scenario) {
    lastScenario = scenario;
    ++scenarioSetCalls;
}

PerfDisplayRenderScenario perfGetDisplayRenderScenario() {
    return lastScenario;
}

void perfClearDisplayRenderScenario() {
    lastScenario = PerfDisplayRenderScenario::None;
    ++scenarioClearCalls;
}

void perfRecordDisplayScenarioRenderUs(uint32_t us) {
    lastScenarioRenderUs = us;
    ++scenarioRenderCalls;
}

void setUp() {
    testDisplay.reset();
    preview = DisplayPreviewModule();
    preview.begin(&testDisplay);
    mockMillis = 10000;
    mockMicros = 1000000;
    lastScenario = PerfDisplayRenderScenario::None;
    scenarioSetCalls = 0;
    scenarioClearCalls = 0;
    scenarioRenderCalls = 0;
    lastScenarioRenderUs = 0;
}

void tearDown() {}

void test_preview_renders_first_step_on_initial_update() {
    preview.requestHold(5500);

    preview.update();

    TEST_ASSERT_EQUAL_INT(1, testDisplay.updateCalls);
    TEST_ASSERT_TRUE(testDisplay.hasLastPriorityAlert);
    TEST_ASSERT_EQUAL_UINT32(10525u, testDisplay.lastPriorityAlert.frequency);
    TEST_ASSERT_EQUAL_INT(BAND_X, testDisplay.lastPriorityAlert.band);
    TEST_ASSERT_EQUAL_INT(DIR_FRONT, testDisplay.lastPriorityAlert.direction);
    TEST_ASSERT_EQUAL_INT(1, scenarioSetCalls);
    TEST_ASSERT_EQUAL(PerfDisplayRenderScenario::None, lastScenario);
    TEST_ASSERT_EQUAL_INT(1, scenarioClearCalls);
    TEST_ASSERT_EQUAL_INT(1, scenarioRenderCalls);
    TEST_ASSERT_EQUAL_INT(1, testDisplay.setPreviewIndicatorOverridesActiveCalls);
    TEST_ASSERT_TRUE(testDisplay.lastPreviewIndicatorOverridesActive);
}

void test_preview_skips_missed_steps_without_catchup_burst() {
    preview.requestHold(0);

    // Simulate preview work being skipped for long enough that the timed table
    // has advanced from step 0 to step 3.  The preview module must drop obsolete
    // visual frames and perform only one display update in this loop.
    mockMillis += 6000;
    mockMicros += 50;

    preview.update();

    TEST_ASSERT_EQUAL_INT(1, testDisplay.updateCalls);
    TEST_ASSERT_TRUE(testDisplay.hasLastPriorityAlert);
    TEST_ASSERT_EQUAL_UINT32(24150u, testDisplay.lastPriorityAlert.frequency);
    TEST_ASSERT_EQUAL_INT(BAND_K, testDisplay.lastPriorityAlert.band);
    TEST_ASSERT_EQUAL_INT(DIR_FRONT, testDisplay.lastPriorityAlert.direction);
    TEST_ASSERT_EQUAL_INT(1, scenarioSetCalls);
    TEST_ASSERT_EQUAL_INT(1, scenarioClearCalls);
    TEST_ASSERT_EQUAL_INT(1, scenarioRenderCalls);
    TEST_ASSERT_TRUE_MESSAGE(preview.isRunning(),
                             "zero-duration request should use full diagnostic auto-duration");

    preview.update();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, testDisplay.updateCalls,
                                  "same timed step must not render again");
}

void test_preview_honors_short_requested_duration() {
    preview.requestHold(5500);

    preview.update();
    mockMillis += 2000;
    preview.update();
    mockMillis += 2000;
    preview.update();

    TEST_ASSERT_TRUE(preview.isRunning());
    TEST_ASSERT_EQUAL_INT(3, testDisplay.updateCalls);

    const int updatesBeforeExpiry = testDisplay.updateCalls;
    const int clearsBeforeExpiry = testDisplay.clearAlpFrequencyOverrideCalls;

    mockMillis += 1500;
    preview.update();

    TEST_ASSERT_FALSE(preview.isRunning());
    TEST_ASSERT_TRUE(preview.ownsPresentation());
    TEST_ASSERT_TRUE(preview.consumeEnded());
    TEST_ASSERT_FALSE(preview.ownsPresentation());
    TEST_ASSERT_EQUAL_INT_MESSAGE(updatesBeforeExpiry, testDisplay.updateCalls,
                                  "expired short preview must not render another frame");
    TEST_ASSERT_EQUAL_INT(clearsBeforeExpiry + 1, testDisplay.clearAlpFrequencyOverrideCalls);
    TEST_ASSERT_FALSE(testDisplay.lastAlpPreviewEnabled);
    TEST_ASSERT_FALSE(testDisplay.lastObdPreviewEnabled);
    TEST_ASSERT_FALSE(testDisplay.lastBleProxyEnabled);
    TEST_ASSERT_EQUAL_INT(2, testDisplay.setPreviewIndicatorOverridesActiveCalls);
    TEST_ASSERT_FALSE(testDisplay.lastPreviewIndicatorOverridesActive);
}

void test_cancel_keeps_presentation_owned_until_restore_consumes_end() {
    preview.requestHold(5500);
    TEST_ASSERT_TRUE(preview.ownsPresentation());

    preview.cancel();

    TEST_ASSERT_FALSE(preview.isRunning());
    TEST_ASSERT_TRUE(preview.ownsPresentation());
    TEST_ASSERT_TRUE(preview.consumeEnded());
    TEST_ASSERT_FALSE(preview.ownsPresentation());
}

void test_preview_fast_forward_preserves_carry_state_for_rendered_step() {
    preview.requestHold(0);

    // Step 36 changes mode to Logic ('l') while keeping profile slot inherited
    // from the earlier carry state.  Jumping directly to it should still apply
    // skipped carry-state changes without rendering each skipped frame.
    mockMillis += 36UL * 2000UL;

    preview.update();

    TEST_ASSERT_EQUAL_INT(1, testDisplay.updateCalls);
    TEST_ASSERT_TRUE(testDisplay.hasLastAlertDisplayState);
    TEST_ASSERT_TRUE(testDisplay.lastAlertDisplayState.hasMode);
    TEST_ASSERT_EQUAL_CHAR('l', testDisplay.lastAlertDisplayState.modeChar);
    TEST_ASSERT_EQUAL_INT(1, testDisplay.setProfileIndicatorSlotCalls);
    TEST_ASSERT_EQUAL_INT(0, testDisplay.lastProfileIndicatorSlotValue);
}

void test_long_requested_preview_loops_visual_sequence() {
    preview.requestHold(90000);

    mockMillis += 89000;
    preview.update();

    TEST_ASSERT_TRUE(preview.isRunning());
    TEST_ASSERT_EQUAL_INT(1, testDisplay.updateCalls);
    TEST_ASSERT_TRUE(testDisplay.hasLastPriorityAlert);
    TEST_ASSERT_EQUAL_UINT32(10525u, testDisplay.lastPriorityAlert.frequency);
    TEST_ASSERT_EQUAL_INT(BAND_X, testDisplay.lastPriorityAlert.band);
    TEST_ASSERT_EQUAL_INT(DIR_FRONT, testDisplay.lastPriorityAlert.direction);
}

void test_resolve_step_uses_shared_main_meter_scaling_and_carry_state() {
    DisplayPreviewModule::ResolvedStep resolved;

    TEST_ASSERT_TRUE(DisplayPreviewModule::resolveStep(14, resolved));

    TEST_ASSERT_EQUAL_INT(14, resolved.index);
    TEST_ASSERT_EQUAL_INT(BAND_KA, resolved.primary.band);
    TEST_ASSERT_EQUAL_UINT32(35500u, resolved.primary.freqMHz);
    TEST_ASSERT_EQUAL_STRING("35.500", resolved.primary.frequencyText);
    TEST_ASSERT_EQUAL_UINT8(8, resolved.mainMeterCount);
    TEST_ASSERT_EQUAL_UINT8(0x80, resolved.flashMask);
    TEST_ASSERT_EQUAL_CHAR('A', resolved.status.modeChar);
    TEST_ASSERT_TRUE(resolved.status.hasMode);
    TEST_ASSERT_EQUAL_INT(0, resolved.status.profileSlot);
}

void test_preview_steps_cover_every_protocol_bogey_glyph() {
    // packet_parser.cpp can emit precisely this non-blank domain.  Keep the
    // physical visual sequence exhaustive so the host L2 decoder never gains
    // an unexercised protocol symbol.
    constexpr char kExpectedGlyphs[] = "0123456789&uJLCU#cdFPAEb";
    bool seen[sizeof(kExpectedGlyphs) - 1] = {};

    for (int stepIndex = 0; stepIndex < DisplayPreviewModule::stepCount(); ++stepIndex) {
        DisplayPreviewModule::ResolvedStep resolved;
        TEST_ASSERT_TRUE(DisplayPreviewModule::resolveStep(stepIndex, resolved));

        bool protocolGlyph = false;
        for (size_t glyphIndex = 0; glyphIndex < sizeof(kExpectedGlyphs) - 1; ++glyphIndex) {
            if (resolved.status.bogeyChar == kExpectedGlyphs[glyphIndex]) {
                seen[glyphIndex] = true;
                protocolGlyph = true;
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(protocolGlyph,
                                 "preview table contains an unsupported bogey glyph");
    }

    for (size_t glyphIndex = 0; glyphIndex < sizeof(kExpectedGlyphs) - 1; ++glyphIndex) {
        TEST_ASSERT_TRUE_MESSAGE(seen[glyphIndex],
                                 "preview table must cover every protocol bogey glyph");
    }
}

void test_multi_x_preview_steps_keep_two_distinct_card_identities() {
    // Card slot continuity deliberately merges same-band alerts within 5 MHz.
    // These authored visual-verification frames must therefore keep all three
    // X sources farther apart so the primary is filtered and both cards remain.
    constexpr uint32_t kCardContinuityWindowMHz = 5;
    const int multiXSteps[] = {24, 41};

    for (int stepIndex : multiXSteps) {
        DisplayPreviewModule::ResolvedStep resolved;

        TEST_ASSERT_TRUE(DisplayPreviewModule::resolveStep(stepIndex, resolved));
        TEST_ASSERT_EQUAL_UINT8(3, resolved.alertCount);
        TEST_ASSERT_TRUE(resolved.secondary.present);
        TEST_ASSERT_TRUE(resolved.third.present);
        TEST_ASSERT_EQUAL_INT(BAND_X, resolved.primary.band);
        TEST_ASSERT_EQUAL_INT(BAND_X, resolved.secondary.band);
        TEST_ASSERT_EQUAL_INT(BAND_X, resolved.third.band);
        TEST_ASSERT_EQUAL_UINT32(10525u, resolved.primary.freqMHz);
        TEST_ASSERT_EQUAL_UINT32(10515u, resolved.secondary.freqMHz);
        TEST_ASSERT_EQUAL_UINT32(10535u, resolved.third.freqMHz);
        TEST_ASSERT_EQUAL_STRING("10.515", resolved.secondary.frequencyText);
        TEST_ASSERT_EQUAL_STRING("10.535", resolved.third.frequencyText);

        const uint32_t primarySecondaryDiff =
            resolved.primary.freqMHz - resolved.secondary.freqMHz;
        const uint32_t primaryThirdDiff =
            resolved.third.freqMHz - resolved.primary.freqMHz;
        const uint32_t secondaryThirdDiff =
            resolved.third.freqMHz - resolved.secondary.freqMHz;
        TEST_ASSERT_GREATER_THAN_UINT32(kCardContinuityWindowMHz, primarySecondaryDiff);
        TEST_ASSERT_GREATER_THAN_UINT32(kCardContinuityWindowMHz, primaryThirdDiff);
        TEST_ASSERT_GREATER_THAN_UINT32(kCardContinuityWindowMHz, secondaryThirdDiff);
    }
}

void test_pin_step_clear_renders_synchronously_and_records_sequence() {
    uint32_t renderSeq = 0;

    TEST_ASSERT_TRUE(preview.pinStep(10, true, &renderSeq));

    TEST_ASSERT_TRUE(preview.isRunning());
    TEST_ASSERT_TRUE(preview.isVisualPinned());
    TEST_ASSERT_EQUAL_INT(10, preview.pinnedStep());
    TEST_ASSERT_EQUAL_UINT32(renderSeq, preview.pinnedRenderSeq());
    TEST_ASSERT_EQUAL_UINT32(testDisplay.renderSequenceId(), renderSeq);
    TEST_ASSERT_EQUAL_INT(1, testDisplay.clearCalls);
    TEST_ASSERT_EQUAL_INT(1, testDisplay.resetChangeTrackingCalls);
    TEST_ASSERT_EQUAL_INT(1, testDisplay.forceNextRedrawCalls);
    TEST_ASSERT_EQUAL_INT(1, testDisplay.setVisualTestBlinkPhaseCalls);
    TEST_ASSERT_TRUE(testDisplay.lastBlinkPhase);
    TEST_ASSERT_EQUAL_UINT32(mockMillis, testDisplay.lastBlinkToggleMs);
    TEST_ASSERT_EQUAL_INT(1, testDisplay.updateCalls);
    TEST_ASSERT_TRUE(testDisplay.hasLastPriorityAlert);
    TEST_ASSERT_EQUAL_UINT32(34700u, testDisplay.lastPriorityAlert.frequency);
    TEST_ASSERT_EQUAL_UINT8(7, testDisplay.lastAlertDisplayState.signalBars);

    const uint32_t firstSeq = renderSeq;
    TEST_ASSERT_TRUE(preview.pinStep(10, false, &renderSeq));

    TEST_ASSERT_EQUAL_INT(1, testDisplay.clearCalls);
    TEST_ASSERT_EQUAL_INT(2, testDisplay.updateCalls);
    TEST_ASSERT_EQUAL_UINT32(firstSeq + 1, renderSeq);
    TEST_ASSERT_EQUAL_UINT32(34700u, testDisplay.lastPriorityAlert.frequency);
    TEST_ASSERT_EQUAL_UINT8(7, testDisplay.lastAlertDisplayState.signalBars);
}

void test_pin_rejects_invalid_index_without_rendering() {
    uint32_t renderSeq = 123;

    TEST_ASSERT_FALSE(preview.pinStep(DisplayPreviewModule::stepCount(), true, &renderSeq));

    TEST_ASSERT_FALSE(preview.isVisualPinned());
    TEST_ASSERT_EQUAL_UINT32(123, renderSeq);
    TEST_ASSERT_EQUAL_INT(0, testDisplay.updateCalls);
    TEST_ASSERT_EQUAL_INT(0, testDisplay.clearCalls);
    TEST_ASSERT_EQUAL_INT(0, testDisplay.enableVisualFlushShadowCalls);
}

void test_pin_enables_flush_shadow_and_clear_releases_it() {
    uint32_t renderSeq = 0;

    TEST_ASSERT_TRUE(preview.pinStep(3, true, &renderSeq));
    TEST_ASSERT_EQUAL_INT(1, testDisplay.enableVisualFlushShadowCalls);
    TEST_ASSERT_TRUE(testDisplay.flushShadowAvailable());

    // Every pin re-requests the shadow; enabling is idempotent, so the
    // shadow persists across the pins of one visual-test run.
    TEST_ASSERT_TRUE(preview.pinStep(4, false, &renderSeq));
    TEST_ASSERT_EQUAL_INT(2, testDisplay.enableVisualFlushShadowCalls);
    TEST_ASSERT_TRUE(testDisplay.flushShadowAvailable());
    TEST_ASSERT_EQUAL_INT(0, testDisplay.disableVisualFlushShadowCalls);

    preview.clearVisualPin();
    TEST_ASSERT_FALSE(preview.isVisualPinned());
    TEST_ASSERT_EQUAL_INT(1, testDisplay.disableVisualFlushShadowCalls);
    TEST_ASSERT_FALSE(testDisplay.flushShadowAvailable());
}

void test_pin_survives_flush_shadow_allocation_failure() {
    uint32_t renderSeq = 0;
    testDisplay.flushShadowAllocFails = true;

    TEST_ASSERT_TRUE(preview.pinStep(3, true, &renderSeq));

    // Pinning is best-effort on the shadow: the framebuffer verdict path
    // still works; the flushshadow route reports unavailability instead.
    TEST_ASSERT_TRUE(preview.isVisualPinned());
    TEST_ASSERT_EQUAL_INT(1, testDisplay.enableVisualFlushShadowCalls);
    TEST_ASSERT_FALSE(testDisplay.flushShadowAvailable());
    TEST_ASSERT_EQUAL_INT(1, testDisplay.updateCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_preview_renders_first_step_on_initial_update);
    RUN_TEST(test_preview_skips_missed_steps_without_catchup_burst);
    RUN_TEST(test_preview_honors_short_requested_duration);
    RUN_TEST(test_cancel_keeps_presentation_owned_until_restore_consumes_end);
    RUN_TEST(test_preview_fast_forward_preserves_carry_state_for_rendered_step);
    RUN_TEST(test_long_requested_preview_loops_visual_sequence);
    RUN_TEST(test_resolve_step_uses_shared_main_meter_scaling_and_carry_state);
    RUN_TEST(test_preview_steps_cover_every_protocol_bogey_glyph);
    RUN_TEST(test_multi_x_preview_steps_keep_two_distinct_card_identities);
    RUN_TEST(test_pin_step_clear_renders_synchronously_and_records_sequence);
    RUN_TEST(test_pin_rejects_invalid_index_without_rendering);
    RUN_TEST(test_pin_enables_flush_shadow_and_clear_releases_it);
    RUN_TEST(test_pin_survives_flush_shadow_allocation_failure);
    return UNITY_END();
}
