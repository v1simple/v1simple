// Run the real accepted audio job, including codec/amp setup and PCM emission.
// Only the task scheduler, I2C/I2S hardware and filesystem are substituted.
#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <unity.h>
#include <vector>

// Include substitutes before production sources; this order is intentional.
#include "../mocks/settings.h"
#include "../mocks/storage_manager.h"
#include "../mocks/ble_client.h"
#include "../mocks/display.h"
#include "../../src/packet_parser.h"
#include "../../src/packet_parser.cpp"
#include "../../src/packet_parser_alerts.cpp"
#include "../../include/audio_internals.h"
#include "../../src/audio_i2c_utils.cpp"
#include "../../src/audio_beep.cpp"
#include "../../src/audio_voice.cpp"
#include "../../src/modules/voice/voice_module.cpp"
#include "../../src/modules/alert_persistence/alert_persistence_module.cpp"
#include "../../src/modules/alp/alp_event_latch.cpp"
#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/display/render_frame_composer.cpp"
#include "../../src/modules/display/display_pipeline_module.cpp"

SerialClass Serial;
unsigned long mockMillis = 10000;
unsigned long mockMicros = 0;
TwoWire tca9554Wire;
SemaphoreHandle_t tca9554WireMutex = reinterpret_cast<void *>(1);

