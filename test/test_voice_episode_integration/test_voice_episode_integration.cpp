// Real BLE queue -> parser -> presentation pipeline -> voice decisions.
// Hardware transport, settings, panel and playback acceptance are substitutes.
#include <unity.h>

#include <initializer_list>
#include <vector>

#include "../mocks/Arduino.h"
#include "../mocks/ble_client.h"
#include "../mocks/settings.h"
#include "../mocks/display.h"
#include "../mocks/modules/power/power_module.h"
#include "../mocks/v1_profiles.h"

SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

// Include the real parser before any module can select its test double.
#include "../../src/packet_parser.h"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"
#include "../../src/modules/alert_persistence/alert_persistence_module.cpp"
#include "../../src/modules/alp/alp_event_latch.cpp"
#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/voice/voice_module.cpp"
#include "../../src/modules/display/render_frame_composer.cpp"
#include "../../src/modules/display/display_pipeline_module.cpp"

class DisplayPreviewModule {
  public:
    bool isRunning() const { return false; }
    void cancel() {}
};
#include "../../src/modules/ble/ble_queue_module.cpp"

namespace {
AudioPlaybackResult playbackResult = AudioPlaybackResult::Accepted;
std::vector<uint16_t> attemptedFrequencies;

struct VoiceRuntime {
    BleQueueModule queue;
    PacketParser parser;
    V1BLEClient ble;
    V1ProfileManager profiles;
    DisplayPreviewModule preview;
    PowerModule power;
    SettingsManager settings;
    V1Display display;
    AlertPersistenceModule persistence;
    VoiceModule voice;
    DisplayMode mode = DisplayMode::IDLE;
    DisplayPipelineModule pipeline;
    uint32_t ingressSequence = 0;

    VoiceRuntime() {
        settings.settings.voiceAlertMode = VOICE_MODE_BAND_FREQ;
        voice.begin(&settings);
        DisplayPipelineDependencies dependencies;
        dependencies.displayMode = &mode;
        dependencies.display = &display;
        dependencies.parser = &parser;
        dependencies.settings = &settings;
        dependencies.ble = &ble;
        dependencies.alertPersistence = &persistence;
        dependencies.voice = &voice;
        pipeline.begin(dependencies);
        TEST_ASSERT_TRUE(queue.begin(&ble, &parser, &profiles, &preview, &power));
        queue.openSession(9);
        enqueue(PACKET_ID_RESP_ALL_VOLUME, {6, 2, 6, 2}, 9000);
        present(9000);
    }

    ~VoiceRuntime() { queue.end(); }

    void enqueue(uint8_t id, std::initializer_list<uint8_t> payload, uint32_t nowMs) {
        const uint8_t destination = (id == PACKET_ID_ALERT_DATA || id == PACKET_ID_DISPLAY_DATA) ? 0xD8 : 0xD6;
        std::vector<uint8_t> frame{0xAA, destination, 0xEA, id, static_cast<uint8_t>(payload.size() + 1)};
        frame.insert(frame.end(), payload);
        uint8_t checksum = 0;
        for (uint8_t value : frame) checksum = static_cast<uint8_t>(checksum + value);
        frame.push_back(checksum);
        frame.push_back(0xAB);
        TEST_ASSERT_TRUE(queue.tryOnNotify(frame.data(), frame.size(), 0xB2CE, 9, nowMs, ++ingressSequence));
    }

    void alert(uint32_t nowMs, uint16_t frequency = 34700, bool secondary = false) {
        enqueue(PACKET_ID_ALERT_DATA,
                {static_cast<uint8_t>(secondary ? 0x12 : 0x11), static_cast<uint8_t>(frequency >> 8),
                 static_cast<uint8_t>(frequency), 0xAC, 0, 0x22, 0x80}, nowMs);
        if (secondary) {
            enqueue(PACKET_ID_ALERT_DATA, {0x22, 0x5E, 0x56, 0x90, 0, 0x24, 0}, nowMs);
        }
    }

    void clear(uint32_t nowMs) { enqueue(PACKET_ID_ALERT_DATA, {0, 0, 0, 0, 0, 0, 0}, nowMs); }

