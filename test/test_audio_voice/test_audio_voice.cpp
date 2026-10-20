// Exercise the real voice decision and complete clip composer through worker
// admission. The existing task fake records notification without running I2S.
#include <unity.h>
#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <string>

#include "../mocks/settings.h"
#include "../mocks/storage_manager.h"
#include "../mocks/ble_client.h"
#include "../../src/packet_parser.h"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"
#include "../../include/audio_internals.h"
#include "../../src/modules/voice/voice_module.cpp"
#include "../../src/audio_voice.cpp"

SerialClass Serial;
unsigned long mockMillis = 10000;
unsigned long mockMicros = 0;
std::atomic<bool> es8311_initialized{true};
bool i2s_initialized = true;
i2s_chan_handle_t i2s_tx_chan = nullptr;
std::atomic<TaskHandle_t> audioTaskHandle{nullptr};
std::atomic<bool> audio_playing{false};
std::atomic<bool> amp_is_warm{false};
std::atomic<unsigned long> amp_last_used_ms{0};
int16_t* g_stereoChunkBuffer = nullptr;
uint8_t* g_mulawChunkBuffer = nullptr;
SDAudioTaskParams g_sdAudioTaskParams{};
StackType_t g_sdAudioTaskStack[SD_AUDIO_TASK_STACK_SIZE]{};
StaticTask_t g_sdAudioTaskTCB{};
bool es8311_init() { return true; }
AudioI2cResult audio_apply_pending_volume(TickType_t) { return AudioI2cResult::Ok; }
void i2s_init() {}
AudioI2cResult set_speaker_amp(bool, TickType_t) { return AudioI2cResult::Ok; }
void audio_log_i2c_failure(const char*, AudioI2cResult) {}

static SettingsManager settings;
static const std::filesystem::path audioRoot =
    std::filesystem::temp_directory_path() /
    ("v1simple_audio_voice_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
static fs::FS audioFiles{audioRoot};
static StorageManager audioStorage;

void setUp() {
    settings = SettingsManager{};
    audio_playing.store(false);
    audioTaskHandle.store(nullptr);
    g_sdAudioTaskParams = {};
    mock_reset_task_notify_state();
    audioStorage.setLittleFS(&audioFiles);
    audio_init_sd(audioStorage);
}
void tearDown() {}

static void expectClips(std::initializer_list<const char*> expected) {
    TEST_ASSERT_EQUAL_UINT(expected.size(), g_sdAudioTaskParams.numClips);
    int index = 0;
    for (const char* clip : expected) {
        const std::string path = std::string("/audio/") + clip;
        TEST_ASSERT_EQUAL_STRING(path.c_str(), g_sdAudioTaskParams.filePaths[index++]);
        TEST_ASSERT_TRUE_MESSAGE(audioFiles.exists(path.c_str()), "Every selected clip must exist in source assets");
    }
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
}

void test_ku_priority_preserves_band_and_composes_true_frequency() {
    VoiceModule voice;
    voice.begin(&settings);
    AlertData alert = AlertData::create(BAND_KU, DIR_FRONT, 4, 0, 13450);
    VoiceContext context;
    context.priority = &alert;
    context.alerts = &alert;
    context.alertCount = 1;
    context.mainVolume = 5;
    context.now = mockMillis;
    const VoiceAction action = voice.prepareAction(context);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_PRIORITY), static_cast<int>(action.type));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertBand::KU), static_cast<int>(action.band));
    TEST_ASSERT_EQUAL_UINT16(13450, action.freq);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AudioPlaybackResult::Accepted),
                         static_cast<int>(try_play_frequency_voice(action.band, action.freq, action.dir,
                                                                   VOICE_MODE_BAND_FREQ, true, 1)));
    expectClips({"band_ku.mul", "tens_13.mul", "digit_4.mul", "tens_50.mul", "dir_ahead.mul"});
}

void test_ku_frequency_only_keeps_edge_frequency_and_count() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AudioPlaybackResult::Accepted),
                         static_cast<int>(try_play_frequency_voice(AlertBand::KU, 13500, AlertDirection::BEHIND,
                                                                   VOICE_MODE_FREQ_ONLY, false, 2)));
    expectClips({"tens_13.mul", "digit_5.mul", "tens_00.mul", "digit_2.mul", "bogeys.mul"});
}

