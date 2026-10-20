#include "audio_internals.h"
#include "audio_task_utils.h"
#include "storage_manager.h"
#include <Arduino.h>

static bool sd_audio_ready = false;
static const char* AUDIO_PATH = "/audio";
static fs::FS* audioFS = nullptr; // Filesystem containing audio files
static uint8_t amp_disable_fail_count = 0;
static constexpr uint8_t AMP_DISABLE_MAX_RETRIES = 5;
static constexpr unsigned long AMP_TIMEOUT_CHECK_INTERVAL_MS = 100UL;

// Clips are bundled in LittleFS, independent of the primary storage device.
void audio_init_sd(StorageManager& storage) {
    sd_audio_ready = false;
    audioFS = nullptr;

    fs::FS* littlefs = storage.getLittleFS();
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
    -32124, -31100, -30076, -29052, -28028, -27004, -25980, -24956, -23932, -22908, -21884, -20860, -19836, -18812,
    -17788, -16764, -15996, -15484, -14972, -14460, -13948, -13436, -12924, -12412, -11900, -11388, -10876, -10364,
    -9852,  -9340,  -8828,  -8316,  -7932,  -7676,  -7420,  -7164,  -6908,  -6652,  -6396,  -6140,  -5884,  -5628,
    -5372,  -5116,  -4860,  -4604,  -4348,  -4092,  -3900,  -3772,  -3644,  -3516,  -3388,  -3260,  -3132,  -3004,
    -2876,  -2748,  -2620,  -2492,  -2364,  -2236,  -2108,  -1980,  -1884,  -1820,  -1756,  -1692,  -1628,  -1564,
    -1500,  -1436,  -1372,  -1308,  -1244,  -1180,  -1116,  -1052,  -988,   -924,   -876,   -844,   -812,   -780,
    -748,   -716,   -684,   -652,   -620,   -588,   -556,   -524,   -492,   -460,   -428,   -396,   -372,   -356,
    -340,   -324,   -308,   -292,   -276,   -260,   -244,   -228,   -212,   -196,   -180,   -164,   -148,   -132,
    -120,   -112,   -104,   -96,    -88,    -80,    -72,    -64,    -56,    -48,    -40,    -32,    -24,    -16,
    -8,     0,      32124,  31100,  30076,  29052,  28028,  27004,  25980,  24956,  23932,  22908,  21884,  20860,
    19836,  18812,  17788,  16764,  15996,  15484,  14972,  14460,  13948,  13436,  12924,  12412,  11900,  11388,
    10876,  10364,  9852,   9340,   8828,   8316,   7932,   7676,   7420,   7164,   6908,   6652,   6396,   6140,
    5884,   5628,   5372,   5116,   4860,   4604,   4348,   4092,   3900,   3772,   3644,   3516,   3388,   3260,
    3132,   3004,   2876,   2748,   2620,   2492,   2364,   2236,   2108,   1980,   1884,   1820,   1756,   1692,
    1628,   1564,   1500,   1436,   1372,   1308,   1244,   1180,   1116,   1052,   988,    924,    876,    844,
    812,    780,    748,    716,    684,    652,    620,    588,    556,    524,    492,    460,    428,    396,
    372,    356,    340,    324,    308,    292,    276,    260,    244,    228,    212,    196,    180,    164,
    148,    132,    120,    112,    104,    96,     88,     80,     72,     64,     56,     48,     40,     32,
    24,     16,     8,      0};

static void sd_audio_playback_task(void* pvParameters);
static TaskHandle_t sdAudioWorkerHandle = nullptr;
// Protected by audio_playing: the caller installs this before notification;
// the worker publishes a terminal receipt before releasing job ownership.
static AudioPlaybackReceipt* sdAudioReceipt = nullptr;

static void publish_audio_receipt(AudioPlaybackReceipt::State state) {
    if (sdAudioReceipt) {
        sdAudioReceipt->state.store(state);
        sdAudioReceipt = nullptr;
    }
}

