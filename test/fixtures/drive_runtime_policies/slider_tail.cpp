#include <unity.h>

#include <cstdint>
#include <vector>

#include "test/mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

// Include the production parser before the queue's mock include path can
// substitute its test double. This suite proves the queue signal and parser
// recovery contract together.
#include "src/packet_parser.cpp"
#include "src/packet_parser.h"
#include "src/packet_parser_alerts.cpp"

#include "test/mocks/ble_client.h"
#include "test/mocks/modules/power/power_module.h"
#include "test/mocks/v1_profiles.h"

class DisplayPreviewModule {
  public:
    bool isRunning() const { return running; }
    void cancel() { running = false; }
    bool running = false;
};

#define private public
#include "src/modules/ble/ble_queue_module.h"
#undef private
#include "src/modules/ble/ble_queue_module.cpp"

namespace {

constexpr uint32_t kSession = 9;
constexpr uint16_t kCharacteristic = 0xB2CE;

BleQueueModule queue;
PacketParser parser;
V1BLEClient client;
V1ProfileManager profiles;
DisplayPreviewModule preview;
PowerModule power;

std::vector<uint8_t> makeAlertFrame(uint8_t index, uint8_t count,
                                    uint16_t frequency, uint8_t bandArrow,
                                    uint8_t aux0) {
    std::vector<uint8_t> frame{
        ESP_PACKET_START,
        0xD8,
        0xEA,
        PACKET_ID_ALERT_DATA,
        0x08,
        static_cast<uint8_t>(((index & 0x0F) << 4) | (count & 0x0F)),
        static_cast<uint8_t>(frequency >> 8),
        static_cast<uint8_t>(frequency),
        0x90,
        0x00,
        bandArrow,
        aux0,
    };
    uint8_t checksum = 0;
    for (uint8_t value : frame)
        checksum = static_cast<uint8_t>(checksum + value);
    frame.push_back(checksum);
    frame.push_back(ESP_PACKET_END);
    return frame;
}

void deliver(const std::vector<uint8_t> &frame, uint32_t ingressSequence,
             uint32_t nowMs) {
    TEST_ASSERT_TRUE(queue.tryOnNotify(frame.data(), frame.size(),
                                       kCharacteristic, kSession, nowMs,
                                       ingressSequence));
    queue.process();
}

} // namespace

void setUp() {
    queue.end();
    parser = PacketParser{};
    client.reset();
    profiles.reset();
    preview = DisplayPreviewModule{};
    power.reset();
    HealthCounters::reset();
    BleQueueModule::Config config;
    TEST_ASSERT_TRUE(
        queue.begin(&client, &parser, &profiles, &preview, &power, config));
    queue.openSession(kSession);
}

void tearDown() { queue.end(); }

#include "test/mocks/display.h"
#include "test/mocks/settings.h"
#include "src/modules/alert_persistence/alert_persistence_module.cpp"
#include "src/modules/alp/alp_event_latch.cpp"
#include "src/modules/quiet/quiet_coordinator_module.cpp"
#include "src/modules/voice/voice_module.cpp"
#include "src/modules/display/render_frame_composer.cpp"
#include "src/modules/display/display_pipeline_module.cpp"

