// audio_voice.cpp — SD-based voice composition and clip playback
// Mu-law compressed clips from LittleFS, multi-clip concatenation,
// frequency/band/direction/bogey voice announcements.
// Hardware init and shared state live in audio_beep.cpp via audio_internals.h.

#include "audio_internals.h"
#include "audio_task_utils.h"
#include "storage_manager.h"
#include <Arduino.h>

// ============================================================================
// SD Card-based Frequency Voice Playback
// ============================================================================

static bool sd_audio_ready = false;
static const char* AUDIO_PATH = "/audio";
static fs::FS* audioFS = nullptr;  // Filesystem containing audio files
static uint8_t amp_disable_fail_count = 0;
static constexpr uint8_t AMP_DISABLE_MAX_RETRIES = 5;
static constexpr unsigned long AMP_TIMEOUT_CHECK_INTERVAL_MS = 100UL;

// Initialize filesystem audio system
// Audio files are stored in LittleFS (uploaded with firmware)
// This works regardless of whether SD card is the primary storage
void audio_init_sd() {
    sd_audio_ready = false;
    audioFS = nullptr;

    fs::FS* littlefs = storageManager.getLittleFS();
    if (!littlefs) {
        Serial.println("[AUDIO] LittleFS not available");
        return;
    }

    if (littlefs->exists(AUDIO_PATH)) {
        audioFS = littlefs;
        sd_audio_ready = true;
        Serial.println("[AUDIO] Frequency audio initialized (LittleFS)");
    } else {
        Serial.println("[AUDIO] Audio folder not found in LittleFS");
    }
}

// Mu-law decode table (8-bit compressed -> 16-bit linear PCM)
// This is the standard ITU-T G.711 mu-law expansion table
static const int16_t mulaw_decode_table[256] = {
    -32124,-31100,-30076,-29052,-28028,-27004,-25980,-24956,
    -23932,-22908,-21884,-20860,-19836,-18812,-17788,-16764,
    -15996,-15484,-14972,-14460,-13948,-13436,-12924,-12412,
    -11900,-11388,-10876,-10364, -9852, -9340, -8828, -8316,
     -7932, -7676, -7420, -7164, -6908, -6652, -6396, -6140,
     -5884, -5628, -5372, -5116, -4860, -4604, -4348, -4092,
     -3900, -3772, -3644, -3516, -3388, -3260, -3132, -3004,
     -2876, -2748, -2620, -2492, -2364, -2236, -2108, -1980,
     -1884, -1820, -1756, -1692, -1628, -1564, -1500, -1436,
     -1372, -1308, -1244, -1180, -1116, -1052,  -988,  -924,
      -876,  -844,  -812,  -780,  -748,  -716,  -684,  -652,
      -620,  -588,  -556,  -524,  -492,  -460,  -428,  -396,
      -372,  -356,  -340,  -324,  -308,  -292,  -276,  -260,
      -244,  -228,  -212,  -196,  -180,  -164,  -148,  -132,
      -120,  -112,  -104,   -96,   -88,   -80,   -72,   -64,
       -56,   -48,   -40,   -32,   -24,   -16,    -8,     0,
     32124, 31100, 30076, 29052, 28028, 27004, 25980, 24956,
     23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764,
     15996, 15484, 14972, 14460, 13948, 13436, 12924, 12412,
     11900, 11388, 10876, 10364,  9852,  9340,  8828,  8316,
      7932,  7676,  7420,  7164,  6908,  6652,  6396,  6140,
      5884,  5628,  5372,  5116,  4860,  4604,  4348,  4092,
      3900,  3772,  3644,  3516,  3388,  3260,  3132,  3004,
      2876,  2748,  2620,  2492,  2364,  2236,  2108,  1980,
      1884,  1820,  1756,  1692,  1628,  1564,  1500,  1436,
      1372,  1308,  1244,  1180,  1116,  1052,   988,   924,
       876,   844,   812,   780,   748,   716,   684,   652,
       620,   588,   556,   524,   492,   460,   428,   396,
       372,   356,   340,   324,   308,   292,   276,   260,
       244,   228,   212,   196,   180,   164,   148,   132,
       120,   112,   104,    96,    88,    80,    72,    64,
        56,    48,    40,    32,    24,    16,     8,     0
};

// Forward declaration for helper function
static void sd_audio_playback_task(void* pvParameters);

