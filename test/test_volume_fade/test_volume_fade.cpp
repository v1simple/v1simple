#include <unity.h>

#include "../mocks/settings.h"  // Mock settings must be included before module sources
#include "../../src/modules/volume_fade/volume_fade_module.h"
#include "../../src/modules/volume_fade/volume_fade_module.cpp"  // Pull implementation when test_build_src=false

// Native unit test builds this module translation unit directly; provide perf hook stub.
void perfRecordVolumeFadeDecision(PerfFadeDecision, uint8_t, uint8_t, uint32_t) {}

// Extern from mocks
#ifndef ARDUINO
SerialClass Serial;
#endif
SettingsManager settingsManager;

static VolumeFadeModule fade;

void setUp() {
    // Reset settings to defaults for each test
    settingsManager.settings = V1Settings();
    fade = VolumeFadeModule();
    fade.begin(&settingsManager);
}

// Helper to build a common context
static VolumeFadeContext makeCtx(bool hasAlert, unsigned long nowMs, uint8_t volume,
                                 uint16_t freq, bool muted = false, bool suppressed = false) {
    VolumeFadeContext ctx;
    ctx.hasAlert = hasAlert;
    ctx.alertMuted = muted;
    ctx.alertSuppressed = suppressed;
    ctx.currentVolume = volume;
    ctx.currentMuteVolume = 0;
    ctx.currentFrequency = freq;
    ctx.now = nowMs;
    return ctx;
}

void test_disabled_feature_returns_none() {
    settingsManager.settings.alertVolumeFadeEnabled = false;
    auto ctx = makeCtx(true, 1000, 7, 34700);
    auto action = fade.process(ctx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::NONE, action.type);
}

void test_fade_triggers_after_delay() {
    settingsManager.settings.alertVolumeFadeEnabled = true;
    settingsManager.settings.alertVolumeFadeDelaySec = 2;  // 2s
    settingsManager.settings.alertVolumeFadeVolume = 3;

    auto ctx = makeCtx(true, 1000, 7, 34700);
    auto action1 = fade.process(ctx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::NONE, action1.type);

    ctx.now = 3500;  // beyond delay (>=2s later)
    auto action2 = fade.process(ctx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::FADE_DOWN, action2.type);
    TEST_ASSERT_EQUAL_UINT8(3, action2.targetVolume);
}

void test_fade_triggers_at_exact_delay_boundary() {
    settingsManager.settings.alertVolumeFadeEnabled = true;
    settingsManager.settings.alertVolumeFadeDelaySec = 2;
    settingsManager.settings.alertVolumeFadeVolume = 3;

    auto ctx = makeCtx(true, 1000, 7, 34700);
    auto action1 = fade.process(ctx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::NONE, action1.type);

    ctx.now = 3000;  // Exactly 2000ms later.
    auto action2 = fade.process(ctx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::FADE_DOWN, action2.type);
    TEST_ASSERT_EQUAL_UINT8(3, action2.targetVolume);
}

void test_new_frequency_restores_when_faded() {
    settingsManager.settings.alertVolumeFadeEnabled = true;
    settingsManager.settings.alertVolumeFadeDelaySec = 1;
    settingsManager.settings.alertVolumeFadeVolume = 2;

    auto ctx = makeCtx(true, 1000, 8, 34700);
    fade.process(ctx);          // start tracking
    ctx.now = 2200;             // trigger fade (>1s)
    auto fadeAction = fade.process(ctx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::FADE_DOWN, fadeAction.type);

    // Now a new frequency arrives while faded, current volume already lowered
    ctx = makeCtx(true, 2300, 2, 35500);  // different freq
    auto restore = fade.process(ctx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::RESTORE, restore.type);
    TEST_ASSERT_EQUAL_UINT8(8, restore.restoreVolume);  // original captured volume
}

void test_muted_alert_during_fade_does_not_restore() {
    settingsManager.settings.alertVolumeFadeEnabled = true;
    settingsManager.settings.alertVolumeFadeDelaySec = 1;
    settingsManager.settings.alertVolumeFadeVolume = 1;

    auto ctx = makeCtx(true, 1000, 6, 24000);
    fade.process(ctx);      // start
    ctx.now = 2200;
    fade.process(ctx);      // fade down

    // Alert reports muted while fade is active — this is the V1 mute indicator
    // triggered by our own fade, not a user mute.  Must NOT restore.
    auto mutedCtx = makeCtx(true, 2500, 1, 24000, true /*muted*/);
    auto action = fade.process(mutedCtx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::NONE, action.type);
}

