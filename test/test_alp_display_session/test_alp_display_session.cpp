// ALP runtime -> normal display pipeline -> real display event setter.
// The renderer records the composed owner; no physical panel/audio is modeled.
#include <unity.h>
#include "../mocks/display_driver.h"
#include "../mocks/Arduino.h"
#include "../mocks/settings.h"
#include "../../src/display.h"
#include "../../src/modules/alp/alp_runtime_module.cpp"

SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
V1Display* g_displayInstance = nullptr;
V1Display::V1Display(SettingsManager& settings) : settings_(settings) { g_displayInstance = this; }
V1Display::~V1Display() = default;

#include "../mocks/ble_client.h"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"
#include "../../src/modules/voice/voice_module.cpp"
#include "../../src/display_indicators.cpp"
#include "../../src/modules/alert_persistence/alert_persistence_module.cpp"
#include "../../src/modules/alp/alp_event_latch.cpp"
#include "../../src/modules/display/render_frame_composer.cpp"
#include "../../src/modules/display/display_pipeline_module.cpp"

// These scenarios have no V1 radar or quiet owner. Unexpected speaker work is
// a test failure, rather than an accepted action hidden by a permissive stub.
AudioPlaybackResult try_play_frequency_voice(AlertBand, uint16_t, AlertDirection, VoiceAlertMode, bool, uint8_t) {
    TEST_FAIL_MESSAGE("unexpected V1 frequency voice");
    return AudioPlaybackResult::Unavailable;
}
AudioPlaybackResult try_play_direction_only(AlertDirection, uint8_t) {
    TEST_FAIL_MESSAGE("unexpected V1 direction voice");
    return AudioPlaybackResult::Unavailable;
}
AudioPlaybackResult try_play_threat_escalation(AlertBand, uint16_t, AlertDirection, uint8_t, uint8_t, uint8_t, uint8_t) {
    TEST_FAIL_MESSAGE("unexpected V1 escalation voice");
    return AudioPlaybackResult::Unavailable;
}
void QuietCoordinatorModule::syncCommittedState() { TEST_FAIL_MESSAGE("unexpected quiet owner"); }

static RenderFrame renderedFrame;
void V1Display::renderFrame(const RenderFrame& frame) { renderedFrame = frame; }
void V1Display::forceNextRedraw() {}
void V1Display::showScanning() {}
void V1Display::drawBLEProxyIndicator() {}
ObdRuntimeStatus ObdRuntimeModule::snapshot(uint32_t) const { return {}; }

void setUp() {
    mockMillis = 0;
    mockMicros = 0;
    renderedFrame = {};
}
void tearDown() { g_displayInstance = nullptr; }

namespace {
struct AlpDisplayRuntime {
    SettingsManager settings;
    V1Display display{settings};
    AlpRuntimeModule alp;
    PacketParser parser;
    V1BLEClient ble;
    AlertPersistenceModule persistence;
    VoiceModule voice;
    AlpEventLatch latch;
    DisplayMode mode = DisplayMode::IDLE;
    DisplayPipelineModule pipeline;

    AlpDisplayRuntime() {
        DisplayPipelineDependencies dependencies;
        dependencies.displayMode = &mode;
        dependencies.display = &display;
        dependencies.parser = &parser;
        dependencies.settings = &settings;
        dependencies.ble = &ble;
        dependencies.alertPersistence = &persistence;
        dependencies.voice = &voice;
        dependencies.alp = &alp;
        dependencies.alpLatch = &latch;
        pipeline.begin(dependencies);
        alp.begin(true);
    }

    void feed(uint32_t now, uint8_t first, uint8_t second, uint8_t third) {
        const uint8_t bytes[] = {first, second, third, alpChecksum(first, second, third)};
        mockMillis = now;
        alp.testInjectBytes(bytes, sizeof(bytes));
        alp.testSetLastUartByteMs(now); // Production drainUart records this arrival time.
        alp.process(now);
        pipeline.handleParsed(now);
    }

