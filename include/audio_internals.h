#pragma once

#include "audio_beep.h"
#include "audio_i2c_utils.h"
#include <atomic>
#include <cstdint>
#include "driver/i2s_std.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

inline constexpr bool AUDIO_DEBUG_LOGS = false;

inline constexpr int AUDIO_CHUNK_SAMPLES = 1024;
inline constexpr int AUDIO_STEREO_CHUNK_SIZE = AUDIO_CHUNK_SAMPLES * 2;
inline constexpr int SD_AUDIO_TASK_STACK_SIZE = 4096;
inline constexpr int MAX_AUDIO_CLIPS = 12;
inline constexpr unsigned long AMP_WARM_TIMEOUT_MS = 3000;

struct SDAudioTaskParams {
    char filePaths[MAX_AUDIO_CLIPS][48];
    int numClips;
};

extern std::atomic<bool> es8311_initialized;
extern bool i2s_initialized;
extern i2s_chan_handle_t i2s_tx_chan;
extern std::atomic<TaskHandle_t> audioTaskHandle;

extern std::atomic<bool> audio_playing;
extern std::atomic<bool> amp_is_warm;
extern std::atomic<unsigned long> amp_last_used_ms;

// PSRAM is safe here: i2s_channel_write() copies into its internal DMA ring.
extern int16_t* g_stereoChunkBuffer;
extern uint8_t* g_mulawChunkBuffer;

extern SDAudioTaskParams g_sdAudioTaskParams;

// LittleFS reads disable the SPI cache, making PSRAM inaccessible; this task's
// stack must therefore reside in internal SRAM.
extern StackType_t g_sdAudioTaskStack[SD_AUDIO_TASK_STACK_SIZE];
extern StaticTask_t g_sdAudioTaskTCB;

bool es8311_init();
AudioI2cResult audio_apply_pending_volume(TickType_t timeoutTicks = pdMS_TO_TICKS(50));
void i2s_init();
AudioI2cResult set_speaker_amp(bool enable, TickType_t timeoutTicks = pdMS_TO_TICKS(50));
void audio_log_i2c_failure(const char* context, AudioI2cResult result);

int getGHz(AlertBand band, uint16_t freqMHz);