    void present(uint32_t nowMs) {
        mockMillis = nowMs;
        queue.process();
        TEST_ASSERT_TRUE(queue.consumeParsedFlag());
        pipeline.handleParsed(nowMs);
        TEST_ASSERT_EQUAL_UINT32(0, HealthCounters::inputDrops());
    }
};
} // namespace

AudioPlaybackResult try_play_frequency_voice(AlertBand, uint16_t frequency, AlertDirection,
                                             VoiceAlertMode, bool, uint8_t, AudioPlaybackReceipt* receipt) {
    if (receipt) receipt->state.store(AudioPlaybackReceipt::State::Started);
    attemptedFrequencies.push_back(frequency);
    return playbackResult;
}
AudioPlaybackResult try_play_direction_only(AlertDirection, uint8_t, AudioPlaybackReceipt* receipt) {
    if (receipt) receipt->state.store(AudioPlaybackReceipt::State::Started);
    TEST_FAIL_MESSAGE("unexpected direction-only announcement");
    return AudioPlaybackResult::Unavailable;
}
AudioPlaybackResult try_play_threat_escalation(AlertBand, uint16_t, AlertDirection,
                                               uint8_t, uint8_t, uint8_t, uint8_t, AudioPlaybackReceipt* receipt) {
    if (receipt) receipt->state.store(AudioPlaybackReceipt::State::Started);
    TEST_FAIL_MESSAGE("unexpected escalation announcement");
    return AudioPlaybackResult::Unavailable;
}

void setUp() {
    mockMillis = mockMicros = 0;
    playbackResult = AudioPlaybackResult::Accepted;
    attemptedFrequencies.clear();
    HealthCounters::reset();
}
void tearDown() {}

void test_coalesced_clear_rearms_same_frequency_once() {
    VoiceRuntime runtime;
    runtime.alert(10000);
    runtime.present(10000);
    TEST_ASSERT_EQUAL_UINT(1, attemptedFrequencies.size());

    runtime.clear(12000);
    runtime.alert(12100);
    runtime.present(12100); // Both transitions are parsed before presentation.
    TEST_ASSERT_EQUAL_UINT(2, attemptedFrequencies.size());
    TEST_ASSERT_EQUAL_UINT16(34700, attemptedFrequencies.back());
    for (uint32_t nowMs = 12200; nowMs <= 16000; nowMs += 100) {
        runtime.alert(nowMs);
        runtime.present(nowMs);
    }
    TEST_ASSERT_EQUAL_UINT(2, attemptedFrequencies.size());
}

void test_separately_presented_clear_also_rearms_inside_cooldown() {
    VoiceRuntime runtime;
    runtime.alert(10000);
    runtime.present(10000);
    runtime.clear(10100);
    runtime.present(10100);
    runtime.alert(10200);
    runtime.present(10200);
    TEST_ASSERT_EQUAL_UINT(2, attemptedFrequencies.size());
}

void test_continuing_encounter_preserves_dedup_and_frequency_change_cooldown() {
    VoiceRuntime runtime;
    runtime.alert(10000);
    runtime.present(10000);
    for (uint32_t nowMs = 10100; nowMs <= 13000; nowMs += 100) {
        runtime.alert(nowMs);
        runtime.present(nowMs);
    }
    TEST_ASSERT_EQUAL_UINT(1, attemptedFrequencies.size());

    runtime.alert(13100, 35500);
    runtime.present(13100);
    TEST_ASSERT_EQUAL_UINT(2, attemptedFrequencies.size());
    runtime.alert(13200, 33800);
    runtime.present(13200);
    runtime.alert(15099, 33800);
    runtime.present(15099);
    TEST_ASSERT_EQUAL_UINT(2, attemptedFrequencies.size());
    runtime.alert(15100, 33800);
    runtime.present(15100);
    TEST_ASSERT_EQUAL_UINT(3, attemptedFrequencies.size());
    TEST_ASSERT_EQUAL_UINT16(33800, attemptedFrequencies.back());
}