    void identifyThenTeardown() {
        feed(100, 0xB0, 0x03, 0); // Normal DLI listening heartbeat.
        feed(200, 0x98, 0, 0xE3); // LID trigger.
        feed(210, 0xC8, 0, 0xD5); // PL3 identification.
        TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, renderedFrame.primaryKind);
        TEST_ASSERT_EQUAL_STRING("PL3", display.ut_alpFreqText());
        feed(250, 0xD0, 0, 0xFD); // Register terminator starts identified teardown.
        TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alp.getState());
        for (uint32_t now = 1000; now <= 5000; now += 1000) {
            // Keep UART alive without reopening or extending the teardown.
            feed(now, 0xB0, 0x03, 0);
            TEST_ASSERT_TRUE(alp.currentEvent().active);
            TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, renderedFrame.primaryKind);
            TEST_ASSERT_EQUAL_STRING("PL3", display.ut_alpFreqText());
        }
    }
};
} // namespace

void test_new_unknown_session_clears_prior_gun_without_an_inactive_display_frame() {
    AlpDisplayRuntime runtime;
    runtime.identifyThenTeardown();
    const uint32_t previousGeneration = runtime.alp.currentEvent().sessionGeneration;

    // Timeout closes the previous session and the queued trigger opens its
    // successor in one process call, so the display sees active -> active.
    runtime.feed(5251, 0x98, 0, 0xE3);
    TEST_ASSERT_TRUE(runtime.alp.currentEvent().active);
    TEST_ASSERT_NOT_EQUAL(previousGeneration, runtime.alp.currentEvent().sessionGeneration);
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, runtime.alp.currentEvent().gun);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_LIVE, renderedFrame.primaryKind);
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, renderedFrame.alpPrimary.gun);
    TEST_ASSERT_TRUE(runtime.display.ut_alpHasLaserEvent());
    TEST_ASSERT_FALSE(runtime.display.ut_alpFreqOverride());
    TEST_ASSERT_EQUAL_STRING("", runtime.display.ut_alpFreqText());
}

void test_same_session_unknown_update_keeps_identified_gun() {
    AlpDisplayRuntime runtime;
    runtime.feed(100, 0xB0, 0x03, 0);
    runtime.feed(200, 0x98, 0, 0xE3);
    runtime.feed(210, 0xC8, 0, 0xD5);
    AlpLaserEvent sameSession = runtime.alp.currentEvent();
    sameSession.gun = AlpGunType::UNKNOWN;
    runtime.display.setAlpLaserEvent(sameSession);
    TEST_ASSERT_TRUE(runtime.display.ut_alpHasLaserEvent());
    TEST_ASSERT_TRUE(runtime.display.ut_alpFreqOverride());
    TEST_ASSERT_EQUAL_STRING("PL3", runtime.display.ut_alpFreqText());
}

void test_identified_teardown_keeps_gun_in_persisted_tail_then_clears() {
    AlpDisplayRuntime runtime;
    runtime.settings.alpAlertPersistSec = 3;
    runtime.identifyThenTeardown();
    runtime.feed(5251, 0xB0, 0x03, 0);
    TEST_ASSERT_FALSE(runtime.alp.currentEvent().active);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::ALP_PERSISTED, renderedFrame.primaryKind);
    TEST_ASSERT_FALSE(runtime.display.ut_alpHasLaserEvent());
    TEST_ASSERT_TRUE(runtime.display.ut_alpFreqOverride());
    TEST_ASSERT_EQUAL_STRING("PL3", runtime.display.ut_alpFreqText());

    runtime.feed(8251, 0xB0, 0x03, 0);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, renderedFrame.primaryKind);
    TEST_ASSERT_FALSE(runtime.display.ut_alpFreqOverride());
    TEST_ASSERT_EQUAL_STRING("", runtime.display.ut_alpFreqText());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_new_unknown_session_clears_prior_gun_without_an_inactive_display_frame);
    RUN_TEST(test_same_session_unknown_update_keeps_identified_gun);
    RUN_TEST(test_identified_teardown_keeps_gun_in_persisted_tail_then_clears);
    return UNITY_END();
}