#include "src/touch_handler.cpp"
#include "src/modules/touch/touch_ui_module.cpp"
#include "src/runtime_coordinator.h"
void audio_set_volume(uint8_t) {}
void play_test_voice() {}
// Admission is recorded here; real worker cancellation/cleanup is exercised
// by test_audio_worker and test_voice_suppression.
static AudioPlaybackReceipt* activeVoiceReceipt = nullptr;
static unsigned voiceAdmissions = 0;
AudioPlaybackResult try_play_frequency_voice(AlertBand, uint16_t, AlertDirection,
                                             VoiceAlertMode, bool, uint8_t, AudioPlaybackReceipt* receipt) {
    ++voiceAdmissions;
    activeVoiceReceipt = receipt;
    receipt->state.store(AudioPlaybackReceipt::State::Pending);
    return AudioPlaybackResult::Accepted;
}
AudioPlaybackResult try_play_direction_only(AlertDirection, uint8_t, AudioPlaybackReceipt*) {
    TEST_FAIL_MESSAGE("Unexpected direction-only admission");
    return AudioPlaybackResult::Unavailable;
}
AudioPlaybackResult try_play_threat_escalation(AlertBand, uint16_t, AlertDirection,
                                               uint8_t, uint8_t, uint8_t, uint8_t,
                                               VoiceAlertMode, bool, bool, AudioPlaybackReceipt*) {
    TEST_FAIL_MESSAGE("Unexpected escalation admission");
    return AudioPlaybackResult::Unavailable;
}
struct Runtime {
    V1Display display_;
    SettingsManager settings_;
    TouchHandler touch_;
    TouchUiModule touchUi_;
    PacketParser &parser_ = parser;
    VoiceModule voice_;
    AlertPersistenceModule persistence_;
    QuietCoordinatorModule quiet_;
    DisplayPipelineModule displayPipeline_;
    DisplayMode displayMode_ = DisplayMode::IDLE;
    struct {
        bool isActive() const { return false; }
    } autoPush_;
    struct {
        bool requiresExclusiveDetectorAdmission() const { return false; }
    } settingsOperations_;
    struct {
        bool ownsLaserDisplay() const { return false; }
        AlpLaserEvent currentEvent() const { return {}; }
    } alp_;
    bool boot = false, warningOwnsDisplay = false;
    unsigned drains = 0, presentations = 0;
    Runtime() {
        activeVoiceReceipt = nullptr;
        voiceAdmissions = 0;
        settings_.settings.voiceAlertMode = VOICE_MODE_BAND_ONLY;
        voice_.begin(&settings_);
        quiet_.begin(&client, &parser);
        DisplayPipelineDependencies dependencies;
        dependencies.displayMode = &displayMode_;
        dependencies.display = &display_;
        dependencies.parser = &parser;
        dependencies.settings = &settings_;
        dependencies.ble = &client;
        dependencies.alertPersistence = &persistence_;
        dependencies.voice = &voice_;
        dependencies.quiet = &quiet_;
        displayPipeline_.begin(dependencies);
        touch_.begin();
        TouchUiModule::Callbacks callbacks{};
        touchUi_.begin(&display_, &touch_, &settings_, callbacks);
    }
    bool active() { return true; }
    DriveLoopTiming beginDriveLoop() {
        return {0, static_cast<uint32_t>(mockMillis)};
    }
    struct Snapshot {
        bool connected = true, backpressured = false, overloaded = false,
             requestShowInitialScanning = false;
    };
    Snapshot processConnectionRuntime(uint32_t) { return {}; }
    void acceptConnectionSnapshot(Snapshot) {}
    void markInitialScanningScreenHandled() {}
    void showInitialScanningScreen() {}
    bool powerOwnsPresentation() { return warningOwnsDisplay; }
    void presentConnectionState(Snapshot) {}
    void processPower(uint32_t) {}
    bool processTouch(uint32_t now) { return touchUi_.process(now, boot); }
    void servicePowerDisplayOwnership(uint32_t) {}
    void processAlp(uint32_t) {}
    bool preemptSettingsForLiveAlert();
    bool hasLiveAlertPresentation() const;
    void openBootReadyGate(uint32_t) {}
    void processBleRuntime() {}
    void processBleQueue() {
        ++drains;
        queue.process();
    }
    bool bleQueueBackpressured() { return queue.isBackpressured(); }
    void processConnectionCycle(uint32_t, bool) {}
    void processObd(uint32_t, bool) {}
    void observeAlpProductState(uint32_t) {}
    void processAlpPresentationAndPower(uint32_t) {}
    void processGps(uint32_t) {}
    void processSpeed(uint32_t) {}
    void processSpeedAlert(uint32_t) {}
    void servicePowerAlertPresentation(uint32_t, bool) {}
    bool powerAlertPresentationNeedsRestore() { return false; }
    void processTapGesture(uint32_t) {}
    struct Edges {
        bool parsedReady;
    };
    Edges consumeDisplayEdges() { return {queue.consumeParsedFlag()}; }
    bool processParsedPolicies(Edges e) { return e.parsedReady; }
    void presentDisplay(Edges e, bool) {
        if (e.parsedReady)
            ++presentations;
    }
    DriveLoopDispatch processConnectionDispatch(bool) {
        return {static_cast<uint32_t>(mockMillis), true};
    }
    void processPeriodicMaintenance(uint32_t, bool, bool, bool, bool) {}
    BleQueueModule &bleQueue_ = queue;
    struct {
        uint32_t lastLoopUs = 0;
        bool bootSplashHoldActive = false;
    } state_;
    uint32_t finishLoop(bool, uint32_t, bool);
    void finishDriveLoop(bool, uint32_t, bool);
    void tick(uint32_t now) {
        mockMillis = now;
        DriveLoopCoordinator::tick(*this);
    }
};
#include "slider_tail.inc"

