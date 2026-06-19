#include <unity.h>

// Speed mute module (header-only types + pure decision function)
#include "../../src/modules/speed_mute/speed_mute_module.h"
#include "../../src/modules/speed_mute/speed_mute_module.cpp"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static SpeedMuteSettings defaultSettings() {
    SpeedMuteSettings s;
    s.enabled = true;
    s.thresholdMph = 25;
    s.hysteresisMph = 3;
    return s;
}

static SpeedMuteContext makeCtx(float speedMph, bool speedValid, uint32_t nowMs) {
    SpeedMuteContext ctx;
    ctx.speedMph = speedMph;
    ctx.speedValid = speedValid;
    ctx.nowMs = nowMs;
    return ctx;
}

// ---------------------------------------------------------------------------
// evaluateSpeedMute — pure function tests
// ---------------------------------------------------------------------------

void test_disabled_never_mutes() {
    SpeedMuteSettings s = defaultSettings();
    s.enabled = false;
    SpeedMuteState state;
    auto ctx = makeCtx(0.0f, true, 1000);
    auto d = evaluateSpeedMute(s, ctx, state);
    TEST_ASSERT_FALSE(d.shouldMute);
}

void test_invalid_speed_never_mutes() {
    SpeedMuteSettings s = defaultSettings();
    SpeedMuteState state;
    auto ctx = makeCtx(0.0f, false, 1000);
    auto d = evaluateSpeedMute(s, ctx, state);
    TEST_ASSERT_FALSE(d.shouldMute);
}

void test_below_threshold_mutes() {
    SpeedMuteSettings s = defaultSettings();
    SpeedMuteState state;
    // First call at t=0 to initialize lastTransitionMs
    auto ctx = makeCtx(10.0f, true, 0);
    evaluateSpeedMute(s, ctx, state);
    // Advance past debounce (1500ms)
    ctx = makeCtx(10.0f, true, 2000);
    auto d = evaluateSpeedMute(s, ctx, state);
    TEST_ASSERT_TRUE(d.shouldMute);
}

void test_above_threshold_does_not_mute() {
    SpeedMuteSettings s = defaultSettings();
    SpeedMuteState state;
    auto ctx = makeCtx(30.0f, true, 2000);
    auto d = evaluateSpeedMute(s, ctx, state);
    TEST_ASSERT_FALSE(d.shouldMute);
}

void test_hysteresis_prevents_cycling() {
    SpeedMuteSettings s = defaultSettings();
    s.thresholdMph = 25;
    s.hysteresisMph = 3;
    SpeedMuteState state;

    // Drop below threshold → mute
    auto ctx = makeCtx(20.0f, true, 0);
    evaluateSpeedMute(s, ctx, state);
    ctx = makeCtx(20.0f, true, 2000);
    evaluateSpeedMute(s, ctx, state);
    TEST_ASSERT_TRUE(state.muteActive);

    // Rise to 26 mph → still below unmute threshold (25+3=28)
    ctx = makeCtx(26.0f, true, 4000);
    evaluateSpeedMute(s, ctx, state);
    TEST_ASSERT_TRUE(state.muteActive);

    // Rise to 29 mph → above unmute threshold
    ctx = makeCtx(29.0f, true, 6000);
    evaluateSpeedMute(s, ctx, state);
    TEST_ASSERT_FALSE(state.muteActive);
}

void test_debounce_blocks_rapid_transition() {
    SpeedMuteSettings s = defaultSettings();
    SpeedMuteState state;

    // Mute at t=0
    auto ctx = makeCtx(10.0f, true, 0);
    evaluateSpeedMute(s, ctx, state);
    ctx = makeCtx(10.0f, true, 1600);
    evaluateSpeedMute(s, ctx, state);
    TEST_ASSERT_TRUE(state.muteActive);

    // Try to unmute quickly at t=1700 (only 100ms after last transition)
    ctx = makeCtx(40.0f, true, 1700);
    evaluateSpeedMute(s, ctx, state);
    TEST_ASSERT_TRUE(state.muteActive);  // debounce blocks

    // Unmute after debounce passes at t=3200
    ctx = makeCtx(40.0f, true, 3200);
    evaluateSpeedMute(s, ctx, state);
    TEST_ASSERT_FALSE(state.muteActive);
}

void test_speed_lost_while_muting_unmutes() {
    SpeedMuteSettings s = defaultSettings();
    SpeedMuteState state;

    // Mute
    auto ctx = makeCtx(10.0f, true, 0);
    evaluateSpeedMute(s, ctx, state);
    ctx = makeCtx(10.0f, true, 2000);
    evaluateSpeedMute(s, ctx, state);
    TEST_ASSERT_TRUE(state.muteActive);

    // Speed source lost → fail-open → unmute
    ctx = makeCtx(0.0f, false, 4000);
    auto d = evaluateSpeedMute(s, ctx, state);
    TEST_ASSERT_FALSE(d.shouldMute);
    TEST_ASSERT_FALSE(state.muteActive);
}

