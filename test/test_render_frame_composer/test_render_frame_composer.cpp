#include <unity.h>

#include "../../src/modules/alp/alp_runtime_module.h"
#include "../../src/modules/display/render_frame_composer.h"
#include "../../src/modules/display/render_frame_composer.cpp"

static RenderFrameComposer composer;

static AlertData makeAlert(Band band,
                           Direction direction,
                           uint32_t frequency,
                           uint8_t frontStrength = 5,
                           uint8_t rearStrength = 0,
                           bool priority = false) {
    AlertData alert = AlertData::create(band, direction, frontStrength, rearStrength, frequency, true, priority);
    alert.isPriority = priority;
    return alert;
}

static DisplayState makeState(uint8_t activeBands = BAND_NONE,
                              Direction arrows = DIR_NONE,
                              uint8_t signalBars = 0) {
    DisplayState state{};
    state.activeBands = activeBands;
    state.arrows = arrows;
    state.priorityArrow = arrows;
    state.signalBars = signalBars;
    state.mainVolume = 4;
    state.muteVolume = 2;
    state.hasVolumeData = true;
    state.bogeyCounterChar = '1';
    return state;
}

void setUp() {}
void tearDown() {}

void test_compose_idle_when_v1_and_alp_idle() {
    V1Snapshot v1{};
    v1.state = makeState();
    AlpSnapshot alp{};

    const RenderFrame frame = composer.compose(v1, alp, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, frame.primaryKind);
    TEST_ASSERT_EQUAL(BAND_NONE, frame.primaryState.activeBands);
}

void test_compose_v1_live_when_v1_has_priority_and_alp_idle() {
    AlertData alerts[] = {makeAlert(BAND_KA, DIR_FRONT, 34520, 6, 0, true)};
    V1Snapshot v1{};
    v1.state = makeState(BAND_KA, DIR_FRONT, 6);
    v1.alerts = alerts;
    v1.alertCount = 1;
    v1.priority = alerts[0];
    v1.hasRenderablePriority = true;

    const RenderFrame frame = composer.compose(v1, AlpSnapshot{}, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, frame.primaryKind);
    TEST_ASSERT_EQUAL(BAND_KA, frame.v1Priority.band);
    TEST_ASSERT_EQUAL(0, frame.cardCount);
    TEST_ASSERT_EQUAL(BAND_KA, frame.primaryState.activeBands);
}

void test_compose_v1_live_signal_bars_follow_parsed_display_strength() {
    AlertData alerts[] = {
        makeAlert(BAND_KA, DIR_FRONT, 34520, 2, 0, true),
        makeAlert(BAND_K, DIR_REAR, 24150, 0, 6, false)
    };
    V1Snapshot v1{};
    v1.state = makeState(static_cast<uint8_t>(BAND_KA | BAND_K),
                         static_cast<Direction>(DIR_FRONT | DIR_REAR),
                         6);
    v1.alerts = alerts;
    v1.alertCount = 2;
    v1.priority = alerts[0];
    v1.hasRenderablePriority = true;

    const RenderFrame frame = composer.compose(v1, AlpSnapshot{}, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, frame.primaryKind);
    TEST_ASSERT_EQUAL(BAND_KA, frame.v1Priority.band);
    TEST_ASSERT_EQUAL(6, frame.primaryState.signalBars);
    TEST_ASSERT_EQUAL(6, frame.context.signalBars);
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(BAND_KA | BAND_K), frame.primaryState.activeBands);
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(DIR_FRONT | DIR_REAR),
                      static_cast<uint8_t>(frame.primaryState.arrows));
    TEST_ASSERT_EQUAL(1, frame.cardCount);
    TEST_ASSERT_EQUAL(6, frame.cards[0].v1Alert.rearStrength);
}

void test_compose_v1_live_signal_bars_ignore_alert_row_strength() {
    AlertData alerts[] = {makeAlert(BAND_K, DIR_REAR, 24150, 1, 5, true)};
    V1Snapshot v1{};
    v1.state = makeState(BAND_K, DIR_REAR, 1);
    v1.alerts = alerts;
    v1.alertCount = 1;
    v1.priority = alerts[0];
    v1.hasRenderablePriority = true;

    const RenderFrame frame = composer.compose(v1, AlpSnapshot{}, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, frame.primaryKind);
    TEST_ASSERT_EQUAL(1, frame.primaryState.signalBars);
}

void test_compose_v1_live_laser_signal_bars_still_follow_parsed_display_strength() {
    AlertData alerts[] = {makeAlert(BAND_LASER, DIR_FRONT, 0, 0, 0, true)};
    V1Snapshot v1{};
    v1.state = makeState(BAND_LASER, DIR_FRONT, 1);
    v1.alerts = alerts;
    v1.alertCount = 1;
    v1.priority = alerts[0];
    v1.hasRenderablePriority = true;

    const RenderFrame frame = composer.compose(v1, AlpSnapshot{}, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, frame.primaryKind);
    TEST_ASSERT_EQUAL(1, frame.primaryState.signalBars);
}