void test_fresh_v1_queue_drains_at_slider_tail_and_preempts_next_tick() {
    Runtime r;
    r.boot = true;
    r.tick(1000);
    r.boot = false;
    r.tick(1400);
    TEST_ASSERT_EQUAL_INT(1, r.display_.showSettingsSlidersCalls);
    const auto packet = makeAlertFrame(1, 1, 34700, 0x22, 0x80);
    TEST_ASSERT_TRUE(queue.tryOnNotify(packet.data(), packet.size(),
                                       kCharacteristic, kSession, 1500, 1));
    r.tick(1500);
    TEST_ASSERT_TRUE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT(0, uxQueueMessagesWaiting(queue.queueHandle_));
    TEST_ASSERT_EQUAL_INT(0, r.display_.hideBrightnessSliderCalls);
    TEST_ASSERT_EQUAL_UINT(0, r.presentations);
    r.tick(1525);
    TEST_ASSERT_EQUAL_INT(1, r.display_.hideBrightnessSliderCalls);
    TEST_ASSERT_EQUAL_UINT(1, r.presentations);
}
void test_normal_loop_drains_the_same_valid_notification() {
    Runtime r;
    const auto packet = makeAlertFrame(1, 1, 34700, 0x22, 0x80);
    TEST_ASSERT_TRUE(queue.tryOnNotify(packet.data(), packet.size(),
                                       kCharacteristic, kSession, 1500, 1));
    r.tick(1500);
    TEST_ASSERT_TRUE(parser.hasAlerts());
    TEST_ASSERT_EQUAL_UINT(1, r.presentations);
}
void test_already_parsed_alert_preempts_real_slider_owner() {
    Runtime r;
    r.boot = true;
    r.tick(1000);
    r.boot = false;
    r.tick(1400);
    deliver(makeAlertFrame(1, 1, 34700, 0x22, 0x80), 1, 1500);
    r.tick(1500);
    TEST_ASSERT_EQUAL_INT(1, r.display_.hideBrightnessSliderCalls);
    TEST_ASSERT_EQUAL_INT(1, r.settings_.requestDeferredPersistCalls);
    TEST_ASSERT_EQUAL_UINT(1, r.presentations);
}

static std::vector<uint8_t> canonicalFrame(uint8_t id, std::initializer_list<uint8_t> data) {
    std::vector<uint8_t> frame{0xAA, static_cast<uint8_t>(id == PACKET_ID_DISPLAY_DATA ? 0xD8 : 0xD6),
                               0xEA, id, static_cast<uint8_t>(data.size() + 1)};
    frame.insert(frame.end(), data);
    uint8_t sum = 0;
    for (const uint8_t byte : frame) sum = static_cast<uint8_t>(sum + byte);
    frame.push_back(sum);
    frame.push_back(0xAB);
    return frame;
}

static void acceptThenClearVoice(Runtime& runtime) {
    mockMillis = 900;
    const auto volume = canonicalFrame(PACKET_ID_RESP_ALL_VOLUME, {6, 2, 6, 2});
    TEST_ASSERT_TRUE(parser.parse(volume.data(), volume.size()));
    deliver(makeAlertFrame(1, 1, 34700, 0x22, 0x80), 1, 900);
    runtime.displayPipeline_.handleParsed(900);
    TEST_ASSERT_EQUAL_UINT(1, voiceAdmissions);
    TEST_ASSERT_NOT_NULL(activeVoiceReceipt);
    TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::Pending, activeVoiceReceipt->state.load());
    mockMillis = 950;
    deliver(makeAlertFrame(0, 0, 0, 0, 0), 2, 950);
    runtime.displayPipeline_.handleParsed(950);
    (void)queue.consumeParsedFlag();
    TEST_ASSERT_FALSE(parser.hasAlerts());
    TEST_ASSERT_TRUE(activeVoiceReceipt->isOwned());
}

static void openRuntimeSliders(Runtime& runtime) {
    runtime.boot = true;
    runtime.tick(1000);
    runtime.boot = false;
    runtime.tick(1400);
    TEST_ASSERT_EQUAL_INT(1, runtime.display_.showSettingsSlidersCalls);
}

void test_tail_soft_mute_cancels_pending_voice_after_clear_under_sliders() {
    Runtime runtime;
    acceptThenClearVoice(runtime);
    openRuntimeSliders(runtime);
    const auto mute = canonicalFrame(PACKET_ID_DISPLAY_DATA, {6, 6, 7, 0x22, 0x22, 0x0D, 0, 0x62});
    TEST_ASSERT_TRUE(queue.tryOnNotify(mute.data(), mute.size(), kCharacteristic, kSession, 1500, 3));
    TEST_ASSERT_FALSE(parser.getDisplayState().softMuted);
    runtime.tick(1500); // No prior parsed edge; the extracted forced tail drains mute.
    TEST_ASSERT_TRUE(parser.getDisplayState().softMuted);
    TEST_ASSERT_EQUAL_UINT(0, uxQueueMessagesWaiting(queue.queueHandle_));
    TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::CancelRequested, activeVoiceReceipt->state.load());
    TEST_ASSERT_EQUAL_UINT(1, voiceAdmissions);
    TEST_ASSERT_EQUAL_UINT(0, runtime.presentations);
    TEST_ASSERT_EQUAL_INT(0, runtime.display_.hideBrightnessSliderCalls);
}

