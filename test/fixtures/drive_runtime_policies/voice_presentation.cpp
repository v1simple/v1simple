// Actual DriveRuntime parsed methods, real composition/voice owners, recorded
// worker admission and paint. This proves ordering/snapshot use, not sound.
#include <unity.h>
#include <vector>
#include "test/mocks/Arduino.h"
#include "test/mocks/ble_client.h"
#include "test/mocks/packet_parser.h"
#include "test/mocks/settings.h"
#include "test/mocks/display.h"
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#include "src/modules/quiet/quiet_coordinator_module.cpp"
#include "src/modules/voice/voice_module.cpp"
#include "src/modules/voice/voice_playback_coordinator.cpp"
#include "src/modules/voice/runtime_voice_observation.h"
#include "src/modules/alert_persistence/alert_persistence_module.cpp"
#include "src/modules/alp/alp_event_latch.cpp"
#include "src/modules/display/render_frame_composer.cpp"
#include "src/modules/display/display_pipeline_module.cpp"
#include "src/modules/display/display_orchestration_module.h"

static PacketParser parser;
static V1BLEClient ble;
static V1Display display;
static std::vector<int> order;
static unsigned attempts;
static bool changeInputsOnSubmit;
static AudioPlaybackReceipt* receipt;
static AlertData alert(uint16_t frequency) {
    AlertData row{};
    row.isValid = row.isPriority = true;
    row.band = BAND_K;
    row.direction = DIR_FRONT;
    row.frequency = frequency;
    row.frontStrength = 5;
    return row;
}
AudioPlaybackResult try_play_frequency_voice(AlertBand, uint16_t frequency, AlertDirection,
    VoiceAlertMode, bool, uint8_t, AudioPlaybackReceipt* result) {
    TEST_ASSERT_EQUAL_UINT16(24148, frequency);
    TEST_ASSERT_EQUAL_INT(0, display.renderFrameCalls);
    order.push_back(3);
    ++attempts;
    receipt = result;
    receipt->state.store(AudioPlaybackReceipt::State::Pending);
    if (changeInputsOnSubmit) {
        // Simulate admission work advancing time and parser inputs. Paint must
        // retain the already-composed row and receive the fresh entry clock.
        parser.setAlerts({alert(24250)});
        mockMillis = 1300;
    }
    return AudioPlaybackResult::Accepted;
}
AudioPlaybackResult try_play_direction_only(AlertDirection, uint8_t, AudioPlaybackReceipt*) {
    TEST_FAIL_MESSAGE("unexpected direction-only"); return AudioPlaybackResult::Unavailable;
}
AudioPlaybackResult try_play_threat_escalation(AlertBand, uint16_t, AlertDirection,
    uint8_t, uint8_t, uint8_t, uint8_t, VoiceAlertMode, bool, bool, AudioPlaybackReceipt*) {
    TEST_FAIL_MESSAGE("unexpected escalation"); return AudioPlaybackResult::Unavailable;
}