// Helper to start SD audio task with pre-allocated params
// Returns true if task started, false if already playing or failed
// Caller should prepare a local SDAudioTaskParams, then call this
static bool start_sd_audio_task(const SDAudioTaskParams& localParams) {
    // Atomic exchange: if already true, abort; otherwise set to true
    if (audio_playing.exchange(true)) {
        AUDIO_LOGLN("[AUDIO] Already playing, skipping");
        PERF_INC(audioPlayBusy);
        return false;
    }

    // Copy local params to pre-allocated global (protected by audio_playing flag)
    g_sdAudioTaskParams.numClips = localParams.numClips;
    for (int i = 0; i < localParams.numClips && i < 12; i++) {
        strncpy(g_sdAudioTaskParams.filePaths[i], localParams.filePaths[i], 47);
        g_sdAudioTaskParams.filePaths[i][47] = '\0';
    }

    // Use static task creation - stack is .bss (internal SRAM, always valid).
    // This avoids heap allocation failures when heap is low during alerts.
    // Stack MUST be internal (not PSRAM) because this task reads LittleFS.
    TaskHandle_t tempHandle = xTaskCreateStaticPinnedToCore(
        sd_audio_playback_task,
        "sd_audio",
        SD_AUDIO_TASK_STACK_SIZE,
        nullptr,  // Params passed via global struct
        1,        // Priority (low)
        g_sdAudioTaskStack,
        &g_sdAudioTaskTCB,
        1         // Core 1
    );
    audioTaskHandle.store(tempHandle);

    if (audioTaskHandle.load() == nullptr) {
        Serial.println("[AUDIO] ERROR: Failed to create SD audio task!");
        PERF_INC(audioTaskFail);
        audio_playing = false;
        return false;
    }
    PERF_INC(audioPlayCount);
    return true;
}