void test_compose_alp_live_when_alp_event_active() {
    V1Snapshot v1{};
    v1.state = makeState();
    AlpSnapshot alp{};
    alp.ownsLaserDisplay = true;
    alp.event.active = true;
    alp.event.gun = AlpGunType::MARKSMAN_ULTRALYTE;
    alp.event.direction = AlpLaserDirection::FRONT;

    const RenderFrame frame = composer.compose(v1, alp, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, frame.primaryKind);
    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, frame.alpPrimary.gun);
    TEST_ASSERT_EQUAL(BAND_LASER, frame.primaryState.activeBands);
    TEST_ASSERT_EQUAL(DIR_FRONT, frame.primaryState.priorityArrow);
    // Laser = full deflection on the local 8-slot meter scale.
    TEST_ASSERT_EQUAL(8, frame.primaryState.signalBars);
}

void test_compose_alp_live_unknown_direction_preserves_dir_none() {
    V1Snapshot v1{};
    v1.state = makeState();
    AlpSnapshot alp{};
    alp.ownsLaserDisplay = true;
    alp.event.active = true;
    alp.event.gun = AlpGunType::MARKSMAN_ULTRALYTE;
    alp.event.direction = AlpLaserDirection::UNKNOWN;

    const RenderFrame frame = composer.compose(v1, alp, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, frame.primaryKind);
    TEST_ASSERT_EQUAL(BAND_LASER, frame.primaryState.activeBands);
    TEST_ASSERT_EQUAL(DIR_NONE, frame.primaryState.arrows);
    TEST_ASSERT_EQUAL(DIR_NONE, frame.primaryState.priorityArrow);
}

void test_compose_alp_live_keeps_v1_priority_as_card() {
    AlertData alerts[] = {makeAlert(BAND_KA, DIR_FRONT, 34520, 6, 0, true)};
    V1Snapshot v1{};
    v1.state = makeState(BAND_KA, DIR_FRONT, 6);
    v1.alerts = alerts;
    v1.alertCount = 1;
    v1.priority = alerts[0];
    v1.hasRenderablePriority = true;
    AlpSnapshot alp{};
    alp.ownsLaserDisplay = true;
    alp.event.active = true;
    alp.event.gun = AlpGunType::PL3_PROLITE;
    alp.event.direction = AlpLaserDirection::REAR;

    const RenderFrame frame = composer.compose(v1, alp, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, frame.primaryKind);
    TEST_ASSERT_EQUAL(1, frame.cardCount);
    TEST_ASSERT_EQUAL(RenderFrameCard::Kind::V1, frame.cards[0].kind);
    TEST_ASSERT_EQUAL(BAND_KA, frame.cards[0].v1Alert.band);
}

void test_compose_alp_live_keeps_multiple_v1_cards() {
    AlertData alerts[] = {
        makeAlert(BAND_KA, DIR_FRONT, 34520, 6, 0, true),
        makeAlert(BAND_K, DIR_REAR, 24148, 0, 4)
    };
    V1Snapshot v1{};
    v1.state = makeState(BAND_KA | BAND_K,
                         static_cast<Direction>(DIR_FRONT | DIR_REAR),
                         6);
    v1.alerts = alerts;
    v1.alertCount = 2;
    v1.priority = alerts[0];
    v1.hasRenderablePriority = true;
    AlpSnapshot alp{};
    alp.ownsLaserDisplay = true;
    alp.event.active = true;
    alp.event.gun = AlpGunType::PL3_PROLITE;
    alp.event.direction = AlpLaserDirection::FRONT;

    const RenderFrame frame = composer.compose(v1, alp, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, frame.primaryKind);
    TEST_ASSERT_EQUAL(2, frame.cardCount);
    TEST_ASSERT_EQUAL(BAND_KA, frame.cards[0].v1Alert.band);
    TEST_ASSERT_EQUAL(BAND_K, frame.cards[1].v1Alert.band);
}

void test_compose_alp_persisted_when_latch_active() {
    V1Snapshot v1{};
    v1.state = makeState();
    AlpSnapshot alp{};
    alp.ownsLaserDisplay = true;
    alp.isPersistedLatch = true;
    alp.latchedEvent.active = true;
    alp.latchedEvent.gun = AlpGunType::LASER_ATLANTA_PL2;
    alp.latchedEvent.direction = AlpLaserDirection::REAR;

    const RenderFrame frame = composer.compose(v1, alp, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_PERSISTED, frame.primaryKind);
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, frame.alpPrimary.gun);
    TEST_ASSERT_EQUAL(DIR_REAR, frame.primaryState.priorityArrow);
}