void test_ku_secondary_action_keeps_its_own_band_and_frequency() {
    VoiceModule voice;
    voice.begin(&settings);
    AlertData alerts[] = {AlertData::create(BAND_KA, DIR_FRONT, 5, 0, 34700),
                          AlertData::create(BAND_KU, DIR_REAR, 0, 3, 13450)};
    VoiceContext context;
    context.priority = &alerts[0];
    context.alerts = alerts;
    context.alertCount = 2;
    context.mainVolume = 5;
    context.now = mockMillis;
    const auto priority = voice.prepareAction(context);
    voice.commitAction(priority, context.now);
    context.now += 3000;
    const auto secondary = voice.prepareAction(context);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_SECONDARY), static_cast<int>(secondary.type));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertBand::KU), static_cast<int>(secondary.band));
    TEST_ASSERT_EQUAL_UINT16(13450, secondary.freq);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AudioPlaybackResult::Accepted),
                         static_cast<int>(try_play_frequency_voice(secondary.band, secondary.freq, secondary.dir,
                                                                   VOICE_MODE_BAND_FREQ, true, 1)));
    expectClips({"band_ku.mul", "tens_13.mul", "digit_4.mul", "tens_50.mul", "dir_behind.mul"});
}

void test_ku_band_only_preserves_requested_direction_and_count() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AudioPlaybackResult::Accepted),
                         static_cast<int>(try_play_frequency_voice(AlertBand::KU, 13450, AlertDirection::SIDE,
                                                                   VOICE_MODE_BAND_ONLY, true, 2)));
    expectClips({"band_ku.mul", "dir_side.mul", "digit_2.mul", "bogeys.mul"});
}

void test_ku_band_only_uses_named_clip() {
    play_band_only(AlertBand::KU);
    expectClips({"band_ku.mul"});
}

void test_simple_ku_alert_uses_band_and_direction() {
    play_alert_voice(AlertBand::KU, AlertDirection::BEHIND);
    expectClips({"band_ku.mul", "dir_behind.mul"});
}

void test_ku_escalation_preserves_frequency_and_breakdown() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AudioPlaybackResult::Accepted),
                         static_cast<int>(try_play_threat_escalation(AlertBand::KU, 13400, AlertDirection::SIDE,
                                                                     2, 1, 1, 0)));
    expectClips({"band_ku.mul", "tens_13.mul", "digit_4.mul", "tens_00.mul", "dir_side.mul", "digit_2.mul",
                 "bogeys.mul", "digit_1.mul", "dir_ahead.mul", "digit_1.mul", "dir_behind.mul"});
}

void test_photo_priority_says_photo_and_direction_without_frequency() {
    VoiceModule voice;
    voice.begin(&settings);
    AlertData alert = AlertData::create(BAND_K, DIR_FRONT, 4, 0, 24125);
    alert.photoType = 1;
    VoiceContext context;
    context.priority = &alert;
    context.alerts = &alert;
    context.alertCount = 1;
    context.mainVolume = 5;
    context.now = mockMillis;

    const VoiceAction action = voice.prepareAction(context);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(VoiceAction::Type::ANNOUNCE_PRIORITY), static_cast<int>(action.type));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertBand::PHOTO), static_cast<int>(action.band));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AudioPlaybackResult::Accepted),
                         static_cast<int>(try_play_frequency_voice(action.band, action.freq, action.dir,
                                                                   VOICE_MODE_BAND_FREQ, true, 1)));
    expectClips({"band_photo.mul", "dir_ahead.mul"});
}

void test_photo_frequency_only_still_says_photo_without_frequency_tokens() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AudioPlaybackResult::Accepted),
                         static_cast<int>(try_play_frequency_voice(AlertBand::PHOTO, 24125,
                                                                   AlertDirection::BEHIND, VOICE_MODE_FREQ_ONLY,
                                                                   true, 2)));
    expectClips({"band_photo.mul", "dir_behind.mul", "digit_2.mul", "bogeys.mul"});
}

void test_photo_escalation_omits_frequency_and_keeps_breakdown() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AudioPlaybackResult::Accepted),
                         static_cast<int>(try_play_threat_escalation(AlertBand::PHOTO, 24125,
                                                                     AlertDirection::SIDE, 2, 1, 1, 0)));
    expectClips({"band_photo.mul", "dir_side.mul", "digit_2.mul", "bogeys.mul", "digit_1.mul",
                 "dir_ahead.mul", "digit_1.mul", "dir_behind.mul"});
}