// Use the returned result: a short clip can finish before the caller samples
// audio_playing, making an accepted job appear to have failed.
static AudioPlaybackResult start_sd_audio_task(const SDAudioTaskParams& localParams,
                                               AudioPlaybackReceipt* receipt = nullptr) {
    if (audio_playing.exchange(true)) {
        return AudioPlaybackResult::Busy;
    }

    // Copy local params to pre-allocated global (protected by audio_playing flag)
    g_sdAudioTaskParams.numClips = localParams.numClips;
    for (int i = 0; i < localParams.numClips && i < 12; i++) {
        strncpy(g_sdAudioTaskParams.filePaths[i], localParams.filePaths[i], 47);
        g_sdAudioTaskParams.filePaths[i][47] = '\0';
    }

    // Reuse one worker: deleting/recreating a StaticTask_t can race FreeRTOS
    // idle-task cleanup and corrupt scheduler state.
    if (sdAudioWorkerHandle == nullptr) {
        sdAudioWorkerHandle =
            xTaskCreateStaticPinnedToCore(sd_audio_playback_task, "sd_audio", SD_AUDIO_TASK_STACK_SIZE,
                                          nullptr, // Params passed via global struct
                                          1,       // Priority (low)
                                          g_sdAudioTaskStack, &g_sdAudioTaskTCB,
                                          1 // Core 1
            );
        if (sdAudioWorkerHandle == nullptr) {
            Serial.println("[AUDIO] ERROR: Failed to create SD audio worker!");
            audio_playing.store(false);
            return AudioPlaybackResult::Unavailable;
        }
    }

    sdAudioReceipt = receipt;
    if (receipt) receipt->state.store(AudioPlaybackReceipt::State::Pending);
    audioTaskHandle.store(sdAudioWorkerHandle);
    xTaskNotifyGive(sdAudioWorkerHandle);
    return AudioPlaybackResult::Accepted;
}

static void finish_sd_audio_job() {
    audioResetTaskState(audio_playing, audioTaskHandle);
}

// Retry only startup of this accepted job. Once PCM starts, replaying a failed
// job could repeat a partial or obsolete announcement. Missing clips are not
// retried either. No retry job remains queued after this short bounded window.
static bool prepare_sd_audio_output() {
    constexpr uint8_t maxAttempts = 3;
    for (uint8_t attempt = 0; attempt < maxAttempts; ++attempt) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        if (!i2s_initialized) {
            i2s_init();
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        if (!i2s_initialized || !es8311_init()) {
            continue;
        }
        const AudioI2cResult volumeResult = audio_apply_pending_volume();
        if (volumeResult != AudioI2cResult::Ok) {
            audio_log_i2c_failure("sd_audio_playback_task volume", volumeResult);
            continue;
        }
        const bool cold = !amp_is_warm.load();
        if (cold) {
            vTaskDelay(pdMS_TO_TICKS(20)); // ES8311 lock time
        }
        const AudioI2cResult ampEnableResult = set_speaker_amp(true);
        if (ampEnableResult != AudioI2cResult::Ok) {
            audio_log_i2c_failure("sd_audio_playback_task amp enable", ampEnableResult);
            continue;
        }
        if (cold) {
            vTaskDelay(pdMS_TO_TICKS(50)); // Amp stabilization
        }
        amp_is_warm = true;
        amp_last_used_ms = millis();
        return true;
    }
    return false;
}

