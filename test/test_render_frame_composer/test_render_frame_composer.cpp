// RenderFrameComposer — Valentine's Law surface.
//
// This suite exists because a real defect reached main: with the ALP connected
// but showing nothing (LISTENING, or a session withheld as Warm-Up /
// unconfirmed-detect), a live V1 BAND_LASER alert was dropped from the frame
// and no ALP branch replaced it. The composed frame came back IDLE while the
// V1 was reporting laser. Found 2026-08-06; reproduced by compiling this file's
// subject directly and dumping its routing table.
//
// Qualifies under the test policy as (a) pinning a bug that actually happened
// and (c) guarding a Tier-0 invariant.

#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/settings.h"

#ifndef ARDUINO
SerialClass Serial;
SettingsManager settingsManager;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/alp/alp_runtime_module.h"

// The composer reads AlpLaserEvent/AlpSnapshot only; the ALP runtime itself is
// not linked here. Stub the one member the header declares but this suite does
// not exercise, matching test_display_pipeline_module.
void AlpRuntimeModule::logDisplayDecision(uint32_t, const char*, const char*) {}

#include "../../src/modules/display/render_frame_composer.cpp"

namespace {

AlertData makeAlert(Band band, Direction dir = DIR_FRONT) {
    AlertData a{};
    a.isValid = true;
    a.band = band;
    a.direction = dir;
    a.frontStrength = 0xFF;
    a.frequency = (band == BAND_LASER) ? 0 : 34700;
    a.isPriority = true;
    return a;
}

V1Snapshot makeV1(const AlertData* alert) {
    V1Snapshot v1;
    if (alert != nullptr) {
        v1.state.activeBands = alert->band;
        v1.state.arrows = alert->direction;
        v1.alerts = alert;
        v1.alertCount = 1;
        v1.priority = *alert;
        v1.hasRenderablePriority = true;
    }
    return v1;
}

AlpSnapshot makeAlp(bool owns, bool eventActive, bool latched) {
    AlpSnapshot alp;
    alp.ownsLaserDisplay = owns;
    alp.event.active = eventActive;
    alp.event.direction = AlpLaserDirection::FRONT;
    alp.isPersistedLatch = latched;
    alp.latchedEvent.active = latched;
    alp.latchedEvent.direction = AlpLaserDirection::FRONT;
    return alp;
}

RenderFrame compose(const V1Snapshot& v1, const AlpSnapshot& alp) {
    V1Settings settings{};
    return RenderFrameComposer{}.compose(v1, alp, settings, 1000);
}

bool frameHasLaserCard(const RenderFrame& frame) {
    for (int i = 0; i < frame.cardCount; ++i) {
        if (frame.cards[i].kind == RenderFrameCard::Kind::V1 &&
            frame.cards[i].v1Alert.band == BAND_LASER) {
            return true;
        }
    }
    return false;
}

} // namespace

// ── The defect ───────────────────────────────────────────────────────
//
// ALP connected, nothing presentable. The V1's own laser must still render.
// This covers LISTENING (ALP sees nothing) and a withheld Warm-Up /
// unconfirmed-detect session identically: both produce ownsLaserDisplay()
// true with currentEvent().active false.

void test_render_frame_composer_v1_laser_survives_silent_alp() {
    const AlertData laser = makeAlert(BAND_LASER);
    const RenderFrame frame = compose(makeV1(&laser), makeAlp(true, false, false));

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, frame.primaryKind);
    TEST_ASSERT_EQUAL(BAND_LASER, frame.v1Priority.band);
    TEST_ASSERT_EQUAL(DIR_FRONT, frame.v1Priority.direction);
}

void test_render_frame_composer_v1_laser_renders_without_alp() {
    const AlertData laser = makeAlert(BAND_LASER);
    const RenderFrame frame = compose(makeV1(&laser), makeAlp(false, false, false));

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, frame.primaryKind);
    TEST_ASSERT_EQUAL(BAND_LASER, frame.v1Priority.band);
}

// ── ALP authority, where it is real ──────────────────────────────────