void test_compose_alp_persisted_when_latch_active_after_live_ownership_drops() {
    V1Snapshot v1{};
    v1.state = makeState();
    AlpSnapshot alp{};
    alp.ownsLaserDisplay = false;
    alp.isPersistedLatch = true;
    alp.latchedEvent.active = true;
    alp.latchedEvent.gun = AlpGunType::MARKSMAN_ULTRALYTE;
    alp.latchedEvent.direction = AlpLaserDirection::FRONT;

    const RenderFrame frame = composer.compose(v1, alp, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_PERSISTED, frame.primaryKind);
    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, frame.alpPrimary.gun);
    TEST_ASSERT_EQUAL(BAND_LASER, frame.primaryState.activeBands);
    TEST_ASSERT_EQUAL(DIR_FRONT, frame.primaryState.priorityArrow);
}

void test_compose_v1_persisted_when_no_live_sources() {
    V1Snapshot v1{};
    v1.state = makeState();
    v1.hasPersistedAlert = true;
    v1.persistedAlert = makeAlert(BAND_KA, DIR_FRONT, 34520, 6, 0);

    const RenderFrame frame = composer.compose(v1, AlpSnapshot{}, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_PERSISTED, frame.primaryKind);
    TEST_ASSERT_EQUAL(BAND_KA, frame.v1Priority.band);
}

void test_compose_alp_live_beats_v1_persisted() {
    V1Snapshot v1{};
    v1.state = makeState();
    v1.hasPersistedAlert = true;
    v1.persistedAlert = makeAlert(BAND_KA, DIR_FRONT, 34520, 6, 0);
    AlpSnapshot alp{};
    alp.ownsLaserDisplay = true;
    alp.event.active = true;
    alp.event.gun = AlpGunType::PL3_PROLITE;
    alp.event.direction = AlpLaserDirection::FRONT;

    const RenderFrame frame = composer.compose(v1, alp, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, frame.primaryKind);
    TEST_ASSERT_EQUAL(0, frame.cardCount);
}

void test_compose_filters_v1_laser_cards_when_alp_owns_laser() {
    AlertData alerts[] = {makeAlert(BAND_LASER, DIR_FRONT, 0, 6, 0, true)};
    V1Snapshot v1{};
    v1.state = makeState(BAND_LASER, DIR_FRONT, 6);
    v1.alerts = alerts;
    v1.alertCount = 1;
    v1.priority = alerts[0];
    v1.hasRenderablePriority = true;
    AlpSnapshot alp{};
    alp.ownsLaserDisplay = true;
    alp.event.active = true;
    alp.event.gun = AlpGunType::MARKSMAN_ULTRALYTE;
    alp.event.direction = AlpLaserDirection::FRONT;

    const RenderFrame frame = composer.compose(v1, alp, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, frame.primaryKind);
    TEST_ASSERT_EQUAL(0, frame.cardCount);
}

void test_compose_alp_live_suppresses_v1_laser_but_keeps_radar_card() {
    AlertData alerts[] = {
        makeAlert(BAND_LASER, DIR_FRONT, 0, 6, 0, true),
        makeAlert(BAND_KA, DIR_REAR, 34520, 0, 4)
    };
    V1Snapshot v1{};
    v1.state = makeState(BAND_LASER | BAND_KA,
                         static_cast<Direction>(DIR_FRONT | DIR_REAR),
                         6);
    v1.alerts = alerts;
    v1.alertCount = 2;
    v1.priority = alerts[0];
    v1.hasRenderablePriority = true;

    AlpSnapshot alp{};
    alp.ownsLaserDisplay = true;
    alp.event.active = true;
    alp.event.gun = AlpGunType::PL3_PROLITE;
    alp.event.direction = AlpLaserDirection::FRONT;

    const RenderFrame frame = composer.compose(v1, alp, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, frame.primaryKind);
    TEST_ASSERT_EQUAL(1, frame.cardCount);
    TEST_ASSERT_EQUAL(RenderFrameCard::Kind::V1, frame.cards[0].kind);
    TEST_ASSERT_EQUAL(BAND_KA, frame.cards[0].v1Alert.band);
}

void test_compose_allows_v1_laser_when_alp_not_connected() {
    AlertData alerts[] = {makeAlert(BAND_LASER, DIR_FRONT, 0, 6, 0, true)};
    V1Snapshot v1{};
    v1.state = makeState(BAND_LASER, DIR_FRONT, 6);
    v1.alerts = alerts;
    v1.alertCount = 1;
    v1.priority = alerts[0];
    v1.hasRenderablePriority = true;

    const RenderFrame frame = composer.compose(v1, AlpSnapshot{}, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, frame.primaryKind);
    TEST_ASSERT_EQUAL(BAND_LASER, frame.v1Priority.band);
}