static void sd_audio_playback_task(void* pvParameters) {
    (void)pvParameters; // Unused - params are in g_sdAudioTaskParams

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!prepare_sd_audio_output()) {
            publish_audio_receipt(AudioPlaybackReceipt::State::FailedBeforeStart);
            finish_sd_audio_job();
            continue;
        }

        if (!audioFS) {
            Serial.println("[AUDIO] ERROR: audioFS is null!");
            // Preserve the normal warm-timeout ownership of amplifier shutdown.
            publish_audio_receipt(AudioPlaybackReceipt::State::Unavailable);
            finish_sd_audio_job();
            continue;
        }

        if (!g_stereoChunkBuffer || !g_mulawChunkBuffer) {
            Serial.println("[AUDIO] ERROR: PSRAM buffers not allocated!");
            publish_audio_receipt(AudioPlaybackReceipt::State::Unavailable);
            finish_sd_audio_job();
            continue;
        }
        bool writeAborted = false;
        for (int i = 0; i < g_sdAudioTaskParams.numClips; i++) {
            File audioFile = audioFS->open(g_sdAudioTaskParams.filePaths[i], "r");
            if (!audioFile) {
                continue;
            }

            size_t bytesRead;
            while ((bytesRead = audioFile.read(g_mulawChunkBuffer, AUDIO_CHUNK_SAMPLES)) > 0) {
                // Decode mu-law to stereo PCM using pre-allocated buffer
                for (size_t j = 0; j < bytesRead; j++) {
                    int16_t sample = mulaw_decode_table[g_mulawChunkBuffer[j]];
                    g_stereoChunkBuffer[j * 2] = sample;     // Left
                    g_stereoChunkBuffer[j * 2 + 1] = sample; // Right
                }

                // Volume can change while a clip is playing. Do not continue
                // emitting samples at a stale level after an I2C failure.
                const AudioI2cResult volumeResult = audio_apply_pending_volume();
                if (volumeResult != AudioI2cResult::Ok) {
                    audio_log_i2c_failure("sd_audio_playback_task pending volume", volumeResult);
                    publish_audio_receipt(AudioPlaybackReceipt::State::FailedBeforeStart);
                    writeAborted = true;
                    break;
                }
                // Once a write is submitted it may have emitted partial PCM,
                // even if it reports an error. Never retry that announcement.
                publish_audio_receipt(AudioPlaybackReceipt::State::Started);
                size_t bytes_written = 0;
                const AudioWriteResult writeResult = audioWriteWithTimeout([&](TickType_t timeoutTicks) {
                    return i2s_channel_write(i2s_tx_chan, g_stereoChunkBuffer, bytesRead * 2 * sizeof(int16_t),
                                             &bytes_written, timeoutTicks);
                });
                if (writeResult.status != AudioWriteStatus::Ok) {
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
            vTaskDelay(pdMS_TO_TICKS(30)); // Reduced from 50ms

            // Keep the amplifier warm for subsequent clips; the timeout owns shutdown.
            amp_last_used_ms = millis();
        }

        // No readable clip means there was no PCM, but retrying the same
        // missing asset cannot help. A Started receipt is already detached.
        publish_audio_receipt(AudioPlaybackReceipt::State::Unavailable);
        finish_sd_audio_job();
    }
}

int getGHz(AlertBand band, uint16_t freqMHz) {
    switch (band) {
    case AlertBand::KA:
        // Ka band: 33.4-36.0 GHz - determine which integer GHz
        if (freqMHz < 34000)
            return 33;
        if (freqMHz < 35000)
            return 34;
        if (freqMHz < 36000)
            return 35;
        return 36;
    case AlertBand::K:
        return 24; // K band is 24.x GHz
    case AlertBand::KU:
        return 13; // Ku band is 13.x GHz
    case AlertBand::X:
        return 10; // X band is 10.x GHz
    default:
        return 0; // Laser and semantic Photo announcements have no spoken frequency
    }
}

static const char* getBandClipFile(AlertBand band) {
    switch (band) {
    case AlertBand::LASER:
        return "band_laser.mul";
    case AlertBand::KA:
        return "band_ka.mul";
    case AlertBand::K:
        return "band_k.mul";
    case AlertBand::X:
        return "band_x.mul";
    case AlertBand::KU:
        return "band_ku.mul";
    case AlertBand::PHOTO:
        return "band_photo.mul";
    }
    return nullptr;
}