// Background task for SD audio concatenation playback (mu-law compressed)
// Uses pre-allocated buffers - no malloc in task
static void sd_audio_playback_task(void* pvParameters) {
    (void)pvParameters;  // Unused - params are in g_sdAudioTaskParams

    if (i2s_tx_chan == nullptr) {
        i2s_init();
        vTaskDelay(pdMS_TO_TICKS(30));  // Reduced from 50ms
    }

    if (!i2s_initialized) {
        Serial.println("[AUDIO] ERROR: I2S init failed!");
        audioResetTaskState(audio_playing, audioTaskHandle);
        vTaskDelete(nullptr);
        return;
    }

    if (!es8311_init()) {
        audioResetTaskState(audio_playing, audioTaskHandle);
        vTaskDelete(nullptr);
        return;
    }

    // Amp warm-keeping: skip stabilization delay if amp is already warm
    if (!amp_is_warm) {
        vTaskDelay(pdMS_TO_TICKS(20));  // ES8311 lock time
        const AudioI2cResult ampEnableResult = set_speaker_amp(true);
        if (ampEnableResult != AudioI2cResult::Ok) {
            audio_log_i2c_failure("sd_audio_playback_task amp enable (cold)", ampEnableResult);
            audioResetTaskState(audio_playing, audioTaskHandle);
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));  // Amp stabilization (only on cold start)
        amp_is_warm = true;
        AUDIO_LOGLN("[AUDIO] Amp cold start - full init");
    } else {
        // Amp already warm - just ensure it's on (no delay needed)
        const AudioI2cResult ampEnableResult = set_speaker_amp(true);
        if (ampEnableResult != AudioI2cResult::Ok) {
            audio_log_i2c_failure("sd_audio_playback_task amp enable (warm)", ampEnableResult);
            audioResetTaskState(audio_playing, audioTaskHandle);
            vTaskDelete(nullptr);
            return;
        }
        AUDIO_LOGLN("[AUDIO] Amp warm - skipping stabilization");
    }

    // Use audioFS (LittleFS) which contains the audio files
    if (!audioFS) {
        Serial.println("[AUDIO] ERROR: audioFS is null!");
        // Don't disable amp here - let timeout handle it
        audioResetTaskState(audio_playing, audioTaskHandle);
        vTaskDelete(nullptr);
        return;
    }

    // Play each clip in sequence using pre-allocated PSRAM buffers
    if (!g_stereoChunkBuffer || !g_mulawChunkBuffer) {
        Serial.println("[AUDIO] ERROR: PSRAM buffers not allocated!");
        audioResetTaskState(audio_playing, audioTaskHandle);
        vTaskDelete(nullptr);
        return;
    }
    bool writeAborted = false;
    for (int i = 0; i < g_sdAudioTaskParams.numClips; i++) {
        File audioFile = audioFS->open(g_sdAudioTaskParams.filePaths[i], "r");
        if (!audioFile) {
            AUDIO_LOGF("[AUDIO] Failed to open: %s\n", g_sdAudioTaskParams.filePaths[i]);
            continue;
        }

        // Use pre-allocated buffers (no malloc needed)
        // g_mulawChunkBuffer: AUDIO_CHUNK_SAMPLES bytes for mu-law data
        // g_stereoChunkBuffer: AUDIO_CHUNK_SAMPLES*2 int16_t for stereo output
        size_t bytesRead;
        while ((bytesRead = audioFile.read(g_mulawChunkBuffer, AUDIO_CHUNK_SAMPLES)) > 0) {
            // Decode mu-law to stereo PCM using pre-allocated buffer
            for (size_t j = 0; j < bytesRead; j++) {
                int16_t sample = mulaw_decode_table[g_mulawChunkBuffer[j]];
                g_stereoChunkBuffer[j * 2] = sample;       // Left
                g_stereoChunkBuffer[j * 2 + 1] = sample;   // Right
            }

            size_t bytes_written = 0;
            const AudioWriteResult writeResult = audioWriteWithTimeout([&](TickType_t timeoutTicks) {
                return i2s_channel_write(i2s_tx_chan,
                                         g_stereoChunkBuffer,
                                         bytesRead * 2 * sizeof(int16_t),
                                         &bytes_written,
                                         timeoutTicks);
            });
            if (writeResult.status != AudioWriteStatus::Ok) {
                AUDIO_LOGF("[AUDIO] i2s_channel_write %s: %d\n",
                           writeResult.status == AudioWriteStatus::Timeout ? "timed out" : "failed",
                           writeResult.error);
                writeAborted = true;
                break;
            }
        }

        audioFile.close();
        if (writeAborted) {
            break;
        }
    }

    if (writeAborted) {
        const AudioI2cResult ampDisableResult = set_speaker_amp(false);
        if (ampDisableResult != AudioI2cResult::Ok) {
            audio_log_i2c_failure("sd_audio_playback_task amp disable", ampDisableResult);
            amp_disable_fail_count++;
            if (amp_disable_fail_count >= AMP_DISABLE_MAX_RETRIES) {
                Serial.println("[AUDIO] ERROR: Amp disable failed after max retries — giving up");
                amp_is_warm = false;
                amp_disable_fail_count = 0;
            } else {
                amp_is_warm = true;
                amp_last_used_ms = millis();
            }
        } else {
            amp_is_warm = false;
            amp_disable_fail_count = 0;
        }
    } else {
        // Brief delay for DMA buffer to flush
        vTaskDelay(pdMS_TO_TICKS(30));  // Reduced from 50ms

        // Don't disable amp immediately - keep it warm for faster subsequent plays
        // Record when we finished so timeout can disable it later
        amp_last_used_ms = millis();
        // Amp stays on - will be disabled by audio_process_amp_timeout() after AMP_WARM_TIMEOUT_MS
    }

    audioResetTaskState(audio_playing, audioTaskHandle);
    vTaskDelete(nullptr);
}

// Get GHz value for band and frequency
int getGHz(AlertBand band, uint16_t freqMHz) {
    switch (band) {
        case AlertBand::KA:
            // Ka band: 33.4-36.0 GHz - determine which integer GHz
            if (freqMHz < 34000) return 33;
            if (freqMHz < 35000) return 34;
            if (freqMHz < 36000) return 35;
            return 36;
        case AlertBand::K:
            return 24;  // K band is 24.x GHz
        case AlertBand::X:
            return 10;  // X band is 10.x GHz
        default:
            return 0;   // Laser has no frequency
    }
}

static const char* getBandClipFile(AlertBand band) {
    switch (band) {
        case AlertBand::LASER: return "band_laser.mul";
        case AlertBand::KA:    return "band_ka.mul";
        case AlertBand::K:     return "band_k.mul";
        case AlertBand::X:     return "band_x.mul";
    }
    return nullptr;
}