void test_muted_alert_without_fade_restores() {
    settingsManager.settings.alertVolumeFadeEnabled = true;
    settingsManager.settings.alertVolumeFadeDelaySec = 5;  // long delay — fade won't fire
    settingsManager.settings.alertVolumeFadeVolume = 1;

    // Start alert session; baseline captured but fade not yet triggered.
    auto ctx = makeCtx(true, 1000, 6, 24000);
    fade.process(ctx);

    // User mutes the alert externally (fadeActive_ is false).
    auto mutedCtx = makeCtx(true, 1500, 1, 24000, true /*muted*/);
    auto restore = fade.process(mutedCtx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::RESTORE, restore.type);
    TEST_ASSERT_EQUAL_UINT8(6, restore.restoreVolume);
}

void test_alert_clear_restores_if_needed() {
    settingsManager.settings.alertVolumeFadeEnabled = true;
    settingsManager.settings.alertVolumeFadeDelaySec = 1;
    settingsManager.settings.alertVolumeFadeVolume = 2;

    auto ctx = makeCtx(true, 1000, 7, 24150);
    fade.process(ctx);      // start
    ctx.now = 2100;
    fade.process(ctx);      // fade down
    
    // Alerts clear while volume is faded
    auto clearCtx = makeCtx(false, 2300, 2, 0);
    auto restore = fade.process(clearCtx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::RESTORE, restore.type);
    TEST_ASSERT_EQUAL_UINT8(7, restore.restoreVolume);
}

void test_alert_clear_does_not_restore_when_never_faded() {
    settingsManager.settings.alertVolumeFadeEnabled = true;
    settingsManager.settings.alertVolumeFadeDelaySec = 5;  // Keep fade inactive.
    settingsManager.settings.alertVolumeFadeVolume = 1;

    // Start an alert session; baseline captured as 5, but fade delay not reached.
    auto ctx = makeCtx(true, 1000, 5, 24150);
    auto start = fade.process(ctx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::NONE, start.type);

    // Alert clears with a different current volume (e.g., user/external change).
    auto clearCtx = makeCtx(false, 1200, 1, 0);
    auto restore = fade.process(clearCtx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::NONE, restore.type);
}

void test_alert_clear_restores_when_fade_command_inflight_and_volume_stale() {
    settingsManager.settings.alertVolumeFadeEnabled = true;
    settingsManager.settings.alertVolumeFadeDelaySec = 1;
    settingsManager.settings.alertVolumeFadeVolume = 2;

    auto ctx = makeCtx(true, 1000, 7, 24150);
    fade.process(ctx);      // start
    ctx.now = 2100;
    auto fadeAction = fade.process(ctx);  // issue fade command
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::FADE_DOWN, fadeAction.type);

    // Alerts clear before parser reflects the lowered volume (still reports original).
    auto clearCtx = makeCtx(false, 2200, 7, 0);
    auto restore = fade.process(clearCtx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::RESTORE, restore.type);
    TEST_ASSERT_EQUAL_UINT8(7, restore.restoreVolume);
}

void test_muted_during_fade_inflight_does_not_restore() {
    settingsManager.settings.alertVolumeFadeEnabled = true;
    settingsManager.settings.alertVolumeFadeDelaySec = 1;
    settingsManager.settings.alertVolumeFadeVolume = 1;

    auto ctx = makeCtx(true, 1000, 6, 24000);
    fade.process(ctx);      // start
    ctx.now = 2200;
    auto fadeAction = fade.process(ctx);  // issue fade command
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::FADE_DOWN, fadeAction.type);

    // Mute event arrives while fade is active — V1 mute indicator from our fade,
    // not a user mute.  Must NOT trigger restore (breaks the feedback loop).
    auto mutedCtx = makeCtx(true, 2250, 6, 24000, true /*muted*/);
    auto action = fade.process(mutedCtx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::NONE, action.type);
}