void test_render_frame_composer_alp_live_outranks_v1_laser() {
    // ALP has something to show: it owns the render, and the V1's duplicate
    // laser is dropped rather than double-rendered.
    const AlertData laser = makeAlert(BAND_LASER);
    const RenderFrame frame = compose(makeV1(&laser), makeAlp(true, true, false));

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, frame.primaryKind);
    TEST_ASSERT_FALSE(frameHasLaserCard(frame));
}

// ── Live beats persisted (screen/speaker contract, ownership rule 3) ─

void test_render_frame_composer_v1_live_outranks_alp_persisted() {
    // A persisted ALP frame is memory, not a live threat. Live V1 truth —
    // radar or the V1's own laser — takes the screen from it.
    const AlertData ka = makeAlert(BAND_KA);
    const RenderFrame radar = compose(makeV1(&ka), makeAlp(true, false, true));
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, radar.primaryKind);
    TEST_ASSERT_EQUAL(BAND_KA, radar.v1Priority.band);

    const AlertData laser = makeAlert(BAND_LASER);
    const RenderFrame live = compose(makeV1(&laser), makeAlp(true, false, true));
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, live.primaryKind);
    TEST_ASSERT_EQUAL(BAND_LASER, live.v1Priority.band);
}

void test_render_frame_composer_alp_persisted_renders_when_v1_idle() {
    const RenderFrame frame = compose(makeV1(nullptr), makeAlp(true, false, true));

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_PERSISTED, frame.primaryKind);
    TEST_ASSERT_FALSE(frameHasLaserCard(frame));
}

void test_render_frame_composer_alp_does_not_suppress_v1_radar() {
    // ALP authority covers laser only. A live Ka alert is never dropped, and
    // stays visible as a card while ALP holds the primary slot.
    const AlertData ka = makeAlert(BAND_KA);
    const RenderFrame frame = compose(makeV1(&ka), makeAlp(true, true, false));

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, frame.primaryKind);
    TEST_ASSERT_EQUAL(1, frame.cardCount);
    TEST_ASSERT_EQUAL(BAND_KA, frame.cards[0].v1Alert.band);
}

// ── Part III claim: a live ALP laser never composes muted ────────────

void test_render_frame_composer_alp_primary_is_never_muted() {
    const AlertData none{};
    V1Snapshot v1 = makeV1(nullptr);
    v1.state.muted = true; // V1 reports muted; ALP owns its own speaker
    (void)none;

    const RenderFrame live = compose(v1, makeAlp(true, true, false));
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, live.primaryKind);
    TEST_ASSERT_FALSE(live.primaryState.muted);

    const RenderFrame persisted = compose(v1, makeAlp(true, false, true));
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_PERSISTED, persisted.primaryKind);
    TEST_ASSERT_FALSE(persisted.primaryState.muted);
}

void test_render_frame_composer_alp_primary_renders_full_urgency() {
    const RenderFrame frame = compose(makeV1(nullptr), makeAlp(true, true, false));

    TEST_ASSERT_EQUAL(BAND_LASER, frame.primaryState.activeBands);
    TEST_ASSERT_EQUAL(DIR_FRONT, frame.primaryState.arrows);
    TEST_ASSERT_EQUAL(0, frame.primaryState.flashBits);
    TEST_ASSERT_EQUAL(0, frame.primaryState.bandFlashBits);
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_render_frame_composer_v1_laser_survives_silent_alp);
    RUN_TEST(test_render_frame_composer_v1_laser_renders_without_alp);
    RUN_TEST(test_render_frame_composer_alp_live_outranks_v1_laser);
    RUN_TEST(test_render_frame_composer_v1_live_outranks_alp_persisted);
    RUN_TEST(test_render_frame_composer_alp_persisted_renders_when_v1_idle);
    RUN_TEST(test_render_frame_composer_alp_does_not_suppress_v1_radar);
    RUN_TEST(test_render_frame_composer_alp_primary_is_never_muted);
    RUN_TEST(test_render_frame_composer_alp_primary_renders_full_urgency);
    return UNITY_END();
}
