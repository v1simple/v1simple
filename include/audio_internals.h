/**
 * Audio internals — shared across audio_beep TU split.
 *
 * Provides: shared state declarations (atomics, buffers, hardware handles),
 * promoted helper declarations, AUDIO_LOG macros, and constants.
 * Each companion .cpp includes this plus its own specific headers.
 */

#pragma once

#include "audio_beep.h"
#include "audio_i2c_utils.h"
#include "perf_metrics.h"
#include <atomic>
#include <cstdint>
#include "driver/i2s_std.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// --- Debug / logging infrastructure ---
inline constexpr bool AUDIO_DEBUG_LOGS = false;

#define AUDIO_LOGF(...) do { } while(0)
#define AUDIO_LOGLN(msg) do { } while(0)

// --- Shared constants ---
inline constexpr int AUDIO_CHUNK_SAMPLES = 1024;
inline constexpr int AUDIO_STEREO_CHUNK_SIZE = AUDIO_CHUNK_SAMPLES * 2;
inline constexpr int SD_AUDIO_TASK_STACK_SIZE = 4096;
inline constexpr int MAX_AUDIO_CLIPS = 12;
inline constexpr unsigned long AMP_WARM_TIMEOUT_MS = 3000;

// --- Shared struct for SD audio task params ---
// Unified type: used both as local preparation buffer and pre-allocated global.
struct SDAudioTaskParams {
    char filePaths[MAX_AUDIO_CLIPS][48];
    int numClips;
};

// --- Shared hardware state (defined in audio_beep.cpp) ---
extern bool es8311_initialized;
extern bool i2s_initialized;
extern i2s_chan_handle_t i2s_tx_chan;
extern std::atomic<TaskHandle_t> audioTaskHandle;

// --- Shared atomic state (defined in audio_beep.cpp) ---
extern std::atomic<bool> audio_playing;
extern std::atomic<bool> amp_is_warm;
extern std::atomic<unsigned long> amp_last_used_ms;

// --- Shared pre-allocated buffers (defined in audio_beep.cpp) ---
// Allocated in PSRAM via audio_init_buffers() to free ~5 KiB internal .bss.
// i2s_channel_write() copies from src to its internal DMA ring, so the
// source buffers can safely reside in PSRAM.
extern int16_t* g_stereoChunkBuffer;
extern uint8_t* g_mulawChunkBuffer;

// SD audio task pre-allocated global (defined in audio_beep.cpp)
extern SDAudioTaskParams g_sdAudioTaskParams;

// Static task allocation (defined in audio_beep.cpp)
// Stack MUST be internal SRAM — sd_audio task accesses LittleFS (SPI flash).
// SPI flash reads disable cache, making PSRAM inaccessible.
extern StackType_t g_sdAudioTaskStack[SD_AUDIO_TASK_STACK_SIZE];
extern StaticTask_t g_sdAudioTaskTCB;

// --- Promoted hardware helper declarations (defined in audio_beep.cpp) ---
bool es8311_init();
void i2s_init();
AudioI2cResult set_speaker_amp(bool enable, TickType_t timeoutTicks = pdMS_TO_TICKS(50));
void audio_log_i2c_failure(const char* context, AudioI2cResult result);

// --- Promoted pure function (defined in audio_voice.cpp) ---
int getGHz(AlertBand band, uint16_t freqMHz);

