#include <unity.h>
#include <stdio.h>

#include "../../src/modules/alp/alp_runtime_module.h"
#include "../../src/modules/display/render_frame_composer.h"
#include "../../src/modules/display/render_frame_composer.cpp"
#include "../../src/modules/display/display_correctness_trace.cpp"

static AlertData makeAlert(Band band, Direction direction, uint32_t frequency) {
    return AlertData::create(band, direction, 6, 0, frequency, true, true);
}

static DisplayState makeState(uint8_t activeBands = BAND_KA,
                              Direction arrows = DIR_FRONT,
                              uint8_t signalBars = 6) {
    DisplayState state{};
    state.activeBands = activeBands;
    state.arrows = arrows;
    state.priorityArrow = arrows;
    state.signalBars = signalBars;
    state.bogeyCounterChar = '1';
    state.muted = true;
    return state;
}

static const char* primaryKindName(RenderFramePrimaryKind kind) {
    switch (kind) {
        case RenderFramePrimaryKind::IDLE:
            return "IDLE";
        case RenderFramePrimaryKind::V1_LIVE:
            return "V1_LIVE";
        case RenderFramePrimaryKind::V1_PERSISTED:
            return "V1_PERSISTED";
        case RenderFramePrimaryKind::ALP_LIVE:
            return "ALP_LIVE";
        case RenderFramePrimaryKind::ALP_PERSISTED:
            return "ALP_PERSISTED";
        case RenderFramePrimaryKind::NONE:
        default:
            return "NONE";
    }
}

static const char* ownerName(DisplayCorrectnessOwner owner) {
    switch (owner) {
        case DisplayCorrectnessOwner::IDLE:
            return "IDLE";
        case DisplayCorrectnessOwner::V1:
            return "V1";
        case DisplayCorrectnessOwner::ALP:
            return "ALP";
        case DisplayCorrectnessOwner::NONE:
        default:
            return "NONE";
    }
}

static const char* statusName(DisplayCorrectnessStatus status) {
    switch (status) {
        case DisplayCorrectnessStatus::MATCH:
            return "M";
        case DisplayCorrectnessStatus::MISMATCH:
            return "X";
        case DisplayCorrectnessStatus::NOT_APPLICABLE:
        default:
            return "NA";
    }
}

static void writeTraceSnapshot(const DisplayCorrectnessTraceEvent& event,
                               char* out,
                               size_t outSize) {
    snprintf(out,
             outSize,
             "kind=%s owner=%s cards=%u band=%u>%u/%s arrows=%u>%u/%s "
             "freq=%lu>%lu/%s bogey=%c>%c/%s mute=%u>%u/%s bars=%u>%u/%s",
             primaryKindName(event.primaryKind),
             ownerName(event.owner),
             event.cardCount,
             event.sourceBand,
             event.renderedBand,
             statusName(event.bandStatus),
             event.sourceArrows,
             event.renderedArrows,
             statusName(event.arrowsStatus),
             static_cast<unsigned long>(event.sourceFrequency),
             static_cast<unsigned long>(event.renderedFrequency),
             statusName(event.frequencyStatus),
             event.sourceBogey ? event.sourceBogey : '-',
             event.renderedBogey ? event.renderedBogey : '-',
             statusName(event.bogeyStatus),
             event.sourceMuted ? 1u : 0u,
             event.renderedMuted ? 1u : 0u,
             statusName(event.muteStatus),
             event.sourceSignalBars,
             event.renderedSignalBars,
             statusName(event.signalBarsStatus));
}

static void assertTraceSnapshot(const RenderFrame& frame, const char* expected) {
    const DisplayCorrectnessTraceEvent event =
        buildDisplayCorrectnessTraceEvent(frame, 5000);
    char actual[256]{};
    writeTraceSnapshot(event, actual, sizeof(actual));
    TEST_ASSERT_EQUAL_STRING(expected, actual);
}

void setUp() {
    displayCorrectnessTraceReset();
}

void tearDown() {}