void test_disabled_clears_active_mute() {
    SpeedMuteSettings s = defaultSettings();
    SpeedMuteState state;

    // Mute
    auto ctx = makeCtx(10.0f, true, 0);
    evaluateSpeedMute(s, ctx, state);
    ctx = makeCtx(10.0f, true, 2000);
    evaluateSpeedMute(s, ctx, state);
    TEST_ASSERT_TRUE(state.muteActive);

    // Disable → immediately unmute
    s.enabled = false;
    ctx = makeCtx(10.0f, true, 3000);
    auto d = evaluateSpeedMute(s, ctx, state);
    TEST_ASSERT_FALSE(d.shouldMute);
    TEST_ASSERT_FALSE(state.muteActive);
}

// ---------------------------------------------------------------------------
// SpeedMuteModule wrapper tests
// ---------------------------------------------------------------------------

void test_module_begin_and_update() {
    SpeedMuteModule mod;
    mod.begin(true, 25, 3);
    auto d = mod.update(10.0f, true, 0);
    // First call may not mute due to debounce from initial state
    d = mod.update(10.0f, true, 2000);
    TEST_ASSERT_TRUE(d.shouldMute);
}

void test_module_sync_settings() {
    SpeedMuteModule mod;
    mod.begin(false, 25, 3);  // disabled

    auto d = mod.update(10.0f, true, 2000);
    TEST_ASSERT_FALSE(d.shouldMute);  // disabled

    mod.syncSettings(true, 25, 3);  // enable
    d = mod.update(10.0f, true, 4000);
    TEST_ASSERT_TRUE(d.shouldMute);  // now should mute
}

void test_module_band_override_always_on_for_laser_and_ka() {
    SpeedMuteModule mod;
    mod.begin(true, 25, 3);

    // Laser and Ka are always overridden (never muted).
    TEST_ASSERT_TRUE(mod.isBandOverridden(1));   // LASER
    TEST_ASSERT_TRUE(mod.isBandOverridden(2));   // KA
    // K and X are not overridden.
    TEST_ASSERT_FALSE(mod.isBandOverridden(4));  // K
    TEST_ASSERT_FALSE(mod.isBandOverridden(8));  // X
}

void test_exact_threshold_boundary() {
    SpeedMuteSettings s = defaultSettings();
    s.thresholdMph = 25;
    SpeedMuteState state;

    // Exactly at threshold → should NOT mute (mute is < threshold, not <=)
    auto ctx = makeCtx(25.0f, true, 0);
    evaluateSpeedMute(s, ctx, state);
    ctx = makeCtx(25.0f, true, 2000);
    auto d = evaluateSpeedMute(s, ctx, state);
    TEST_ASSERT_FALSE(d.shouldMute);
}

void test_exact_unmute_boundary() {
    SpeedMuteSettings s = defaultSettings();
    s.thresholdMph = 25;
    s.hysteresisMph = 3;
    SpeedMuteState state;

    // Mute at low speed
    auto ctx = makeCtx(10.0f, true, 0);
    evaluateSpeedMute(s, ctx, state);
    ctx = makeCtx(10.0f, true, 2000);
    evaluateSpeedMute(s, ctx, state);
    TEST_ASSERT_TRUE(state.muteActive);

    // Exactly at unmute threshold (28) → unmute (>= unmuteAbove)
    ctx = makeCtx(28.0f, true, 4000);
    evaluateSpeedMute(s, ctx, state);
    TEST_ASSERT_FALSE(state.muteActive);
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();

    // Pure function tests
    RUN_TEST(test_disabled_never_mutes);
    RUN_TEST(test_invalid_speed_never_mutes);
    RUN_TEST(test_below_threshold_mutes);
    RUN_TEST(test_above_threshold_does_not_mute);
    RUN_TEST(test_hysteresis_prevents_cycling);
    RUN_TEST(test_debounce_blocks_rapid_transition);
    RUN_TEST(test_speed_lost_while_muting_unmutes);
    RUN_TEST(test_disabled_clears_active_mute);
    RUN_TEST(test_exact_threshold_boundary);
    RUN_TEST(test_exact_unmute_boundary);

    // Module wrapper tests
    RUN_TEST(test_module_begin_and_update);
    RUN_TEST(test_module_sync_settings);
    RUN_TEST(test_module_band_override_always_on_for_laser_and_ka);

    return UNITY_END();
}