static const char* getDirectionClipFile(AlertDirection direction) {
    switch (direction) {
        case AlertDirection::AHEAD:  return "dir_ahead.mul";
        case AlertDirection::BEHIND: return "dir_behind.mul";
        case AlertDirection::SIDE:   return "dir_side.mul";
    }
    return nullptr;
}

static bool appendAudioClip(SDAudioTaskParams& params, const char* clipFile) {
    if (!clipFile || params.numClips >= MAX_AUDIO_CLIPS) {
        return false;
    }
    snprintf(params.filePaths[params.numClips++], 48, "%s/%s", AUDIO_PATH, clipFile);
    return true;
}

// Play voice alert for band/direction (non-blocking)
void play_alert_voice(AlertBand band, AlertDirection direction) {
    AUDIO_LOGF("[AUDIO] play_alert_voice() band=%d dir=%d\n", (int)band, (int)direction);

    if (audio_playing.load()) {
        AUDIO_LOGLN("[AUDIO] Already playing, skipping");
        PERF_INC(audioPlayBusy);
        return;
    }

    if (!sd_audio_ready) {
        AUDIO_LOGLN("[AUDIO] LittleFS audio not ready, skipping simple alert");
        return;
    }

    SDAudioTaskParams params;
    params.numClips = 0;
    const char* bandFile = getBandClipFile(band);
    const char* dirFile = getDirectionClipFile(direction);
    if (!bandFile || !dirFile) {
        return;
    }
    appendAudioClip(params, bandFile);
    appendAudioClip(params, dirFile);

    if (params.numClips == 0) {
        return;
    }

    AUDIO_LOGF("[AUDIO] Playing simple alert: %d clips\n", params.numClips);
    start_sd_audio_task(params);
}

// Play test voice for volume adjustment (short "Ka ahead" composed clip)
void play_test_voice() {
    AUDIO_LOGLN("[AUDIO] play_test_voice() called");
    play_alert_voice(AlertBand::KA, AlertDirection::AHEAD);
}