struct ObservedPipeline : DisplayPipelineModule {
    unsigned preparations = 0;
    uint32_t composedAt = 0, paintedAt = 0;
    bool expectCancellationBeforePrepare = false;
    bool prepareParsed(uint32_t now, RenderFrame& frame) {
        order.push_back(2);
        if (expectCancellationBeforePrepare)
            TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::CancelRequested, receipt->state.load());
        ++preparations;
        composedAt = now;
        return DisplayPipelineModule::prepareParsed(now, frame);
    }
    void presentPrepared(const RenderFrame& frame, uint32_t now) {
        order.push_back(4);
        paintedAt = now;
        DisplayPipelineModule::presentPrepared(frame, now);
    }
};
struct Runtime {
    PacketParser& parser_ = parser;
    V1BLEClient& ble_ = ble;
    SettingsManager settings_;
    VoiceModule voice_;
    QuietCoordinatorModule quiet_;
    VoicePlaybackCoordinator voicePlayback_{voice_, parser_, ble_, &quiet_};
    AlertPersistenceModule persistence_;
    DisplayMode mode_ = DisplayMode::IDLE;
    ObservedPipeline displayPipeline_;
    struct { bool bootSplashHoldActive = false; } state_;
    struct { bool isActive() const { return false; } } autoPush_;
    struct { bool requiresExclusiveDetectorAdmission() const { return false; } } settingsOperations_;
    struct {
        bool blink = false;
        bool processParsedFrame(const DisplayOrchestrationParsedContext& context) {
            order.push_back(1);
            return context.parsedReady && !context.bootSplashHoldActive;
        }
        bool processLightweightRefresh(const DisplayOrchestrationRefreshContext& context) {
            return blink && !context.pipelineRanThisLoop;
        }
    } displayOrchestration_;
    struct DisplayEdges { uint32_t nowMs; bool parsedReady; };
    Runtime() {
        settings_.settings.voiceAlertMode = VOICE_MODE_BAND_ONLY;
        voice_.begin(&settings_);
        quiet_.begin(&ble, &parser);
        voicePlayback_.begin(parser.alertLifetime());
        DisplayPipelineDependencies dependencies;
        dependencies.displayMode = &mode_;
        dependencies.display = &display;
        dependencies.parser = &parser;
        dependencies.settings = &settings_;
        dependencies.ble = &ble;
        dependencies.alertPersistence = &persistence_;
        displayPipeline_.begin(dependencies);
    }
    bool processParsedPolicies(const DisplayEdges&);
    void admitPreparedVoice(const RenderFrame&, uint32_t);
    bool presentParsedDisplay(uint32_t);
    void presentDisplay(const DisplayEdges&, bool);
};
#include "voice_presentation.inc"

void setUp() {
    parser.reset(); ble.reset(); display.reset(); order.clear();
    attempts = 0; changeInputsOnSubmit = false; receipt = nullptr;
    mockMillis = 1000; mockMicros = 0;
    ble.setConnected(true);
    parser.setMainVolume(6);
    parser.setAlerts({alert(24148)});
}
void tearDown() {}
void test_one_composition_feeds_voice_then_paint_with_fresh_clock() {
    Runtime runtime;
    changeInputsOnSubmit = true;
    runtime.presentDisplay({1000, true}, false);
    TEST_ASSERT_EQUAL_UINT(1, runtime.displayPipeline_.preparations);
    TEST_ASSERT_EQUAL_UINT32(1000, runtime.displayPipeline_.composedAt);
    TEST_ASSERT_EQUAL_UINT32(1300, runtime.displayPipeline_.paintedAt);
    TEST_ASSERT_EQUAL_UINT(1, attempts);
    TEST_ASSERT_EQUAL_INT(1, display.renderFrameCalls);
    TEST_ASSERT_EQUAL_UINT16(24148, display.lastRenderFrame.v1Priority.frequency);
    TEST_ASSERT_EQUAL_UINT16(24250, parser.getAllAlerts()[0].frequency);
    const int expected[]{1, 2, 3, 4};
    TEST_ASSERT_EQUAL_UINT(4, order.size());
    TEST_ASSERT_EQUAL_INT_ARRAY(expected, order.data(), 4);
}
void test_pending_cancellation_precedes_preparation_without_resubmission() {
    Runtime runtime;
    runtime.presentDisplay({1000, true}, false);
    parser.state.softMuted = true;
    runtime.displayPipeline_.expectCancellationBeforePrepare = true;
    runtime.presentDisplay({1001, true}, false);
    TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::CancelRequested, receipt->state.load());
    TEST_ASSERT_EQUAL_UINT(1, attempts);
    TEST_ASSERT_EQUAL_UINT(2, runtime.displayPipeline_.preparations);
}
void test_blink_and_owner_restore_never_admit_voice() {
    Runtime runtime;
    runtime.displayOrchestration_.blink = true;
    runtime.presentDisplay({1000, false}, false);
    TEST_ASSERT_TRUE(runtime.displayPipeline_.restoreCurrentOwner(1100));
    TEST_ASSERT_EQUAL_UINT(0, attempts);
    TEST_ASSERT_EQUAL_UINT(0, runtime.displayPipeline_.preparations);
    TEST_ASSERT_EQUAL_INT(2, display.renderFrameCalls);
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_one_composition_feeds_voice_then_paint_with_fresh_clock);
    RUN_TEST(test_pending_cancellation_precedes_preparation_without_resubmission);
    RUN_TEST(test_blink_and_owner_restore_never_admit_voice);
    return UNITY_END();
}