void test_v1_live_trace_marks_primary_fields_as_matching() {
    RenderFrame frame{};
    frame.primaryKind = RenderFramePrimaryKind::V1_LIVE;
    frame.context = makeState();
    frame.primaryState = makeState();
    frame.v1Priority = makeAlert(BAND_KA, DIR_FRONT, 34520);
    frame.cardCount = 2;

    const DisplayCorrectnessTraceEvent event =
        buildDisplayCorrectnessTraceEvent(frame, 1234);

    TEST_ASSERT_EQUAL_UINT32(1234u, event.tsMs);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DisplayCorrectnessOwner::V1),
                            static_cast<uint8_t>(event.owner));
    TEST_ASSERT_EQUAL_UINT8(BAND_KA, event.sourceBand);
    TEST_ASSERT_EQUAL_UINT8(BAND_KA, event.renderedBand);
    TEST_ASSERT_EQUAL_UINT32(34520u, event.sourceFrequency);
    TEST_ASSERT_EQUAL_UINT32(34520u, event.renderedFrequency);
    TEST_ASSERT_TRUE(event.sourceMuted);
    TEST_ASSERT_TRUE(event.renderedMuted);
    TEST_ASSERT_EQUAL_UINT8(2u, event.cardCount);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DisplayCorrectnessStatus::MATCH),
                            static_cast<uint8_t>(event.bandStatus));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DisplayCorrectnessStatus::MATCH),
                            static_cast<uint8_t>(event.frequencyStatus));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DisplayCorrectnessStatus::MATCH),
                            static_cast<uint8_t>(event.muteStatus));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DisplayCorrectnessStatus::MATCH),
                            static_cast<uint8_t>(event.signalBarsStatus));
}

void test_v1_live_trace_compares_signal_bars_to_frame_context() {
    RenderFrame frame{};
    frame.primaryKind = RenderFramePrimaryKind::V1_LIVE;
    frame.context = makeState();
    frame.context.signalBars = 2;
    frame.primaryState = makeState();
    frame.primaryState.signalBars = 5;
    frame.v1Priority = makeAlert(BAND_KA, DIR_FRONT, 34520);

    const DisplayCorrectnessTraceEvent event =
        buildDisplayCorrectnessTraceEvent(frame, 1235);

    TEST_ASSERT_EQUAL_UINT8(2, event.sourceSignalBars);
    TEST_ASSERT_EQUAL_UINT8(5, event.renderedSignalBars);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DisplayCorrectnessStatus::MISMATCH),
                            static_cast<uint8_t>(event.signalBarsStatus));
}

void test_alp_live_trace_records_alp_ownership_and_unmuted_render() {
    RenderFrame frame{};
    frame.primaryKind = RenderFramePrimaryKind::ALP_LIVE;
    frame.context = makeState();
    frame.primaryState = makeState();
    frame.primaryState.activeBands = BAND_LASER;
    frame.primaryState.arrows = DIR_REAR;
    frame.primaryState.priorityArrow = DIR_REAR;
    frame.primaryState.signalBars = 6;
    frame.primaryState.muted = false;
    frame.alpPrimary.active = true;
    frame.alpPrimary.direction = AlpLaserDirection::REAR;

    const DisplayCorrectnessTraceEvent event =
        buildDisplayCorrectnessTraceEvent(frame, 4321);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DisplayCorrectnessOwner::ALP),
                            static_cast<uint8_t>(event.owner));
    TEST_ASSERT_EQUAL_UINT8(BAND_LASER, event.sourceBand);
    TEST_ASSERT_EQUAL_UINT8(BAND_LASER, event.renderedBand);
    TEST_ASSERT_EQUAL_UINT8(DIR_REAR, event.sourceArrows);
    TEST_ASSERT_EQUAL_UINT8(DIR_REAR, event.renderedArrows);
    TEST_ASSERT_TRUE(event.sourceMuted);
    TEST_ASSERT_FALSE(event.renderedMuted);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DisplayCorrectnessStatus::NOT_APPLICABLE),
                            static_cast<uint8_t>(event.muteStatus));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DisplayCorrectnessStatus::MATCH),
                            static_cast<uint8_t>(event.bandStatus));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DisplayCorrectnessStatus::MATCH),
                            static_cast<uint8_t>(event.arrowsStatus));
}

void test_trace_log_is_bounded_and_copies_most_recent_first() {
    for (uint32_t i = 0; i < DisplayCorrectnessTraceLog::kCapacity + 3; ++i) {
        RenderFrame frame{};
        frame.primaryKind = RenderFramePrimaryKind::IDLE;
        frame.context = makeState();
        frame.primaryState = makeState();
        DisplayCorrectnessTraceEvent event = buildDisplayCorrectnessTraceEvent(frame, i);
        event.seq = i + 1;
        displayCorrectnessTracePublish(event);
    }

    const DisplayCorrectnessTraceStats stats = displayCorrectnessTraceStats();
    TEST_ASSERT_EQUAL_UINT32(DisplayCorrectnessTraceLog::kCapacity + 3u, stats.published);
    TEST_ASSERT_EQUAL_UINT32(3u, stats.drops);
    TEST_ASSERT_EQUAL_UINT32(DisplayCorrectnessTraceLog::kCapacity, stats.size);

    DisplayCorrectnessTraceEvent recent[4]{};
    const size_t copied = displayCorrectnessTraceCopyRecent(recent, 4);
    TEST_ASSERT_EQUAL_UINT32(4u, copied);
    TEST_ASSERT_EQUAL_UINT32(DisplayCorrectnessTraceLog::kCapacity + 3u, recent[0].seq);
    TEST_ASSERT_EQUAL_UINT32(DisplayCorrectnessTraceLog::kCapacity + 2u, recent[1].seq);
    TEST_ASSERT_EQUAL_UINT32(DisplayCorrectnessTraceLog::kCapacity + 1u, recent[2].seq);
    TEST_ASSERT_EQUAL_UINT32(DisplayCorrectnessTraceLog::kCapacity, recent[3].seq);
}