// Play frequency voice announcement from SD card
// Format depends on mode:
//   BAND_ONLY: "Ka"
//   FREQ_ONLY: "34 7 49"
//   BAND_FREQ: "Ka 34 7 49"
// direction appended if includeDirection is true
// bogeyCount appended if > 1: "2 bogeys", "3 bogeys", etc.
void play_frequency_voice(AlertBand band, uint16_t freqMHz, AlertDirection direction,
                          VoiceAlertMode mode, bool includeDirection, uint8_t bogeyCount) {
    AUDIO_LOGF("[AUDIO] play_frequency_voice() band=%d freq=%d dir=%d mode=%d incDir=%d bogeys=%d\n",
               (int)band, freqMHz, (int)direction, (int)mode, includeDirection, bogeyCount);

    if (audio_playing.load()) {
        AUDIO_LOGLN("[AUDIO] Already playing, skipping");
        PERF_INC(audioPlayBusy);
        return;
    }

    if (mode == VOICE_MODE_DISABLED) {
        AUDIO_LOGLN("[AUDIO] Voice alerts disabled");
        return;
    }

    if (!sd_audio_ready) {
        AUDIO_LOGLN("[AUDIO] LittleFS audio not ready, skipping frequency voice");
        return;
    }

    // Laser doesn't have frequency - always include direction if enabled
    // Since there's no frequency to announce, direction is especially important
    if (band == AlertBand::LASER) {
        if (includeDirection) {
            // Compose "Laser ahead/behind/side" from LittleFS clips.
            play_alert_voice(band, direction);
        } else {
            // Just say "Laser" without direction
            play_band_only(band);
        }
        return;
    }

    // Prepare params on stack (no malloc needed)
    SDAudioTaskParams params;
    params.numClips = 0;

    // 1. Band clip (if mode includes band)
    if (mode == VOICE_MODE_BAND_ONLY || mode == VOICE_MODE_BAND_FREQ) {
        const char* bandFile = nullptr;
        switch (band) {
            case AlertBand::KA: bandFile = "band_ka.mul"; break;
            case AlertBand::K:  bandFile = "band_k.mul"; break;
            case AlertBand::X:  bandFile = "band_x.mul"; break;
            default: break;
        }
        if (bandFile) {
            snprintf(params.filePaths[params.numClips++], 48, "%s/%s", AUDIO_PATH, bandFile);
        }
    }

    // 2-4. Frequency clips (if mode includes frequency)
    if (mode == VOICE_MODE_FREQ_ONLY || mode == VOICE_MODE_BAND_FREQ) {
        // GHz token reuses two-digit number clips (e.g., "thirty four")
        int ghz = getGHz(band, freqMHz);
        if (ghz > 0) {
            snprintf(params.filePaths[params.numClips++], 48, "%s/tens_%02d.mul", AUDIO_PATH, ghz);
        }

        // Hundreds digit of MHz (first digit after decimal point)
        int mhz = freqMHz % 1000;
        int hundredsDigit = mhz / 100;
        snprintf(params.filePaths[params.numClips++], 48, "%s/digit_%d.mul", AUDIO_PATH, hundredsDigit);

        // Last two digits as natural number (tens file)
        int lastTwo = mhz % 100;
        snprintf(params.filePaths[params.numClips++], 48, "%s/tens_%02d.mul", AUDIO_PATH, lastTwo);
    }

    // 5. Direction clip (if enabled)
    if (includeDirection) {
        const char* dirFile = nullptr;
        switch (direction) {
            case AlertDirection::AHEAD:  dirFile = "dir_ahead.mul"; break;
            case AlertDirection::BEHIND: dirFile = "dir_behind.mul"; break;
            case AlertDirection::SIDE:   dirFile = "dir_side.mul"; break;
        }
        if (dirFile) {
            snprintf(params.filePaths[params.numClips++], 48, "%s/%s", AUDIO_PATH, dirFile);
        }
    }

    // 6-7. Bogey count (if > 1): "<count> bogeys"
    if (bogeyCount > 1 && bogeyCount <= 10 && params.numClips < 6) {
        // Add count clip: use digit_X for 2-9, tens_10 for 10
        if (bogeyCount == 10) {
            snprintf(params.filePaths[params.numClips++], 48, "%s/tens_10.mul", AUDIO_PATH);
        } else {
            snprintf(params.filePaths[params.numClips++], 48, "%s/digit_%d.mul", AUDIO_PATH, bogeyCount);
        }
        // Add "bogeys" clip
        snprintf(params.filePaths[params.numClips++], 48, "%s/bogeys.mul", AUDIO_PATH);
        AUDIO_LOGF("[AUDIO] Adding bogey count: %d bogeys\n", bogeyCount);
    }

    AUDIO_LOGF("[AUDIO] Playing %d clips for freq announcement\n", params.numClips);
    for (int i = 0; i < params.numClips; i++) {
        AUDIO_LOGF("[AUDIO]   %d: %s\n", i, params.filePaths[i]);
    }

    // Start task using pre-allocated global params
    start_sd_audio_task(params);
}

// Play band-only announcement (e.g., "Ka", "K", "X", "Laser")
void play_band_only(AlertBand band) {
    AUDIO_LOGF("[AUDIO] play_band_only() band=%d\n", (int)band);

    if (audio_playing.load()) {
        AUDIO_LOGLN("[AUDIO] Already playing, skipping");
        PERF_INC(audioPlayBusy);
        return;
    }

    if (!sd_audio_ready) {
        AUDIO_LOGLN("[AUDIO] SD audio not ready");
        return;
    }

    // Prepare params on stack (no malloc needed)
    SDAudioTaskParams params;
    params.numClips = 0;

    const char* bandFile = nullptr;
    switch (band) {
        case AlertBand::LASER: bandFile = "band_laser.mul"; break;
        case AlertBand::KA:    bandFile = "band_ka.mul"; break;
        case AlertBand::K:     bandFile = "band_k.mul"; break;
        case AlertBand::X:     bandFile = "band_x.mul"; break;
    }

    if (bandFile) {
        snprintf(params.filePaths[params.numClips++], 48, "%s/%s", AUDIO_PATH, bandFile);
    }

    // Start task using pre-allocated global params
    start_sd_audio_task(params);
}

