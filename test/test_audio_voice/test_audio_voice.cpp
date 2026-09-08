// Exercise the real voice decision and complete clip composer through worker
// admission. The existing task fake records notification without running I2S.
#include <unity.h>
#include <filesystem>
#include <initializer_list>
#include <string>

#include "../mocks/settings.h"
#include "../mocks/storage_manager.h"
#include "../mocks/ble_client.h"
#include "../../include/audio_internals.h"
#include "../../src/modules/voice/voice_module.cpp"
#include "../../src/audio_voice.cpp"

SerialClass Serial;
unsigned long mockMillis = 10000;
unsigned long mockMicros = 0;
bool es8311_initialized = true;
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
void i2s_init() {}
AudioI2cResult set_speaker_amp(bool, TickType_t) { return AudioI2cResult::Ok; }
void audio_log_i2c_failure(const char*, AudioI2cResult) {}

static SettingsManager settings;
static fs::FS audioFiles{std::filesystem::path(PROJECT_DIR) / "data"};
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
        TEST_ASSERT_TRUE_MESSAGE(audioFiles.exists(path.c_str()), "Every selected clip must be shipped");
    }
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
}

void test_ku_priority_preserves_band_and_composes_true_frequency() {
    VoiceModule voice;
    voice.begin(&settings, nullptr);
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
    expectClips({"tens_13.mul", "digit_4.mul", "tens_50.mul", "dir_ahead.mul"});
}

void test_ku_frequency_only_keeps_edge_frequency_and_count() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AudioPlaybackResult::Accepted),
                         static_cast<int>(try_play_frequency_voice(AlertBand::KU, 13500, AlertDirection::BEHIND,
                                                                   VOICE_MODE_FREQ_ONLY, false, 2)));
    expectClips({"tens_13.mul", "digit_5.mul", "tens_00.mul", "digit_2.mul", "bogeys.mul"});
}

void test_ku_secondary_action_keeps_its_own_band_and_frequency() {
    VoiceModule voice;
    voice.begin(&settings, nullptr);
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
    expectClips({"tens_13.mul", "digit_4.mul", "tens_50.mul", "dir_behind.mul"});
}

void test_ku_band_only_preserves_requested_direction_and_count() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AudioPlaybackResult::Accepted),
                         static_cast<int>(try_play_frequency_voice(AlertBand::KU, 13450, AlertDirection::SIDE,
                                                                   VOICE_MODE_BAND_ONLY, true, 2)));
    expectClips({"dir_side.mul", "digit_2.mul", "bogeys.mul"});
}

void test_missing_ku_band_clip_does_not_admit_empty_worker_or_block_next_alert() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AudioPlaybackResult::Unavailable),
                         static_cast<int>(try_play_frequency_voice(AlertBand::KU, 13450, AlertDirection::AHEAD,
                                                                   VOICE_MODE_BAND_ONLY, false, 1)));
    play_band_only(AlertBand::KU);
    TEST_ASSERT_FALSE(audio_playing.load());
    TEST_ASSERT_EQUAL_UINT(0, g_mock_task_notify_state.giveCalls);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AudioPlaybackResult::Accepted),
                         static_cast<int>(try_play_frequency_voice(AlertBand::KA, 34749, AlertDirection::AHEAD,
                                                                   VOICE_MODE_BAND_FREQ, true, 1)));
    expectClips({"band_ka.mul", "tens_34.mul", "digit_7.mul", "tens_49.mul", "dir_ahead.mul"});
}

void test_simple_ku_alert_uses_only_available_direction() {
    play_alert_voice(AlertBand::KU, AlertDirection::BEHIND);
    expectClips({"dir_behind.mul"});
}

void test_ku_escalation_preserves_frequency_and_breakdown() {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AudioPlaybackResult::Accepted),
                         static_cast<int>(try_play_threat_escalation(AlertBand::KU, 13400, AlertDirection::SIDE,
                                                                     2, 1, 1, 0)));
    expectClips({"tens_13.mul", "digit_4.mul", "tens_00.mul", "dir_side.mul", "digit_2.mul", "bogeys.mul",
                 "digit_1.mul", "dir_ahead.mul", "digit_1.mul", "dir_behind.mul"});
}

void test_existing_band_frequency_mappings_are_unchanged() {
    TEST_ASSERT_EQUAL_INT(10, getGHz(AlertBand::X, 10525));
    TEST_ASSERT_EQUAL_INT(24, getGHz(AlertBand::K, 24150));
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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ku_priority_preserves_band_and_composes_true_frequency);
    RUN_TEST(test_ku_frequency_only_keeps_edge_frequency_and_count);
    RUN_TEST(test_ku_secondary_action_keeps_its_own_band_and_frequency);
    RUN_TEST(test_ku_band_only_preserves_requested_direction_and_count);
    RUN_TEST(test_missing_ku_band_clip_does_not_admit_empty_worker_or_block_next_alert);
    RUN_TEST(test_simple_ku_alert_uses_only_available_direction);
    RUN_TEST(test_ku_escalation_preserves_frequency_and_breakdown);
    RUN_TEST(test_existing_band_frequency_mappings_are_unchanged);
    return UNITY_END();
}