void test_no_frame_proxy_takeover_cancels_cleared_voice_under_each_screen_owner() {
    for (const bool warning : {false, true}) {
        setUp();
        Runtime runtime;
        acceptThenClearVoice(runtime);
        if (warning) runtime.warningOwnsDisplay = true;
        else openRuntimeSliders(runtime);
        TEST_ASSERT_FALSE(queue.consumeParsedFlag());
        client.setProxyConnected(true);
        runtime.tick(1500); // No presentation callback can perform cancellation.
        TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::CancelRequested, activeVoiceReceipt->state.load());
        TEST_ASSERT_EQUAL_UINT(1, voiceAdmissions);
        TEST_ASSERT_EQUAL_UINT(0, runtime.presentations);
        tearDown();
    }
}

void test_tail_policy_service_never_admits_new_speech() {
    Runtime runtime;
    const auto alert = makeAlertFrame(1, 1, 34700, 0x22, 0x80);
    TEST_ASSERT_TRUE(queue.tryOnNotify(alert.data(), alert.size(), kCharacteristic, kSession, 1500, 1));
    mockMillis = 1500;
    runtime.finishLoop(false, 0, true);
    TEST_ASSERT_TRUE(parser.hasAlerts());
    runtime.displayPipeline_.servicePendingVoice();
    TEST_ASSERT_EQUAL_UINT(0, voiceAdmissions);
    TEST_ASSERT_NULL(activeVoiceReceipt);
    TEST_ASSERT_EQUAL_UINT(0, runtime.presentations);
}

void test_tail_refreshes_in_the_box_policy_before_pending_voice_service() {
    Runtime runtime;
    V1InTheBoxSettings boxes;
    boxes.bands[3].muteOutside = true;
    runtime.quiet_.setInTheBoxSettings(boxes, client.sessionGeneration());
    mockMillis = 1000;
    const auto volume = canonicalFrame(PACKET_ID_RESP_ALL_VOLUME, {6, 2, 6, 2});
    const auto display = canonicalFrame(PACKET_ID_DISPLAY_DATA, {6, 6, 7, 0x22, 0x22, 0x0C, 0, 0x62});
    TEST_ASSERT_TRUE(parser.parse(volume.data(), volume.size()));
    TEST_ASSERT_TRUE(parser.parse(display.data(), display.size()));
    deliver(makeAlertFrame(1, 1, 34700, 0x22, 0x80), 1, 1000);
    runtime.quiet_.processInTheBox(1000);
    TEST_ASSERT_FALSE(runtime.quiet_.inTheBoxMuteActive());
    runtime.displayPipeline_.handleParsed(1000);
    TEST_ASSERT_EQUAL_UINT(1, voiceAdmissions);
    TEST_ASSERT_TRUE(activeVoiceReceipt->isOwned());
    (void)queue.consumeParsedFlag();

    const auto outside = makeAlertFrame(1, 1, 35000, 0x22, 0x80);
    TEST_ASSERT_TRUE(queue.tryOnNotify(outside.data(), outside.size(), kCharacteristic, kSession, 1010, 2));
    mockMillis = 1010;
    // No other policy owner runs between queueing and this extracted tail.
    runtime.finishLoop(false, 0, true);
    TEST_ASSERT_FALSE(parser.getDisplayState().softMuted); // No detector echo yet.
    TEST_ASSERT_TRUE(runtime.quiet_.inTheBoxMuteActive());
    TEST_ASSERT_TRUE(client.lastMuteValue);
    TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::CancelRequested, activeVoiceReceipt->state.load());
    TEST_ASSERT_EQUAL_UINT(1, voiceAdmissions);
    TEST_ASSERT_EQUAL_UINT(0, runtime.presentations);
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_tail_soft_mute_cancels_pending_voice_after_clear_under_sliders);
    RUN_TEST(test_no_frame_proxy_takeover_cancels_cleared_voice_under_each_screen_owner);
    RUN_TEST(test_tail_policy_service_never_admits_new_speech);
    RUN_TEST(test_tail_refreshes_in_the_box_policy_before_pending_voice_service);
    RUN_TEST(test_fresh_v1_queue_drains_at_slider_tail_and_preempts_next_tick);
    RUN_TEST(test_normal_loop_drains_the_same_valid_notification);
    RUN_TEST(test_already_parsed_alert_preempts_real_slider_owner);
    return UNITY_END();
}