// Play direction-only announcement (used when same alert changes direction)
// Says "ahead", "behind", or "side", optionally with bogey count if > 1
void play_direction_only(AlertDirection direction, uint8_t bogeyCount) {
    AUDIO_LOGF("[AUDIO] play_direction_only() dir=%d bogeys=%d\n", (int)direction, bogeyCount);

    if (audio_playing.load()) {
        AUDIO_LOGLN("[AUDIO] Already playing, skipping");
        PERF_INC(audioPlayBusy);
        return;
    }

    if (!sd_audio_ready) {
        AUDIO_LOGLN("[AUDIO] SD audio not ready, skipping direction-only");
        return;
    }

    // Prepare params on stack (no malloc needed)
    SDAudioTaskParams params;
    params.numClips = 0;

    // Just the direction clip
    const char* dirFile = nullptr;
    switch (direction) {
        case AlertDirection::AHEAD:  dirFile = "dir_ahead.mul"; break;
        case AlertDirection::BEHIND: dirFile = "dir_behind.mul"; break;
        case AlertDirection::SIDE:   dirFile = "dir_side.mul"; break;
    }
    if (dirFile) {
        snprintf(params.filePaths[params.numClips++], 48, "%s/%s", AUDIO_PATH, dirFile);
    }

    // Add bogey count if provided and > 1
    if (bogeyCount > 1 && bogeyCount <= 10) {
        if (bogeyCount == 10) {
            snprintf(params.filePaths[params.numClips++], 48, "%s/tens_10.mul", AUDIO_PATH);
        } else {
            snprintf(params.filePaths[params.numClips++], 48, "%s/digit_%d.mul", AUDIO_PATH, bogeyCount);
        }
        snprintf(params.filePaths[params.numClips++], 48, "%s/bogeys.mul", AUDIO_PATH);
        AUDIO_LOGF("[AUDIO] Adding bogey count: %d bogeys\n", bogeyCount);
    }

    if (params.numClips == 0) {
        return;
    }

    AUDIO_LOGF("[AUDIO] Playing direction-only: %s\n", params.filePaths[0]);

    // Start task using pre-allocated global params
    start_sd_audio_task(params);
}

// Call from main loop to handle amp warm timeout
// Disables amp after AMP_WARM_TIMEOUT_MS of inactivity to save power.
// This runs at most 10 Hz; finer cadence does not change the user-visible
// timeout behavior but does add pointless main-loop housekeeping.
void audio_process_amp_timeout() {
    if (!amp_is_warm || audio_playing) {
        return;
    }

    static unsigned long lastAmpTimeoutCheckMs = 0;
    const unsigned long now = millis();
    if (now - lastAmpTimeoutCheckMs < AMP_TIMEOUT_CHECK_INTERVAL_MS) {
        return;
    }
    lastAmpTimeoutCheckMs = now;

    if (now - amp_last_used_ms >= AMP_WARM_TIMEOUT_MS) {
        const AudioI2cResult result = set_speaker_amp(false, 0);
        if (result == AudioI2cResult::Ok) {
            amp_is_warm = false;
            amp_disable_fail_count = 0;
            AUDIO_LOGLN("[AUDIO] Amp timeout - disabled to save power");
        } else if (result != AudioI2cResult::Busy) {
            audio_log_i2c_failure("audio_process_amp_timeout", result);
            amp_disable_fail_count++;
            if (amp_disable_fail_count >= AMP_DISABLE_MAX_RETRIES) {
                Serial.println("[AUDIO] ERROR: Amp timeout disable failed after max retries — giving up");
                amp_is_warm = false;
                amp_disable_fail_count = 0;
            }
        }
    }
}

