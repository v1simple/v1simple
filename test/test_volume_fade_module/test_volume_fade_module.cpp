// VolumeFadeModule distinct-alert release — Valentine's Law surface.
//
// Screen/speaker contract (docs/VALENTINE_PHILOSOPHY.md): within the volume
// the user allows, a newly detected, distinct alert releases any active fade
// mute and sounds again. Radar alerts are identified by frequency; laser has
// no frequency, so it is deduped as its own distinct-alert class. This suite
// pins the defect where a laser arriving mid-fade — the highest-urgency
// threat — was the one alert class that could ride through a fade silently.

#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/settings.h"

#ifndef ARDUINO
SerialClass Serial;
SettingsManager settingsManager;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/perf_metrics.h"

void perfRecordVolumeFadeDecision(PerfFadeDecision /*decision*/, uint8_t /*currentVolume*/,
                                  uint8_t /*originalVolume*/, uint32_t /*nowMs*/) {}

#include "../../src/modules/volume_fade/volume_fade_module.cpp"

namespace {

SettingsManager gSettings;
VolumeFadeModule gModule;

VolumeFadeContext makeCtx(unsigned long now, uint16_t freq, bool laser, uint8_t vol = 6) {
    VolumeFadeContext ctx;
    ctx.hasAlert = true;
    ctx.alertMuted = false;
    ctx.alertSuppressed = false;
    ctx.currentVolume = vol;
    ctx.currentMuteVolume = 5;
    ctx.currentFrequency = freq;
    ctx.priorityIsLaser = laser;
    ctx.now = now;
    return ctx;
}

// Start a K-band session at t=1000 and fade it down at t=3500 (delay 2 s).
void startFadedKbandSession() {
    TEST_ASSERT_FALSE(gModule.process(makeCtx(1000, 24150, false)).hasAction());
    const VolumeFadeAction fade = gModule.process(makeCtx(3500, 24150, false));
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::FADE_DOWN, fade.type);
    TEST_ASSERT_EQUAL(1, fade.targetVolume);
}

} // namespace

void setUp() {
    gModule = VolumeFadeModule{};
    gSettings = SettingsManager{};
    gSettings.settings.alertVolumeFadeEnabled = true;
    gSettings.settings.alertVolumeFadeDelaySec = 2;
    gSettings.settings.alertVolumeFadeVolume = 1;
    gModule.begin(&gSettings);
}

void tearDown() {}

void test_new_laser_releases_active_fade() {
    startFadedKbandSession();

    // Laser arrives mid-fade: restore immediately, once.
    const VolumeFadeAction release = gModule.process(makeCtx(4000, 0, true, 1));
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::RESTORE, release.type);
    TEST_ASSERT_EQUAL(6, release.restoreVolume);

    // Same laser is deduped — no second release.
    TEST_ASSERT_FALSE(gModule.process(makeCtx(4100, 0, true)).hasAction());

    // The fade timer restarted at the release: fade fires again ~2 s later.
    const VolumeFadeAction refade = gModule.process(makeCtx(6100, 0, true));
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::FADE_DOWN, refade.type);
}

void test_new_frequency_still_releases_active_fade() {
    startFadedKbandSession();

    const VolumeFadeAction release = gModule.process(makeCtx(4000, 34700, false, 1));
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::RESTORE, release.type);
    TEST_ASSERT_EQUAL(6, release.restoreVolume);
}

void test_laser_at_session_start_is_not_a_release() {
    // Session opens on laser: it is the session's first alert, not a new one.
    TEST_ASSERT_FALSE(gModule.process(makeCtx(1000, 0, true)).hasAction());
    const VolumeFadeAction fade = gModule.process(makeCtx(3500, 0, true));
    TEST_ASSERT_EQUAL(VolumeFadeAction::Type::FADE_DOWN, fade.type);
    TEST_ASSERT_FALSE(gModule.process(makeCtx(3600, 0, true, 1)).hasAction());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_new_laser_releases_active_fade);
    RUN_TEST(test_new_frequency_still_releases_active_fade);
    RUN_TEST(test_laser_at_session_start_is_not_a_release);
    return UNITY_END();
}