// ALP authority: ALP owns the laser alert end-to-end (alerting + its own
// speaker). The V1's SAVVY/OBD-driven mute bit has no bearing on whether
// the driver is hearing this alert, so the composer must never carry it
// forward onto an ALP primary frame. Source of the visible bug fixed here:
// with OBD connected and vol=0, a sustained ALP laser engagement showed
// the mute icon flashing as v1.state.muted ticked under the 2-packet
// hysteresis. Project rule: gun-ID'd alerts should never get "muted" visually.
void test_compose_alp_live_never_surfaces_v1_muted() {
    V1Snapshot v1{};
    v1.state = makeState();
    v1.state.muted = true;  // V1 is muted (SAVVY/OBD pulled vol=0)
    AlpSnapshot alp{};
    alp.ownsLaserDisplay = true;
    alp.event.active = true;
    alp.event.gun = AlpGunType::MARKSMAN_ULTRALYTE;
    alp.event.direction = AlpLaserDirection::FRONT;

    const RenderFrame frame = composer.compose(v1, alp, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, frame.primaryKind);
    TEST_ASSERT_FALSE(frame.primaryState.muted);
}

void test_compose_alp_persisted_never_surfaces_v1_muted() {
    V1Snapshot v1{};
    v1.state = makeState();
    v1.state.muted = true;
    AlpSnapshot alp{};
    alp.ownsLaserDisplay = false;
    alp.isPersistedLatch = true;
    alp.latchedEvent.active = true;
    alp.latchedEvent.gun = AlpGunType::PL3_PROLITE;
    alp.latchedEvent.direction = AlpLaserDirection::REAR;

    const RenderFrame frame = composer.compose(v1, alp, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_PERSISTED, frame.primaryKind);
    TEST_ASSERT_FALSE(frame.primaryState.muted);
}

// Regression guard: the mute suppression must be scoped to ALP primaries.
// V1 alerts (including V1's own laser fallback when ALP is disconnected)
// continue to honour the V1 mute bit end-to-end, because V1 is the sole
// audio source for those alerts and the driver needs to know it was
// silenced.
void test_compose_v1_live_preserves_v1_muted() {
    AlertData alerts[] = {makeAlert(BAND_KA, DIR_FRONT, 34520, 6, 0, true)};
    V1Snapshot v1{};
    v1.state = makeState(BAND_KA, DIR_FRONT, 6);
    v1.state.muted = true;
    v1.alerts = alerts;
    v1.alertCount = 1;
    v1.priority = alerts[0];
    v1.hasRenderablePriority = true;

    const RenderFrame frame = composer.compose(v1, AlpSnapshot{}, V1Settings{}, 1000);

    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, frame.primaryKind);
    TEST_ASSERT_TRUE(frame.primaryState.muted);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_compose_idle_when_v1_and_alp_idle);
    RUN_TEST(test_compose_v1_live_when_v1_has_priority_and_alp_idle);
    RUN_TEST(test_compose_v1_live_signal_bars_follow_parsed_display_strength);
    RUN_TEST(test_compose_v1_live_signal_bars_ignore_alert_row_strength);
    RUN_TEST(test_compose_v1_live_laser_signal_bars_still_follow_parsed_display_strength);
    RUN_TEST(test_compose_alp_live_when_alp_event_active);
    RUN_TEST(test_compose_alp_live_unknown_direction_preserves_dir_none);
    RUN_TEST(test_compose_alp_live_keeps_v1_priority_as_card);
    RUN_TEST(test_compose_alp_live_keeps_multiple_v1_cards);
    RUN_TEST(test_compose_alp_persisted_when_latch_active);
    RUN_TEST(test_compose_alp_persisted_when_latch_active_after_live_ownership_drops);
    RUN_TEST(test_compose_v1_persisted_when_no_live_sources);
    RUN_TEST(test_compose_alp_live_beats_v1_persisted);
    RUN_TEST(test_compose_filters_v1_laser_cards_when_alp_owns_laser);
    RUN_TEST(test_compose_alp_live_suppresses_v1_laser_but_keeps_radar_card);
    RUN_TEST(test_compose_allows_v1_laser_when_alp_not_connected);
    RUN_TEST(test_compose_alp_live_never_surfaces_v1_muted);
    RUN_TEST(test_compose_alp_persisted_never_surfaces_v1_muted);
    RUN_TEST(test_compose_v1_live_preserves_v1_muted);
    return UNITY_END();
}
