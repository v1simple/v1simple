#pragma once

// Run the real accepted audio job, including codec/amp setup and PCM emission.
// Only the task scheduler, I2C/I2S hardware and filesystem are substituted.
#include <chrono>
#include <climits>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <string>
#include <unity.h>
#include <vector>

// A deterministic legal task-yield seam; no real scheduler or hardware is used.
#include "../mocks/freertos/FreeRTOS.h"
static std::function<void(TickType_t)> audioWorkerDelayHook;
static void audioWorkerDelay(TickType_t ticks) {
    if (audioWorkerDelayHook) audioWorkerDelayHook(ticks);
}
#define vTaskDelay audioWorkerDelay

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
#undef vTaskDelay

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
    audioWorkerDelayHook = nullptr;
    mockMillis += 10000;
    fs::mock_reset_fs_open_state();
    fs::mock_exists_uses_read_open(true); // Match ESP32 VFS::exists, including transient read faults.
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
void tearDown() {
    audioWorkerDelayHook = nullptr;
    fs::mock_reset_fs_open_state();
    TEST_ASSERT_TRUE(g_mock_semaphore_state.heldHandles.empty());
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
        const uint8_t destination =
            (id == PACKET_ID_ALERT_DATA || id == PACKET_ID_DISPLAY_DATA) ? 0xD8 : 0xD6;
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