void test_existing_band_frequency_mappings_are_unchanged() {
    TEST_ASSERT_EQUAL_INT(10, getGHz(AlertBand::X, 10525));
    TEST_ASSERT_EQUAL_INT(24, getGHz(AlertBand::K, 24150));
    TEST_ASSERT_EQUAL_INT(0, getGHz(AlertBand::PHOTO, 24125));
    TEST_ASSERT_EQUAL_INT(33, getGHz(AlertBand::KA, 33800));
    TEST_ASSERT_EQUAL_INT(34, getGHz(AlertBand::KA, 34700));
    TEST_ASSERT_EQUAL_INT(35, getGHz(AlertBand::KA, 35500));
    TEST_ASSERT_EQUAL_INT(36, getGHz(AlertBand::KA, 36000));
    TEST_ASSERT_EQUAL_INT(0, getGHz(AlertBand::LASER, 0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AudioPlaybackResult::Accepted),
                         static_cast<int>(try_play_frequency_voice(AlertBand::K, 24150, AlertDirection::SIDE,
                                                                   VOICE_MODE_BAND_FREQ, true, 1)));
    expectClips({"band_k.mul", "tens_24.mul", "digit_1.mul", "tens_50.mul", "dir_side.mul"});
}

void test_parsed_unknown_direction_preserves_alert_without_side_clip() {
    PacketParser parser;
    std::vector<uint8_t> packet{0xAA, 0xD8, 0xEA, 0x43, 8, 0x11, 0x87, 0x8C, 0xAC, 0, 0x02, 0x80};
    uint8_t checksum = 0;
    for (uint8_t value : packet) checksum = static_cast<uint8_t>(checksum + value);
    packet.push_back(checksum);
    packet.push_back(0xAB);
    TEST_ASSERT_TRUE(parser.parse(packet.data(), packet.size(), mockMillis));
    AlertData priority;
    TEST_ASSERT_TRUE(parser.getRenderablePriorityAlert(priority));
    TEST_ASSERT_EQUAL(DIR_NONE, priority.direction);
    VoiceModule voice;
    voice.begin(&settings);
    VoiceContext context;
    context.alerts = parser.getAllAlerts().data();
    context.alertCount = parser.getAlertCount();
    context.priority = &priority;
    context.mainVolume = 5;
    context.now = mockMillis;
    const auto action = voice.prepareAction(context);
    TEST_ASSERT_EQUAL(VoiceAction::Type::ANNOUNCE_PRIORITY, action.type);
    TEST_ASSERT_EQUAL(AlertDirection::UNKNOWN, action.dir);
    TEST_ASSERT_EQUAL(AudioPlaybackResult::Accepted,
        try_play_frequency_voice(action.band, action.freq, action.dir, VOICE_MODE_BAND_FREQ, true, 1));
    expectClips({"band_ka.mul", "tens_34.mul", "digit_7.mul", "tens_00.mul"});
}

void test_unknown_laser_direction_keeps_band_announcement() {
    TEST_ASSERT_EQUAL(AudioPlaybackResult::Accepted,
        try_play_frequency_voice(AlertBand::LASER, 0, AlertDirection::UNKNOWN, VOICE_MODE_BAND_FREQ, true, 1));
    expectClips({"band_laser.mul"});
}

void test_unknown_simple_direction_keeps_band_announcement() {
    play_alert_voice(AlertBand::KA, AlertDirection::UNKNOWN);
    expectClips({"band_ka.mul"});
}

void test_unknown_escalation_direction_preserves_count_and_known_breakdown() {
    TEST_ASSERT_EQUAL(AudioPlaybackResult::Accepted,
        try_play_threat_escalation(AlertBand::K, 24125, AlertDirection::UNKNOWN, 2, 1, 0, 0));
    expectClips({"band_k.mul", "tens_24.mul", "digit_1.mul", "tens_25.mul", "digit_2.mul",
                 "bogeys.mul", "digit_1.mul", "dir_ahead.mul"});
}