static const char* getDirectionClipFile(AlertDirection direction) {
    switch (direction) {
    case AlertDirection::AHEAD:
        return "dir_ahead.mul";
    case AlertDirection::BEHIND:
        return "dir_behind.mul";
    case AlertDirection::SIDE:
        return "dir_side.mul";
    case AlertDirection::UNKNOWN:
        return nullptr;
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

void play_alert_voice(AlertBand band, AlertDirection direction) {

    if (audio_playing.load()) {
        return;
    }

    if (!sd_audio_ready) {
        return;
    }

    SDAudioTaskParams params;
    params.numClips = 0;
    const char* bandFile = getBandClipFile(band);
    const char* dirFile = getDirectionClipFile(direction);
    if (!bandFile) {
        return;
    }
    appendAudioClip(params, bandFile);
    appendAudioClip(params, dirFile);

    if (params.numClips == 0) {
        return;
    }

    start_sd_audio_task(params);
}

// Play test voice for volume adjustment (short "Ka ahead" composed clip)
void play_test_voice() {
    play_alert_voice(AlertBand::KA, AlertDirection::AHEAD);
}

// Every enabled mode speaks "Photo" without its underlying K-band frequency.
static AudioPlaybackResult play_frequency_voice_impl(AlertBand band, uint16_t freqMHz, AlertDirection direction,
                                                      VoiceAlertMode mode, bool includeDirection,
                                                      uint8_t bogeyCount, AudioPlaybackReceipt* receipt = nullptr) {
    if (audio_playing.load()) {
        return AudioPlaybackResult::Busy;
    }

    if (mode == VOICE_MODE_DISABLED) {
        return AudioPlaybackResult::Unavailable;
    }

    if (!sd_audio_ready) {
        return AudioPlaybackResult::Unavailable;
    }

    // Laser has no frequency; pair its band clip with direction when enabled.
    if (band == AlertBand::LASER) {
        SDAudioTaskParams laserParams;
        laserParams.numClips = 0;
        appendAudioClip(laserParams, getBandClipFile(band));
        if (includeDirection) {
            appendAudioClip(laserParams, getDirectionClipFile(direction));
        }
        return laserParams.numClips > 0 ? start_sd_audio_task(laserParams, receipt) : AudioPlaybackResult::Unavailable;
    }

    SDAudioTaskParams params;
    params.numClips = 0;

    if (band == AlertBand::PHOTO || mode == VOICE_MODE_BAND_ONLY || mode == VOICE_MODE_BAND_FREQ) {
        appendAudioClip(params, getBandClipFile(band));
    }

    if (band != AlertBand::PHOTO && (mode == VOICE_MODE_FREQ_ONLY || mode == VOICE_MODE_BAND_FREQ)) {
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

    if (includeDirection) {
        const char* dirFile = nullptr;
        switch (direction) {
        case AlertDirection::AHEAD:
            dirFile = "dir_ahead.mul";
            break;
        case AlertDirection::BEHIND:
            dirFile = "dir_behind.mul";
            break;
        case AlertDirection::SIDE:
            dirFile = "dir_side.mul";
            break;
        case AlertDirection::UNKNOWN:
            break;
        }
        if (dirFile) {
            snprintf(params.filePaths[params.numClips++], 48, "%s/%s", AUDIO_PATH, dirFile);
        }
    }

    if (bogeyCount > 1 && bogeyCount <= 10 && params.numClips < 6) {
        if (bogeyCount == 10) {
            snprintf(params.filePaths[params.numClips++], 48, "%s/tens_10.mul", AUDIO_PATH);
        } else {
            snprintf(params.filePaths[params.numClips++], 48, "%s/digit_%d.mul", AUDIO_PATH, bogeyCount);
        }
        snprintf(params.filePaths[params.numClips++], 48, "%s/bogeys.mul", AUDIO_PATH);
    }

    return params.numClips > 0 ? start_sd_audio_task(params, receipt) : AudioPlaybackResult::Unavailable;
}

void play_frequency_voice(AlertBand band, uint16_t freqMHz, AlertDirection direction, VoiceAlertMode mode,
                          bool includeDirection, uint8_t bogeyCount) {
    (void)play_frequency_voice_impl(band, freqMHz, direction, mode, includeDirection, bogeyCount);
}

void play_band_only(AlertBand band) {
    if (audio_playing.load()) {
        return;
    }

    if (!sd_audio_ready) {
        return;
    }

    SDAudioTaskParams params;
    params.numClips = 0;

    if (!appendAudioClip(params, getBandClipFile(band))) {
        return;
    }

    start_sd_audio_task(params);
}

static AudioPlaybackResult play_direction_only_impl(AlertDirection direction, uint8_t bogeyCount,
                                                         AudioPlaybackReceipt* receipt = nullptr) {
    if (audio_playing.load()) {
        return AudioPlaybackResult::Busy;
    }

    if (!sd_audio_ready) {
        return AudioPlaybackResult::Unavailable;
    }

    SDAudioTaskParams params;
    params.numClips = 0;

    const char* dirFile = nullptr;
    switch (direction) {
    case AlertDirection::AHEAD:
        dirFile = "dir_ahead.mul";
        break;
    case AlertDirection::BEHIND:
        dirFile = "dir_behind.mul";
        break;
    case AlertDirection::SIDE:
        dirFile = "dir_side.mul";
        break;
    case AlertDirection::UNKNOWN:
        break;
    }
    if (dirFile) {
        snprintf(params.filePaths[params.numClips++], 48, "%s/%s", AUDIO_PATH, dirFile);
    }

    if (bogeyCount > 1 && bogeyCount <= 10) {
        if (bogeyCount == 10) {
            snprintf(params.filePaths[params.numClips++], 48, "%s/tens_10.mul", AUDIO_PATH);
        } else {
            snprintf(params.filePaths[params.numClips++], 48, "%s/digit_%d.mul", AUDIO_PATH, bogeyCount);
        }
        snprintf(params.filePaths[params.numClips++], 48, "%s/bogeys.mul", AUDIO_PATH);
    }

    if (params.numClips == 0) {
        return AudioPlaybackResult::Unavailable;
    }

    return start_sd_audio_task(params, receipt);
}

void play_direction_only(AlertDirection direction, uint8_t bogeyCount) {
    (void)play_direction_only_impl(direction, bogeyCount);
}

// Check amp inactivity at most 10 Hz.
void audio_process_amp_timeout() {
    static unsigned long lastAmpTimeoutCheckMs = 0;
    const unsigned long now = millis();
    if (now - lastAmpTimeoutCheckMs < AMP_TIMEOUT_CHECK_INTERVAL_MS) {
        return;
    }
    lastAmpTimeoutCheckMs = now;

    // Retry a failed user volume change even when there is no next alert. Use
    // a nonblocking bus attempt; the worker also checks before every PCM chunk.
    if (es8311_initialized.load()) {
        const AudioI2cResult result = audio_apply_pending_volume(0);
        if (result != AudioI2cResult::Ok && result != AudioI2cResult::Busy) {
            audio_log_i2c_failure("audio_process_amp_timeout volume", result);
        }
    }
    if (!amp_is_warm || audio_playing) {
        return;
    }

    if (now - amp_last_used_ms >= AMP_WARM_TIMEOUT_MS) {
        const AudioI2cResult result = set_speaker_amp(false, 0);
        if (result == AudioI2cResult::Ok) {
            amp_is_warm = false;
            amp_disable_fail_count = 0;
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

static AudioPlaybackResult play_threat_escalation_impl(AlertBand band, uint16_t freqMHz, AlertDirection direction,
                                                       uint8_t total, uint8_t ahead, uint8_t behind, uint8_t side,
                                                       AudioPlaybackReceipt* receipt = nullptr) {
    if (audio_playing.load()) {
        return AudioPlaybackResult::Busy;
    }

    if (!sd_audio_ready) {
        return AudioPlaybackResult::Unavailable;
    }

    if (band == AlertBand::LASER) {
        return AudioPlaybackResult::Unavailable;
    }

    SDAudioTaskParams params;
    params.numClips = 0;

    appendAudioClip(params, getBandClipFile(band));

    // Photo keeps its K-band frequency internally but does not speak it.
    if (band != AlertBand::PHOTO) {
        int ghz = getGHz(band, freqMHz);
        if (ghz > 0) {
            snprintf(params.filePaths[params.numClips++], 48, "%s/tens_%02d.mul", AUDIO_PATH, ghz);
        }
        int mhz = freqMHz % 1000;
        int hundredsDigit = mhz / 100;
        snprintf(params.filePaths[params.numClips++], 48, "%s/digit_%d.mul", AUDIO_PATH, hundredsDigit);
        int lastTwo = mhz % 100;
        snprintf(params.filePaths[params.numClips++], 48, "%s/tens_%02d.mul", AUDIO_PATH, lastTwo);
    }

    const char* dirFile = nullptr;
    switch (direction) {
    case AlertDirection::AHEAD:
        dirFile = "dir_ahead.mul";
        break;
    case AlertDirection::BEHIND:
        dirFile = "dir_behind.mul";
        break;
    case AlertDirection::SIDE:
        dirFile = "dir_side.mul";
        break;
    case AlertDirection::UNKNOWN:
        break;
    }
    if (dirFile) {
        snprintf(params.filePaths[params.numClips++], 48, "%s/%s", AUDIO_PATH, dirFile);
    }

    if (total >= 2 && total <= 10 && params.numClips < MAX_AUDIO_CLIPS - 2) {
        if (total == 10) {
            snprintf(params.filePaths[params.numClips++], 48, "%s/tens_10.mul", AUDIO_PATH);
        } else {
            snprintf(params.filePaths[params.numClips++], 48, "%s/digit_%d.mul", AUDIO_PATH, total);
        }
        snprintf(params.filePaths[params.numClips++], 48, "%s/bogeys.mul", AUDIO_PATH);
    }

    if (ahead > 0 && ahead <= 10 && params.numClips < MAX_AUDIO_CLIPS - 2) {
        if (ahead == 10) {
            snprintf(params.filePaths[params.numClips++], 48, "%s/tens_10.mul", AUDIO_PATH);
        } else {
            snprintf(params.filePaths[params.numClips++], 48, "%s/digit_%d.mul", AUDIO_PATH, ahead);
        }
        snprintf(params.filePaths[params.numClips++], 48, "%s/dir_ahead.mul", AUDIO_PATH);
    }

    if (behind > 0 && behind <= 10 && params.numClips < MAX_AUDIO_CLIPS - 2) {
        if (behind == 10) {
            snprintf(params.filePaths[params.numClips++], 48, "%s/tens_10.mul", AUDIO_PATH);
        } else {
            snprintf(params.filePaths[params.numClips++], 48, "%s/digit_%d.mul", AUDIO_PATH, behind);
        }
        snprintf(params.filePaths[params.numClips++], 48, "%s/dir_behind.mul", AUDIO_PATH);
    }

    // Omit the side count if earlier clips exhausted capacity.
    if (side > 0 && side <= 10 && params.numClips < MAX_AUDIO_CLIPS - 2) {
        if (side == 10) {
            snprintf(params.filePaths[params.numClips++], 48, "%s/tens_10.mul", AUDIO_PATH);
        } else {
            snprintf(params.filePaths[params.numClips++], 48, "%s/digit_%d.mul", AUDIO_PATH, side);
        }
        snprintf(params.filePaths[params.numClips++], 48, "%s/dir_side.mul", AUDIO_PATH);
    }

    return params.numClips > 0 ? start_sd_audio_task(params, receipt) : AudioPlaybackResult::Unavailable;
}

void play_threat_escalation(AlertBand band, uint16_t freqMHz, AlertDirection direction, uint8_t total, uint8_t ahead,
                            uint8_t behind, uint8_t side) {
    (void)play_threat_escalation_impl(band, freqMHz, direction, total, ahead, behind, side);
}

AudioPlaybackResult try_play_frequency_voice(AlertBand band, uint16_t freqMHz, AlertDirection direction,
                                             VoiceAlertMode mode, bool includeDirection, uint8_t bogeyCount,
                                             AudioPlaybackReceipt* receipt) {
    return play_frequency_voice_impl(band, freqMHz, direction, mode, includeDirection, bogeyCount, receipt);
}

AudioPlaybackResult try_play_direction_only(AlertDirection direction, uint8_t bogeyCount,
                                            AudioPlaybackReceipt* receipt) {
    return play_direction_only_impl(direction, bogeyCount, receipt);
}

AudioPlaybackResult try_play_threat_escalation(AlertBand band, uint16_t freqMHz, AlertDirection direction,
                                               uint8_t total, uint8_t ahead, uint8_t behind, uint8_t side,
                                               AudioPlaybackReceipt* receipt) {
    return play_threat_escalation_impl(band, freqMHz, direction, total, ahead, behind, side, receipt);
}