void test_pending_busy_retry_keeps_its_deadline_within_one_encounter() {
    VoiceRuntime runtime;
    playbackResult = AudioPlaybackResult::Busy;
    runtime.alert(10000);
    runtime.present(10000);
    playbackResult = AudioPlaybackResult::Accepted;
    runtime.alert(10099);
    runtime.present(10099);
    TEST_ASSERT_EQUAL_UINT(1, attemptedFrequencies.size());
    runtime.alert(10100);
    runtime.present(10100);
    TEST_ASSERT_EQUAL_UINT(2, attemptedFrequencies.size());
    runtime.alert(14000);
    runtime.present(14000);
    TEST_ASSERT_EQUAL_UINT(2, attemptedFrequencies.size());
}

void test_coalesced_clear_discards_old_pending_secondary_before_new_priority() {
    VoiceRuntime runtime;
    runtime.settings.settings.announceSecondaryAlerts = true;
    runtime.settings.settings.secondaryK = true;
    runtime.alert(10000, 34700, true);
    runtime.present(10000);
    playbackResult = AudioPlaybackResult::Busy;
    runtime.alert(12000, 34700, true);
    runtime.present(12000);
    TEST_ASSERT_EQUAL_UINT(2, attemptedFrequencies.size());
    TEST_ASSERT_EQUAL_UINT16(24150, attemptedFrequencies.back());

    runtime.clear(12010);
    runtime.alert(12020, 34700, true);
    playbackResult = AudioPlaybackResult::Accepted;
    runtime.present(12020);
    TEST_ASSERT_EQUAL_UINT(3, attemptedFrequencies.size());
    TEST_ASSERT_EQUAL_UINT16(34700, attemptedFrequencies.back());
    runtime.alert(12120, 34700, true);
    runtime.present(12120);
    TEST_ASSERT_EQUAL_UINT(3, attemptedFrequencies.size());
}

void test_new_encounter_does_not_inherit_unavailable_audio_retry_delay() {
    VoiceRuntime runtime;
    playbackResult = AudioPlaybackResult::Unavailable;
    runtime.alert(10000);
    runtime.present(10000);
    runtime.clear(10100);
    runtime.alert(10200);
    playbackResult = AudioPlaybackResult::Accepted;
    runtime.present(10200);
    TEST_ASSERT_EQUAL_UINT(2, attemptedFrequencies.size());
    runtime.alert(11000);
    runtime.present(11000);
    TEST_ASSERT_EQUAL_UINT(2, attemptedFrequencies.size());
}

void test_batched_completed_encounter_retains_priority_without_obsolete_voice() {
    VoiceRuntime runtime;
    runtime.settings.slotAlertPersistSec[0] = 3;
    runtime.alert(10000, 34700, true);
    runtime.clear(10100);
    runtime.present(11500);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_PERSISTED, runtime.display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL_UINT32(34700, runtime.display.lastRenderFrame.v1Priority.frequency);
    TEST_ASSERT_EQUAL_UINT(0, attemptedFrequencies.size());
    runtime.pipeline.refreshBlinkTick(13099);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_PERSISTED, runtime.display.lastRenderFrame.primaryKind);
    TEST_ASSERT_TRUE(runtime.pipeline.v1PersistenceRefreshDue(13100));
    runtime.pipeline.refreshBlinkTick(13100);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, runtime.display.lastRenderFrame.primaryKind);
    runtime.pipeline.handleParsed(13200);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, runtime.display.lastRenderFrame.primaryKind);
}

void test_delayed_completed_encounter_cannot_renew_expired_persistence() {
    VoiceRuntime runtime;
    runtime.settings.slotAlertPersistSec[0] = 3;
    runtime.alert(10000);
    runtime.clear(10100);
    runtime.present(13100);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, runtime.display.lastRenderFrame.primaryKind);
    TEST_ASSERT_FALSE(runtime.persistence.getPersistedAlert().isValid);
    TEST_ASSERT_EQUAL_UINT(0, attemptedFrequencies.size());
}