void test_escalation_content_controls_are_independent() {
    struct Case {
        VoiceAlertMode mode;
        bool direction;
        bool count;
        std::initializer_list<const char*> clips;
    };
    const Case cases[] = {
        {VOICE_MODE_BAND_ONLY, false, false, {"band_ka.mul"}},
        {VOICE_MODE_BAND_ONLY, true, false, {"band_ka.mul", "dir_side.mul"}},
        {VOICE_MODE_BAND_ONLY, false, true, {"band_ka.mul", "digit_2.mul", "bogeys.mul"}},
        {VOICE_MODE_BAND_ONLY, true, true, {"band_ka.mul", "dir_side.mul", "digit_2.mul", "bogeys.mul",
                                          "digit_1.mul", "dir_ahead.mul", "digit_1.mul", "dir_behind.mul"}},
        {VOICE_MODE_FREQ_ONLY, false, false, {"tens_35.mul", "digit_5.mul", "tens_00.mul"}},
        {VOICE_MODE_FREQ_ONLY, true, false, {"tens_35.mul", "digit_5.mul", "tens_00.mul", "dir_side.mul"}},
        {VOICE_MODE_FREQ_ONLY, false, true, {"tens_35.mul", "digit_5.mul", "tens_00.mul", "digit_2.mul", "bogeys.mul"}},
        {VOICE_MODE_FREQ_ONLY, true, true, {"tens_35.mul", "digit_5.mul", "tens_00.mul", "dir_side.mul",
                                          "digit_2.mul", "bogeys.mul", "digit_1.mul", "dir_ahead.mul",
                                          "digit_1.mul", "dir_behind.mul"}},
        {VOICE_MODE_BAND_FREQ, false, false, {"band_ka.mul", "tens_35.mul", "digit_5.mul", "tens_00.mul"}},
        {VOICE_MODE_BAND_FREQ, true, false, {"band_ka.mul", "tens_35.mul", "digit_5.mul", "tens_00.mul", "dir_side.mul"}},
        {VOICE_MODE_BAND_FREQ, false, true, {"band_ka.mul", "tens_35.mul", "digit_5.mul", "tens_00.mul",
                                           "digit_2.mul", "bogeys.mul"}},
        {VOICE_MODE_BAND_FREQ, true, true, {"band_ka.mul", "tens_35.mul", "digit_5.mul", "tens_00.mul", "dir_side.mul",
                                          "digit_2.mul", "bogeys.mul", "digit_1.mul", "dir_ahead.mul",
                                          "digit_1.mul", "dir_behind.mul"}},
    };
    for (const auto& entry : cases) {
        audio_playing = false; // Complete the previous admitted job in this composer-only fixture.
        mock_reset_task_notify_state();
        TEST_ASSERT_EQUAL(AudioPlaybackResult::Accepted,
            try_play_threat_escalation(AlertBand::KA, 35500, AlertDirection::SIDE, 2, 1, 1, 0,
                                       entry.mode, entry.direction, entry.count));
        expectClips(entry.clips);
    }
}

void test_disabled_escalation_admits_no_job_even_with_context_enabled() {
    TEST_ASSERT_EQUAL(AudioPlaybackResult::Unavailable,
        try_play_threat_escalation(AlertBand::KA, 35500, AlertDirection::AHEAD, 2, 2, 0, 0,
                                   VOICE_MODE_DISABLED, true, true));
    TEST_ASSERT_FALSE(audio_playing.load());
    TEST_ASSERT_EQUAL_UINT(0, g_mock_task_notify_state.giveCalls);
}

void test_photo_escalation_respects_disabled_direction_and_count_in_frequency_mode() {
    TEST_ASSERT_EQUAL(AudioPlaybackResult::Accepted,
        try_play_threat_escalation(AlertBand::PHOTO, 24125, AlertDirection::AHEAD, 2, 2, 0, 0,
                                   VOICE_MODE_FREQ_ONLY, false, false));
    expectClips({"band_photo.mul"});
}

int main() {
    // Native tests run before web/LittleFS staging, including on a clean checkout.
    std::filesystem::copy(std::filesystem::path(PROJECT_DIR) / "tools" / "freq_audio" / "mulaw",
                          audioRoot / "audio", std::filesystem::copy_options::recursive);
    UNITY_BEGIN();
    RUN_TEST(test_escalation_content_controls_are_independent);
    RUN_TEST(test_disabled_escalation_admits_no_job_even_with_context_enabled);
    RUN_TEST(test_photo_escalation_respects_disabled_direction_and_count_in_frequency_mode);
    RUN_TEST(test_ku_priority_preserves_band_and_composes_true_frequency);
    RUN_TEST(test_ku_frequency_only_keeps_edge_frequency_and_count);
    RUN_TEST(test_ku_secondary_action_keeps_its_own_band_and_frequency);
    RUN_TEST(test_ku_band_only_preserves_requested_direction_and_count);
    RUN_TEST(test_ku_band_only_uses_named_clip);
    RUN_TEST(test_simple_ku_alert_uses_band_and_direction);
    RUN_TEST(test_ku_escalation_preserves_frequency_and_breakdown);
    RUN_TEST(test_photo_priority_says_photo_and_direction_without_frequency);
    RUN_TEST(test_photo_frequency_only_still_says_photo_without_frequency_tokens);
    RUN_TEST(test_photo_escalation_omits_frequency_and_keeps_breakdown);
    RUN_TEST(test_existing_band_frequency_mappings_are_unchanged);
    RUN_TEST(test_parsed_unknown_direction_preserves_alert_without_side_clip);
    RUN_TEST(test_unknown_laser_direction_keeps_band_announcement);
    RUN_TEST(test_unknown_simple_direction_keeps_band_announcement);
    RUN_TEST(test_unknown_escalation_direction_preserves_count_and_known_breakdown);
    const int result = UNITY_END();
    std::filesystem::remove_all(audioRoot);
    return result;
}