// Play threat escalation: "[Band] [freq] [direction] [N] bogeys, [X] ahead, [Y] behind"
// Used when secondary alert ramps up from weak (≤2 bars) to strong (≥4 bars) over time
void play_threat_escalation(AlertBand band, uint16_t freqMHz, AlertDirection direction,
                            uint8_t total, uint8_t ahead, uint8_t behind, uint8_t side) {
    AUDIO_LOGF("[AUDIO] play_threat_escalation() band=%d freq=%d dir=%d total=%d\n",
               (int)band, freqMHz, (int)direction, total);

    if (audio_playing.load()) {
        AUDIO_LOGLN("[AUDIO] Already playing, skipping");
        PERF_INC(audioPlayBusy);
        return;
    }

    if (!sd_audio_ready) {
        AUDIO_LOGLN("[AUDIO] SD audio not ready, skipping threat escalation");
        return;
    }

    // Laser excluded - shouldn't happen but guard anyway
    if (band == AlertBand::LASER) {
        AUDIO_LOGLN("[AUDIO] Laser excluded from threat escalation");
        return;
    }

    // Prepare params on stack (no malloc needed)
    SDAudioTaskParams params;
    params.numClips = 0;

    // 1. Band clip
    const char* bandFile = nullptr;
    switch (band) {
        case AlertBand::KA: bandFile = "band_ka.mul"; break;
        case AlertBand::K:  bandFile = "band_k.mul"; break;
        case AlertBand::X:  bandFile = "band_x.mul"; break;
        default: break;
    }
    if (bandFile) {
        snprintf(params.filePaths[params.numClips++], 48, "%s/%s", AUDIO_PATH, bandFile);
    }

    // 2-4. Frequency clips (GHz token reuses two-digit number clips)
    int ghz = getGHz(band, freqMHz);
    if (ghz > 0) {
        snprintf(params.filePaths[params.numClips++], 48, "%s/tens_%02d.mul", AUDIO_PATH, ghz);
    }
    int mhz = freqMHz % 1000;
    int hundredsDigit = mhz / 100;
    snprintf(params.filePaths[params.numClips++], 48, "%s/digit_%d.mul", AUDIO_PATH, hundredsDigit);
    int lastTwo = mhz % 100;
    snprintf(params.filePaths[params.numClips++], 48, "%s/tens_%02d.mul", AUDIO_PATH, lastTwo);

    // 5. Direction clip
    const char* dirFile = nullptr;
    switch (direction) {
        case AlertDirection::AHEAD:  dirFile = "dir_ahead.mul"; break;
        case AlertDirection::BEHIND: dirFile = "dir_behind.mul"; break;
        case AlertDirection::SIDE:   dirFile = "dir_side.mul"; break;
    }
    if (dirFile) {
        snprintf(params.filePaths[params.numClips++], 48, "%s/%s", AUDIO_PATH, dirFile);
    }

    // 6-7. Total bogey count if >= 2: "[N] bogeys"
    if (total >= 2 && total <= 10 && params.numClips < MAX_AUDIO_CLIPS - 2) {
        if (total == 10) {
            snprintf(params.filePaths[params.numClips++], 48, "%s/tens_10.mul", AUDIO_PATH);
        } else {
            snprintf(params.filePaths[params.numClips++], 48, "%s/digit_%d.mul", AUDIO_PATH, total);
        }
        snprintf(params.filePaths[params.numClips++], 48, "%s/bogeys.mul", AUDIO_PATH);
    }

    // 8-9. Direction breakdown: "[N] ahead" (only if > 0)
    if (ahead > 0 && ahead <= 10 && params.numClips < MAX_AUDIO_CLIPS - 2) {
        if (ahead == 10) {
            snprintf(params.filePaths[params.numClips++], 48, "%s/tens_10.mul", AUDIO_PATH);
        } else {
            snprintf(params.filePaths[params.numClips++], 48, "%s/digit_%d.mul", AUDIO_PATH, ahead);
        }
        snprintf(params.filePaths[params.numClips++], 48, "%s/dir_ahead.mul", AUDIO_PATH);
    }

    // 10-11. "[N] behind" (only if > 0)
    if (behind > 0 && behind <= 10 && params.numClips < MAX_AUDIO_CLIPS - 2) {
        if (behind == 10) {
            snprintf(params.filePaths[params.numClips++], 48, "%s/tens_10.mul", AUDIO_PATH);
        } else {
            snprintf(params.filePaths[params.numClips++], 48, "%s/digit_%d.mul", AUDIO_PATH, behind);
        }
        snprintf(params.filePaths[params.numClips++], 48, "%s/dir_behind.mul", AUDIO_PATH);
    }

    // 12. "[N] side" (only if > 0, may be truncated if clips exhausted)
    if (side > 0 && side <= 10 && params.numClips < MAX_AUDIO_CLIPS - 2) {
        if (side == 10) {
            snprintf(params.filePaths[params.numClips++], 48, "%s/tens_10.mul", AUDIO_PATH);
        } else {
            snprintf(params.filePaths[params.numClips++], 48, "%s/digit_%d.mul", AUDIO_PATH, side);
        }
        snprintf(params.filePaths[params.numClips++], 48, "%s/dir_side.mul", AUDIO_PATH);
    }

    AUDIO_LOGF("[AUDIO] Playing threat escalation: %d clips\n", params.numClips);
    for (int i = 0; i < params.numClips; i++) {
        AUDIO_LOGF("[AUDIO]   %d: %s\n", i, params.filePaths[i]);
    }

    // Start task using pre-allocated global params
    start_sd_audio_task(params);
}