void test_trace_snapshots_cover_v1_alp_persisted_and_idle_contracts() {
    RenderFrameComposer composer;

    AlertData liveAlerts[] = {
        makeAlert(BAND_KA, DIR_FRONT, 34520),
        makeAlert(BAND_K, DIR_REAR, 24150),
    };
    V1Snapshot liveV1{};
    liveV1.state = makeState();
    liveV1.state.activeBands = BAND_KA | BAND_K;
    liveV1.state.arrows = static_cast<Direction>(DIR_FRONT | DIR_REAR);
    liveV1.state.priorityArrow = DIR_FRONT;
    liveV1.state.signalBars = 6;
    liveV1.state.bogeyCounterChar = '2';
    liveV1.alerts = liveAlerts;
    liveV1.alertCount = 2;
    liveV1.priority = liveAlerts[0];
    liveV1.hasRenderablePriority = true;

    const RenderFrame v1LiveFrame = composer.compose(liveV1, AlpSnapshot{}, V1Settings{}, 5000);
    assertTraceSnapshot(
        v1LiveFrame,
        "kind=V1_LIVE owner=V1 cards=1 band=2>2/M arrows=5>5/M "
        "freq=34520>34520/M bogey=2>2/M mute=1>1/M bars=6>6/M");

    V1Snapshot alpOwnedV1 = liveV1;
    alpOwnedV1.state.muted = true;
    AlpSnapshot alp{};
    alp.ownsLaserDisplay = true;
    alp.event.active = true;
    alp.event.gun = AlpGunType::PL3_PROLITE;
    alp.event.direction = AlpLaserDirection::REAR;

    const RenderFrame alpLiveFrame = composer.compose(alpOwnedV1, alp, V1Settings{}, 5000);
    // ALP-owned laser synthesizes full deflection on the local 8-slot meter.
    assertTraceSnapshot(
        alpLiveFrame,
        "kind=ALP_LIVE owner=ALP cards=2 band=1>1/M arrows=4>4/M "
        "freq=0>0/NA bogey=2>2/M mute=1>0/NA bars=8>8/M");

    V1Snapshot persistedV1{};
    persistedV1.state = makeState();
    persistedV1.state.activeBands = BAND_NONE;
    persistedV1.state.arrows = DIR_NONE;
    persistedV1.state.priorityArrow = DIR_NONE;
    persistedV1.state.signalBars = 0;
    persistedV1.state.bogeyCounterChar = '1';
    persistedV1.state.muted = false;
    persistedV1.hasPersistedAlert = true;
    persistedV1.persistedAlert = makeAlert(BAND_KA, DIR_REAR, 35500);

    const RenderFrame persistedFrame =
        composer.compose(persistedV1, AlpSnapshot{}, V1Settings{}, 5000);
    assertTraceSnapshot(
        persistedFrame,
        "kind=V1_PERSISTED owner=V1 cards=0 band=2>2/M arrows=4>4/M "
        "freq=35500>35500/M bogey=1>1/M mute=0>0/NA bars=0>0/NA");

    V1Snapshot idleV1{};
    idleV1.state = makeState(BAND_NONE, DIR_NONE, 0);
    idleV1.state.bogeyCounterChar = '0';
    idleV1.state.muted = false;

    const RenderFrame idleFrame = composer.compose(idleV1, AlpSnapshot{}, V1Settings{}, 5000);
    assertTraceSnapshot(
        idleFrame,
        "kind=IDLE owner=IDLE cards=0 band=0>0/NA arrows=0>0/NA "
        "freq=0>0/NA bogey=0>0/M mute=0>0/NA bars=0>0/NA");
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_v1_live_trace_marks_primary_fields_as_matching);
    RUN_TEST(test_v1_live_trace_compares_signal_bars_to_frame_context);
    RUN_TEST(test_alp_live_trace_records_alp_ownership_and_unmuted_render);
    RUN_TEST(test_trace_log_is_bounded_and_copies_most_recent_first);
    RUN_TEST(test_trace_snapshots_cover_v1_alp_persisted_and_idle_contracts);
    return UNITY_END();
}