void test_new_frequency_restore_when_fade_command_inflight_and_volume_stale() {
    settingsManager.settings.alertVolumeFadeEnabled = true;
    settingsManager.settings.alertVolumeFadeDelaySec = 1;
    settingsManager.settings.alertVolumeFadeVolume = 2;

    auto ctx = makeCtx(true, 1000, 8, 34700);
    fade.process(ctx);      // start
    ctx.now = 2200;
    auto fadeAction = fade.process(ctx);  // issue fade command
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::FADE_DOWN, fadeAction.type);

    // New frequency arrives before parser reflects the lowered volume.
    auto newFreqCtx = makeCtx(true, 2300, 8, 35500);
    auto restore = fade.process(newFreqCtx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::RESTORE, restore.type);
    TEST_ASSERT_EQUAL_UINT8(8, restore.restoreVolume);
}

void test_restore_baseline_survives_immediate_realert_with_stale_volume() {
    settingsManager.settings.alertVolumeFadeEnabled = true;
    settingsManager.settings.alertVolumeFadeDelaySec = 1;
    settingsManager.settings.alertVolumeFadeVolume = 1;

    auto ctx = makeCtx(true, 1000, 4, 35498);
    fade.process(ctx);  // start tracking

    ctx.now = 2200;
    auto fadeAction = fade.process(ctx);  // fade down to 1
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::FADE_DOWN, fadeAction.type);
    TEST_ASSERT_EQUAL_UINT8(1, fadeAction.targetVolume);

    // Alerts clear while V1 is still reporting faded volume.
    auto clearCtx = makeCtx(false, 2300, 1, 0);
    auto restore1 = fade.process(clearCtx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::RESTORE, restore1.type);
    TEST_ASSERT_EQUAL_UINT8(4, restore1.restoreVolume);

    // New alert arrives before restore state is reflected in display packets.
    auto relertCtx = makeCtx(true, 2350, 1, 35498);
    auto noAction = fade.process(relertCtx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::NONE, noAction.type);

    // Clear again; baseline must remain 4 (not recaptured as 1).
    auto clearCtx2 = makeCtx(false, 2400, 1, 0);
    auto restore2 = fade.process(clearCtx2);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::RESTORE, restore2.type);
    TEST_ASSERT_EQUAL_UINT8(4, restore2.restoreVolume);
}

void test_restore_retries_stop_after_pending_window_expires() {
    settingsManager.settings.alertVolumeFadeEnabled = true;
    settingsManager.settings.alertVolumeFadeDelaySec = 1;
    settingsManager.settings.alertVolumeFadeVolume = 1;

    auto ctx = makeCtx(true, 1000, 5, 35498);
    fade.process(ctx);  // start tracking

    ctx.now = 2200;
    auto fadeAction = fade.process(ctx);  // fade down to 1
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::FADE_DOWN, fadeAction.type);

    // Alerts clear with faded volume. First clear should request restore.
    auto clearCtx = makeCtx(false, 2300, 1, 0);
    auto restore1 = fade.process(clearCtx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::RESTORE, restore1.type);

    // While pending window is open, retries continue.
    auto retryCtx = makeCtx(false, 2400, 1, 0);
    auto restore2 = fade.process(retryCtx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::RESTORE, restore2.type);

    // After pending window expiry, stop retrying and release tracking.
    auto expiredCtx = makeCtx(false, 3900, 1, 0);
    auto none = fade.process(expiredCtx);
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::NONE, none.type);
}

void runAllTests() {
    RUN_TEST(test_disabled_feature_returns_none);
    RUN_TEST(test_fade_triggers_after_delay);
    RUN_TEST(test_fade_triggers_at_exact_delay_boundary);
    RUN_TEST(test_new_frequency_restores_when_faded);
    RUN_TEST(test_muted_alert_during_fade_does_not_restore);
    RUN_TEST(test_muted_alert_without_fade_restores);
    RUN_TEST(test_alert_clear_restores_if_needed);
    RUN_TEST(test_alert_clear_does_not_restore_when_never_faded);
    RUN_TEST(test_alert_clear_restores_when_fade_command_inflight_and_volume_stale);
    RUN_TEST(test_muted_during_fade_inflight_does_not_restore);
    RUN_TEST(test_new_frequency_restore_when_fade_command_inflight_and_volume_stale);
    RUN_TEST(test_restore_baseline_survives_immediate_realert_with_stale_volume);
    RUN_TEST(test_restore_retries_stop_after_pending_window_expires);
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
int main(int argc, char **argv) {
    UNITY_BEGIN();
    runAllTests();
    return UNITY_END();
}
#endif