static const std::filesystem::path audioRoot =
    std::filesystem::temp_directory_path() /
    ("v1simple_audio_worker_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
static fs::FS audioFiles{audioRoot};
static StorageManager audioStorage;
static int16_t stereoBuffer[AUDIO_STEREO_CHUNK_SIZE];
static uint8_t mulawBuffer[AUDIO_CHUNK_SAMPLES];
struct WorkerWaiting {};
static unsigned workerWaitCalls;

static uint32_t takeOneNotification(BaseType_t, TickType_t) {
    if (++workerWaitCalls > 1)
        throw WorkerWaiting{};
    return 1;
}

static void runAcceptedJob() {
    workerWaitCalls = 0;
    g_mock_task_notify_take_hook = takeOneNotification;
    try {
        sd_audio_playback_task(nullptr);
    } catch (const WorkerWaiting &) {
    }
    g_mock_task_notify_take_hook = nullptr;
    TEST_ASSERT_FALSE(audio_playing.load());
    TEST_ASSERT_NULL(audioTaskHandle.load());
    TEST_ASSERT_EQUAL_UINT(2, workerWaitCalls);
}

static void acceptBandJob() {
    TEST_ASSERT_EQUAL(
        AudioPlaybackResult::Accepted,
        try_play_frequency_voice(AlertBand::KA, 34700, AlertDirection::AHEAD, VOICE_MODE_BAND_ONLY, false));
}

static uint8_t lastVolumeRegister() {
    for (auto it = tca9554Wire.transmissionHistory.rbegin(); it != tca9554Wire.transmissionHistory.rend();
         ++it) {
        if (it->address == 0x18 && it->data.size() == 2 && it->data[0] == 0x32)
            return it->data[1];
    }
    TEST_FAIL_MESSAGE("No codec volume write");
    return 0;
}

void setUp() {
    mockMillis += 10000;
    tca9554Wire.resetMock();
    mock_reset_semaphore_state();
    mock_reset_task_notify_state();
    g_mock_i2s_state = MockI2sState{};
    i2s_tx_chan = reinterpret_cast<void *>(1);
    i2s_initialized = true;
    es8311_initialized = true;
    current_volume_percent.store(75);
    applied_volume_percent.store(-1);
    audio_set_volume(75);
    audio_playing = false;
    audioTaskHandle = nullptr;
    amp_is_warm = false;
    amp_last_used_ms = 0;
    amp_disable_fail_count = 0;
    g_stereoChunkBuffer = stereoBuffer;
    g_mulawChunkBuffer = mulawBuffer;
    audioStorage.setLittleFS(&audioFiles);
    audio_init_sd(audioStorage);
}
void tearDown() { TEST_ASSERT_TRUE(g_mock_semaphore_state.heldHandles.empty()); }

void test_failed_mute_is_applied_before_first_pcm_without_another_user_request() {
    mock_queue_semaphore_take_result(pdFALSE);
    audio_set_volume(0);
    TEST_ASSERT_EQUAL_INT(75, applied_volume_percent.load());
    g_mock_i2s_state.onWrite = [] { TEST_ASSERT_EQUAL_UINT8(0, lastVolumeRegister()); };
    acceptBandJob();
    runAcceptedJob();
    TEST_ASSERT_GREATER_THAN_UINT(0, g_mock_i2s_state.writeCalls);
    TEST_ASSERT_EQUAL_INT(0, applied_volume_percent.load());
}

void test_failed_volume_raise_is_applied_before_playback() {
    audio_set_volume(0);
    tca9554Wire.queueEndTransmission(4); // A write failure, not just mutex contention.
    audio_set_volume(100);
    TEST_ASSERT_EQUAL_INT(0, applied_volume_percent.load());
    g_mock_i2s_state.onWrite = [] { TEST_ASSERT_EQUAL_UINT8(0xBF, lastVolumeRegister()); };
    acceptBandJob();
    runAcceptedJob();
    TEST_ASSERT_GREATER_THAN_UINT(0, g_mock_i2s_state.writeCalls);
    TEST_ASSERT_EQUAL_INT(100, applied_volume_percent.load());
}

void test_periodic_service_retries_failed_volume_without_an_alert() {
    mock_queue_semaphore_take_result(pdFALSE);
    audio_set_volume(0);
    audio_process_amp_timeout();
    TEST_ASSERT_EQUAL_INT(0, applied_volume_percent.load());
    TEST_ASSERT_EQUAL_UINT8(0, lastVolumeRegister());
    TEST_ASSERT_EQUAL_UINT(0, g_mock_i2s_state.writeCalls);
}

void test_newer_mute_during_an_older_write_remains_pending() {
    bool inject = true;
    tca9554Wire.onEndTransmission = [&] {
        if (!inject)
            return;
        inject = false;
        // Model the loop publishing a new request while the worker owns I2C.
        mock_queue_semaphore_take_result(pdFALSE);
        audio_set_volume(0);
    };
    audio_set_volume(100);
    tca9554Wire.onEndTransmission = nullptr;
    TEST_ASSERT_EQUAL_INT(0, current_volume_percent.load());
    TEST_ASSERT_EQUAL_INT(100, applied_volume_percent.load());
    audio_process_amp_timeout();
    TEST_ASSERT_EQUAL_INT(0, applied_volume_percent.load());
    TEST_ASSERT_EQUAL_UINT8(0, lastVolumeRegister());
}

void test_failed_mid_clip_mute_is_retried_before_next_pcm_chunk() {
    g_mock_i2s_state.onWrite = [] {
        if (g_mock_i2s_state.writeCalls == 1) {
            mock_queue_semaphore_take_result(pdFALSE);
            audio_set_volume(0);
        } else {
            TEST_ASSERT_EQUAL_UINT8(0, lastVolumeRegister());
        }
    };
    acceptBandJob();
    runAcceptedJob();
    TEST_ASSERT_GREATER_THAN_UINT(1, g_mock_i2s_state.writeCalls);
    TEST_ASSERT_EQUAL_INT(0, applied_volume_percent.load());
}

void test_failed_mid_clip_volume_retry_stops_pcm_without_replaying_clip() {
    g_mock_i2s_state.onWrite = [] {
        mock_queue_semaphore_take_result(pdFALSE);
        audio_set_volume(0);
        mock_queue_semaphore_take_result(pdFALSE); // Worker cannot apply it either.
    };
    acceptBandJob();
    runAcceptedJob();
    TEST_ASSERT_EQUAL_UINT(1, g_mock_i2s_state.writeCalls);
    TEST_ASSERT_FALSE(amp_is_warm.load());
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
    g_mock_i2s_state.onWrite = nullptr;
    audio_process_amp_timeout();
    TEST_ASSERT_EQUAL_INT(0, applied_volume_percent.load());
}

void test_pending_volume_failure_exhausts_bounded_startup_without_pcm() {
    mock_queue_semaphore_take_result(pdFALSE);
    audio_set_volume(0);
    for (unsigned i = 0; i < 3; ++i)
        mock_queue_semaphore_take_result(pdFALSE);
    const auto takeCalls = g_mock_semaphore_state.takeCalls;
    acceptBandJob();
    runAcceptedJob();
    TEST_ASSERT_EQUAL_UINT(takeCalls + 3, g_mock_semaphore_state.takeCalls);
    TEST_ASSERT_EQUAL_UINT(0, g_mock_i2s_state.writeCalls);
    TEST_ASSERT_EQUAL_INT(75, applied_volume_percent.load());
}

void test_one_amp_failure_recovers_the_accepted_announcement_without_duplicate() {
    SettingsManager settings;
    VoiceModule voice;
    voice.begin(&settings);
    const AlertData alert = AlertData::create(BAND_KA, DIR_FRONT, 4, 0, 34700);
    VoiceContext context;
    context.alerts = &alert;
    context.alertCount = 1;
    context.priority = &alert;
    context.mainVolume = 5;
    context.now = mockMillis;
    const auto action = voice.prepareAction(context);
    TEST_ASSERT_TRUE(action.hasAction());
    TEST_ASSERT_EQUAL(
        AudioPlaybackResult::Accepted,
        try_play_frequency_voice(action.band, action.freq, action.dir, VOICE_MODE_BAND_ONLY, false));
    voice.commitAction(action, context.now);
    mock_queue_semaphore_take_result(pdFALSE); // First amp-enable attempt.
    runAcceptedJob();
    TEST_ASSERT_GREATER_THAN_UINT(0, g_mock_i2s_state.writeCalls);
    for (unsigned i = 0; i < 100; ++i) {
        context.now += 100;
        TEST_ASSERT_FALSE(voice.prepareAction(context).hasAction());
    }
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
}

void test_one_warm_amp_failure_recovers_without_resubmitting_job() {
    amp_is_warm = true;
    mock_queue_semaphore_take_result(pdFALSE);
    acceptBandJob();
    runAcceptedJob();
    TEST_ASSERT_GREATER_THAN_UINT(0, g_mock_i2s_state.writeCalls);
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
}

void test_one_codec_initialization_failure_recovers_before_pcm() {
    es8311_initialized = false;
    mock_queue_semaphore_take_result(pdFALSE);
    acceptBandJob();
    runAcceptedJob();
    TEST_ASSERT_TRUE(es8311_initialized.load());
    TEST_ASSERT_GREATER_THAN_UINT(0, g_mock_i2s_state.writeCalls);
}

void test_one_i2s_initialization_failure_recovers_before_pcm() {
    i2s_tx_chan = nullptr;
    i2s_initialized = false;
    g_mock_i2s_state.createResults.push_back(ESP_ERR_TIMEOUT);
    acceptBandJob();
    runAcceptedJob();
    TEST_ASSERT_EQUAL_UINT(2, g_mock_i2s_state.createCalls);
    TEST_ASSERT_GREATER_THAN_UINT(0, g_mock_i2s_state.writeCalls);
}

void test_permanent_amp_failure_releases_job_after_three_attempts() {
    for (unsigned i = 0; i < 3; ++i)
        mock_queue_semaphore_take_result(pdFALSE);
    const auto takeCalls = g_mock_semaphore_state.takeCalls;
    acceptBandJob();
    runAcceptedJob();
    TEST_ASSERT_EQUAL_UINT(takeCalls + 3, g_mock_semaphore_state.takeCalls);
    TEST_ASSERT_EQUAL_UINT(0, g_mock_i2s_state.writeCalls);
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
    // A later independent request can still use the recovered hardware.
    acceptBandJob();
    runAcceptedJob();
    TEST_ASSERT_GREATER_THAN_UINT(0, g_mock_i2s_state.writeCalls);
}

void test_missing_clip_is_not_retried_or_left_busy() {
    SDAudioTaskParams params{};
    params.numClips = 1;
    strcpy(params.filePaths[0], "/audio/missing.mul");
    TEST_ASSERT_EQUAL(AudioPlaybackResult::Accepted, start_sd_audio_task(params));
    runAcceptedJob();
    TEST_ASSERT_EQUAL_UINT(0, g_mock_i2s_state.writeCalls);
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
    TEST_ASSERT_EQUAL_UINT(1, g_mock_semaphore_state.takeCalls - 1); // One amp operation after setup volume.
}

void test_pcm_failure_does_not_retry_an_already_started_announcement() {
    g_mock_i2s_state.writeResults.push_back(ESP_ERR_TIMEOUT);
    acceptBandJob();
    runAcceptedJob();
    TEST_ASSERT_EQUAL_UINT(1, g_mock_i2s_state.writeCalls);
    TEST_ASSERT_FALSE(amp_is_warm.load());
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
}

// Exercise receipt ownership through the actual parser, pipeline and worker.
struct PipelineAudio {
    SettingsManager settings;
    PacketParser parser;
    V1BLEClient ble;
    V1Display display;
    AlertPersistenceModule persistence;
    VoiceModule voice;
    DisplayMode mode = DisplayMode::IDLE;
    DisplayPipelineModule pipeline;

    PipelineAudio() {
        settings.settings.voiceAlertMode = VOICE_MODE_BAND_ONLY;
        voice.begin(&settings);
        DisplayPipelineDependencies deps;
        deps.displayMode = &mode;
        deps.display = &display;
        deps.parser = &parser;
        deps.settings = &settings;
        deps.ble = &ble;
        deps.alertPersistence = &persistence;
        deps.voice = &voice;
        pipeline.begin(deps);
        packet(PACKET_ID_RESP_ALL_VOLUME, {6, 2, 6, 2});
    }
    void packet(uint8_t id, std::initializer_list<uint8_t> payload) {
        const uint8_t destination = id == PACKET_ID_ALERT_DATA ? 0xD8 : 0xD6;
        std::vector<uint8_t> frame{0xAA, destination, 0xEA, id, static_cast<uint8_t>(payload.size() + 1)};
        frame.insert(frame.end(), payload);
        uint8_t checksum = 0;
        for (uint8_t byte : frame)
            checksum = static_cast<uint8_t>(checksum + byte);
        frame.push_back(checksum);
        frame.push_back(0xAB);
        TEST_ASSERT_TRUE(parser.parse(frame.data(), frame.size()));
    }
    void alert(uint16_t frequency = 34700, uint8_t direction = 0x20) {
        packet(PACKET_ID_ALERT_DATA,
               {0x11, static_cast<uint8_t>(frequency >> 8), static_cast<uint8_t>(frequency), 0xAC, 0,
                static_cast<uint8_t>(direction | 0x02), 0x80});
        pipeline.handleParsed(mockMillis);
    }
    void clear() {
        packet(PACKET_ID_ALERT_DATA, {0, 0, 0, 0, 0, 0, 0});
        pipeline.handleParsed(mockMillis);
    }
};

void test_unchanged_alert_recovers_after_all_worker_startup_attempts_fail() {
    PipelineAudio runtime;
    runtime.alert();
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
    for (unsigned i = 0; i < 3; ++i)
        mock_queue_semaphore_take_result(pdFALSE);
    runAcceptedJob();
    TEST_ASSERT_EQUAL_UINT(0, g_mock_i2s_state.writeCalls);
    mockMillis += 100;
    runtime.alert(); // Consume failed receipt, arm retry.
    mockMillis += 99;
    runtime.alert();
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
    ++mockMillis;
    runtime.alert();
    TEST_ASSERT_EQUAL_UINT(2, g_mock_task_notify_state.giveCalls);
    runAcceptedJob();
    TEST_ASSERT_GREATER_THAN_UINT(0, g_mock_i2s_state.writeCalls);
    for (unsigned i = 0; i < 100; ++i) {
        mockMillis += 100;
        runtime.alert();
    }
    TEST_ASSERT_EQUAL_UINT(2, g_mock_task_notify_state.giveCalls);
}

void test_failed_startup_is_discarded_when_alert_ends() {
    PipelineAudio runtime;
    runtime.alert();
    for (unsigned i = 0; i < 3; ++i)
        mock_queue_semaphore_take_result(pdFALSE);
    runAcceptedJob();
    mockMillis += 100;
    runtime.clear();
    for (unsigned i = 0; i < 30; ++i) {
        mockMillis += 100;
        runtime.clear();
    }
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
    TEST_ASSERT_EQUAL_UINT(0, g_mock_i2s_state.writeCalls);
    runtime.alert(35500); // A new live episode is independently eligible.
    TEST_ASSERT_EQUAL_UINT(2, g_mock_task_notify_state.giveCalls);
    TEST_ASSERT_NOT_NULL(strstr(g_sdAudioTaskParams.filePaths[0], "band_ka"));
    runAcceptedJob();
}

void test_failed_startup_cannot_retry_replaced_priority() {
    PipelineAudio runtime;
    runtime.settings.settings.voiceAlertMode = VOICE_MODE_BAND_FREQ;
    runtime.alert(34700);
    for (unsigned i = 0; i < 3; ++i)
        mock_queue_semaphore_take_result(pdFALSE);
    runAcceptedJob();
    mockMillis += 100;
    runtime.alert(35500);
    TEST_ASSERT_EQUAL_UINT(2, g_mock_task_notify_state.giveCalls);
    TEST_ASSERT_NOT_NULL(strstr(g_sdAudioTaskParams.filePaths[1], "tens_35"));
    runAcceptedJob();
}

void test_partial_pcm_failure_does_not_replay_unchanged_alert() {
    PipelineAudio runtime;
    runtime.alert();
    g_mock_i2s_state.writeResults.push_back(ESP_OK);
    g_mock_i2s_state.writeResults.push_back(ESP_ERR_TIMEOUT);
    runAcceptedJob();
    TEST_ASSERT_EQUAL_UINT(2, g_mock_i2s_state.writeCalls);
    for (unsigned i = 0; i < 100; ++i) {
        mockMillis += 100;
        runtime.alert();
    }
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
}

void test_unrelated_test_voice_cannot_overwrite_failed_alert_receipt() {
    PipelineAudio runtime;
    runtime.alert();
    for (unsigned i = 0; i < 3; ++i)
        mock_queue_semaphore_take_result(pdFALSE);
    runAcceptedJob();
    play_test_voice(); // A different caller has no receipt for this pipeline.
    runAcceptedJob();
    mockMillis += 100;
    runtime.alert();
    mockMillis += 100;
    runtime.alert();
    TEST_ASSERT_EQUAL_UINT(3, g_mock_task_notify_state.giveCalls);
    runAcceptedJob();
}

void test_pending_worker_does_not_admit_duplicate_announcement() {
    PipelineAudio runtime;
    runtime.alert();
    for (unsigned i = 0; i < 10; ++i) {
        mockMillis += 100;
        runtime.alert();
    }
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
    runAcceptedJob();
    mockMillis += 100;
    runtime.alert();
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
}

void test_late_failure_receipt_cannot_consume_new_episode() {
    PipelineAudio runtime;
    runtime.alert();
    mockMillis += 100;
    runtime.clear(); // Old worker has not reported anything yet.
    for (unsigned i = 0; i < 3; ++i)
        mock_queue_semaphore_take_result(pdFALSE);
    runAcceptedJob();
    mockMillis += 100;
    runtime.alert(); // Identical identity in a new encounter.
    TEST_ASSERT_EQUAL_UINT(2, g_mock_task_notify_state.giveCalls);
    runAcceptedJob();
    for (unsigned i = 0; i < 100; ++i) {
        mockMillis += 100;
        runtime.alert();
    }
    TEST_ASSERT_EQUAL_UINT(2, g_mock_task_notify_state.giveCalls);
}

void test_missing_asset_receipt_does_not_retry_forever() {
    PipelineAudio runtime;
    runtime.alert();
    fs::FS missingFiles{audioRoot / "missing"};
    audioFS = &missingFiles;
    runAcceptedJob();
    TEST_ASSERT_EQUAL_UINT(0, g_mock_i2s_state.writeCalls);
    for (unsigned i = 0; i < 100; ++i) {
        mockMillis += 100;
        runtime.alert();
    }
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
    audio_init_sd(audioStorage);
}

void test_unheard_direction_retries_without_repeating_full_priority() {
    PipelineAudio runtime;
    runtime.settings.settings.voiceDirectionEnabled = true;
    runtime.alert();
    runAcceptedJob();
    mockMillis += 100;
    runtime.alert();
    mockMillis += 2000;
    runtime.alert(34700, 0x80);
    TEST_ASSERT_EQUAL_UINT(2, g_mock_task_notify_state.giveCalls);
    TEST_ASSERT_NOT_NULL(strstr(g_sdAudioTaskParams.filePaths[0], "dir_behind"));
    for (unsigned i = 0; i < 3; ++i)
        mock_queue_semaphore_take_result(pdFALSE);
    runAcceptedJob();
    mockMillis += 100;
    runtime.alert(34700, 0x80);
    mockMillis += 100;
    runtime.alert(34700, 0x80);
    TEST_ASSERT_EQUAL_UINT(3, g_mock_task_notify_state.giveCalls);
    TEST_ASSERT_NOT_NULL(strstr(g_sdAudioTaskParams.filePaths[0], "dir_behind"));
    runAcceptedJob();
    mockMillis += 3000;
    runtime.alert(34700, 0x80);
    TEST_ASSERT_EQUAL_UINT(3, g_mock_task_notify_state.giveCalls);
}

int main() {
    std::filesystem::copy(std::filesystem::path(PROJECT_DIR) / "tools" / "freq_audio" / "mulaw",
                          audioRoot / "audio", std::filesystem::copy_options::recursive);
    UNITY_BEGIN();
    RUN_TEST(test_unheard_direction_retries_without_repeating_full_priority);
    RUN_TEST(test_pending_worker_does_not_admit_duplicate_announcement);
    RUN_TEST(test_late_failure_receipt_cannot_consume_new_episode);
    RUN_TEST(test_missing_asset_receipt_does_not_retry_forever);
    RUN_TEST(test_unchanged_alert_recovers_after_all_worker_startup_attempts_fail);
    RUN_TEST(test_failed_startup_is_discarded_when_alert_ends);
    RUN_TEST(test_failed_startup_cannot_retry_replaced_priority);
    RUN_TEST(test_partial_pcm_failure_does_not_replay_unchanged_alert);
    RUN_TEST(test_unrelated_test_voice_cannot_overwrite_failed_alert_receipt);
    RUN_TEST(test_failed_mute_is_applied_before_first_pcm_without_another_user_request);
    RUN_TEST(test_failed_volume_raise_is_applied_before_playback);
    RUN_TEST(test_periodic_service_retries_failed_volume_without_an_alert);
    RUN_TEST(test_newer_mute_during_an_older_write_remains_pending);
    RUN_TEST(test_failed_mid_clip_mute_is_retried_before_next_pcm_chunk);
    RUN_TEST(test_failed_mid_clip_volume_retry_stops_pcm_without_replaying_clip);
    RUN_TEST(test_pending_volume_failure_exhausts_bounded_startup_without_pcm);
    RUN_TEST(test_one_amp_failure_recovers_the_accepted_announcement_without_duplicate);
    RUN_TEST(test_one_warm_amp_failure_recovers_without_resubmitting_job);
    RUN_TEST(test_one_codec_initialization_failure_recovers_before_pcm);
    RUN_TEST(test_one_i2s_initialization_failure_recovers_before_pcm);
    RUN_TEST(test_permanent_amp_failure_releases_job_after_three_attempts);
    RUN_TEST(test_missing_clip_is_not_retried_or_left_busy);
    RUN_TEST(test_pcm_failure_does_not_retry_an_already_started_announcement);
    const int result = UNITY_END();
    std::filesystem::remove_all(audioRoot);
    return result;
}