void test_completed_encounter_timestamp_wrap_and_zero_keep_original_expiry() {
    for (uint32_t clearedAt : {uint32_t{0}, UINT32_MAX - 999u}) {
        VoiceRuntime runtime;
        runtime.settings.slotAlertPersistSec[0] = 3;
        runtime.alert(clearedAt - 100u);
        runtime.clear(clearedAt);
        runtime.present(clearedAt + 2000u);
        TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_PERSISTED, runtime.display.lastRenderFrame.primaryKind);
        runtime.pipeline.refreshBlinkTick(clearedAt + 2999u);
        TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_PERSISTED, runtime.display.lastRenderFrame.primaryKind);
        runtime.pipeline.refreshBlinkTick(clearedAt + 3000u);
        TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, runtime.display.lastRenderFrame.primaryKind);
    }
}

void test_completed_encounter_does_not_replace_new_live_or_survive_session_reset() {
    VoiceRuntime runtime;
    runtime.settings.slotAlertPersistSec[0] = 3;
    runtime.alert(10000);
    runtime.clear(10100);
    runtime.alert(10200, 35500);
    runtime.present(10200);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_LIVE, runtime.display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL_UINT32(35500, runtime.display.lastRenderFrame.v1Priority.frequency);
    TEST_ASSERT_EQUAL_UINT(1, attemptedFrequencies.size());
    runtime.clear(10300);
    runtime.present(10300);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_PERSISTED, runtime.display.lastRenderFrame.primaryKind);
    runtime.queue.openSession(10);
    runtime.parser.resetAlertState(); // Connection owner resets parser session state.
    runtime.pipeline.handleParsed(10400);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, runtime.display.lastRenderFrame.primaryKind);
}

void test_slot_switch_cannot_resurrect_unpresented_or_expired_encounter() {
    VoiceRuntime runtime;
    runtime.settings.slotAlertPersistSec[0] = 0;
    runtime.settings.slotAlertPersistSec[1] = 3;
    runtime.alert(10000);
    runtime.clear(10100);
    runtime.present(10100);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, runtime.display.lastRenderFrame.primaryKind);
    runtime.settings.settings.activeSlot = 1;
    runtime.pipeline.handleParsed(10200);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::IDLE, runtime.display.lastRenderFrame.primaryKind);
}

void test_batched_display_laser_clear_retains_laser_without_radar_rows() {
    VoiceRuntime runtime;
    runtime.settings.slotAlertPersistSec[0] = 3;
    runtime.enqueue(PACKET_ID_DISPLAY_DATA, {0x49, 0, 0xFF, 0x21, 0, 0x0C, 0, 0x62}, 10000);
    runtime.enqueue(PACKET_ID_DISPLAY_DATA, {0x3F, 0, 0, 0, 0, 0x0C, 0, 0x62}, 10100);
    runtime.present(10100);
    TEST_ASSERT_EQUAL(RenderFramePrimaryKind::V1_PERSISTED, runtime.display.lastRenderFrame.primaryKind);
    TEST_ASSERT_EQUAL(BAND_LASER, runtime.display.lastRenderFrame.v1Priority.band);
    TEST_ASSERT_EQUAL_UINT(0, attemptedFrequencies.size());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_batched_display_laser_clear_retains_laser_without_radar_rows);
    RUN_TEST(test_batched_completed_encounter_retains_priority_without_obsolete_voice);
    RUN_TEST(test_delayed_completed_encounter_cannot_renew_expired_persistence);
    RUN_TEST(test_completed_encounter_timestamp_wrap_and_zero_keep_original_expiry);
    RUN_TEST(test_completed_encounter_does_not_replace_new_live_or_survive_session_reset);
    RUN_TEST(test_slot_switch_cannot_resurrect_unpresented_or_expired_encounter);
    RUN_TEST(test_coalesced_clear_rearms_same_frequency_once);
    RUN_TEST(test_separately_presented_clear_also_rearms_inside_cooldown);
    RUN_TEST(test_continuing_encounter_preserves_dedup_and_frequency_change_cooldown);
    RUN_TEST(test_pending_busy_retry_keeps_its_deadline_within_one_encounter);
    RUN_TEST(test_coalesced_clear_discards_old_pending_secondary_before_new_priority);
    RUN_TEST(test_new_encounter_does_not_inherit_unavailable_audio_retry_delay);
    return UNITY_END();
}
