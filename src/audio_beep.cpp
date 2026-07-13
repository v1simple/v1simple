// audio_beep.cpp — Hardware layer + embedded warning PCM playback
// ES8311 DAC codec, I2S driver, TCA9554 amp, pre-recorded warning playback.
// SD-based voice composition lives in audio_voice.cpp.
//
// I2C bus: SDA=47, SCL=48 (shared with battery manager TCA9554)
// I2S pins: MCLK=7, BCLK=15, WS=46, DOUT=45 (for playback)
// TCA9554 address: 0x20 (ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000)
// ES8311 address: 0x18

#include "audio_internals.h"
#include "audio_task_utils.h"
#include "battery_manager.h"  // For tca9554Wire (shared I2C bus)
#include <Arduino.h>
#include <Wire.h>
#include "driver/gpio.h"
#include <esp_heap_caps.h>

// ES8311 I2C address
#define ES8311_ADDR 0x18

// TCA9554 I2C address (same chip as battery manager, address 0x20)
#define TCA9554_ADDR 0x20
#define TCA9554_SPK_AMP_PIN 7

// I2S pins (from Waveshare board_cfg.txt for S3_LCD_3_49)
#define I2S_MCLK_PIN GPIO_NUM_7
#define I2S_BCLK_PIN GPIO_NUM_15
#define I2S_WS_PIN   GPIO_NUM_46
#define I2S_DOUT_PIN GPIO_NUM_45   // Data OUT for playback (not DIN=6 which is for recording)

// Audio parameters
#define SAMPLE_RATE 22050  // Match Waveshare BSP default (22.05kHz)

// Use battery manager's TwoWire instance (already initialized on SDA=47, SCL=48)
// ES8311 codec and TCA9554 IO expander are both on this bus
static TwoWire& audioWire = tca9554Wire;
bool es8311_initialized = false;
bool i2s_initialized = false;
i2s_chan_handle_t i2s_tx_chan = nullptr;  // New I2S driver handle

// Current volume level (0-100%) - must be declared before es8311_init() uses it
static uint8_t current_volume_percent = 75;

namespace {

constexpr unsigned long AUDIO_I2C_LOG_MIN_INTERVAL_MS = 2000;
unsigned long lastAudioI2cErrorLogMs = 0;

bool shouldLogAudioI2cFailureNow() {
    const unsigned long now = millis();
    if (lastAudioI2cErrorLogMs != 0 &&
        now - lastAudioI2cErrorLogMs < AUDIO_I2C_LOG_MIN_INTERVAL_MS) {
        return false;
    }
    lastAudioI2cErrorLogMs = now;
    return true;
}

}  // namespace

void audio_log_i2c_failure(const char* context, AudioI2cResult result) {
    if (!context || result == AudioI2cResult::Ok || !shouldLogAudioI2cFailureNow()) {
        return;
    }

    Serial.printf("[AUDIO][I2C] %s: %s\n", context, audioI2cResultToString(result));
}

// Write a register to ES8311
static AudioI2cResult es8311_write_reg(uint8_t reg,
                                       uint8_t val,
                                       TickType_t timeoutTicks = pdMS_TO_TICKS(50)) {
    AudioI2cLockGuard lock(tca9554WireMutex, timeoutTicks);
    if (!lock.ok()) {
        return lock.result();
    }
    return audioI2cWriteRegister(audioWire, ES8311_ADDR, reg, val);
}

// Enable/disable speaker amp via TCA9554 pin 7
// Note: Battery manager uses pin 6 for power latch, we use pin 7 for speaker amp
// Per ESP-ADF and Waveshare examples, PA_EN is active-HIGH
AudioI2cResult set_speaker_amp(bool enable, TickType_t timeoutTicks) {
    AudioI2cLockGuard lock(tca9554WireMutex, timeoutTicks);
    if (!lock.ok()) {
        return lock.result();
    }

    uint8_t nextConfig = 0;
    uint8_t nextOutput = 0;
    const AudioI2cResult result = audioI2cSetTca9554Pin(audioWire,
                                                        TCA9554_ADDR,
                                                        TCA9554_CONFIG_PORT,
                                                        TCA9554_OUTPUT_PORT,
                                                        TCA9554_SPK_AMP_PIN,
                                                        enable,
                                                        &nextConfig,
                                                        &nextOutput);
    AUDIO_LOGF("[AUDIO] Speaker amp %s config=0x%02X output=0x%02X result=%s\n",
               enable ? "ENABLED" : "DISABLED",
               nextConfig,
               nextOutput,
               audioI2cResultToString(result));
    return result;
}

// ES8311 Register definitions (from ESP-ADF)
#define ES8311_RESET_REG00        0x00
#define ES8311_CLK_MANAGER_REG01  0x01
#define ES8311_CLK_MANAGER_REG02  0x02
#define ES8311_CLK_MANAGER_REG03  0x03
#define ES8311_CLK_MANAGER_REG04  0x04
#define ES8311_CLK_MANAGER_REG05  0x05
#define ES8311_CLK_MANAGER_REG06  0x06
#define ES8311_CLK_MANAGER_REG07  0x07
#define ES8311_CLK_MANAGER_REG08  0x08
#define ES8311_SDPIN_REG09        0x09
#define ES8311_SDPOUT_REG0A       0x0A
#define ES8311_SYSTEM_REG0B       0x0B
#define ES8311_SYSTEM_REG0C       0x0C
#define ES8311_SYSTEM_REG0D       0x0D
#define ES8311_SYSTEM_REG0E       0x0E
#define ES8311_SYSTEM_REG0F       0x0F
#define ES8311_SYSTEM_REG10       0x10
#define ES8311_SYSTEM_REG11       0x11
#define ES8311_SYSTEM_REG12       0x12
#define ES8311_SYSTEM_REG13       0x13
#define ES8311_SYSTEM_REG14       0x14
#define ES8311_ADC_REG15          0x15
#define ES8311_ADC_REG16          0x16
#define ES8311_ADC_REG17          0x17
#define ES8311_ADC_REG1B          0x1B
#define ES8311_ADC_REG1C          0x1C
#define ES8311_DAC_REG31          0x31
#define ES8311_DAC_REG32          0x32
#define ES8311_DAC_REG37          0x37
#define ES8311_GPIO_REG44         0x44
#define ES8311_GP_REG45           0x45

// Read a register from ES8311
static AudioI2cResult es8311_read_reg(uint8_t reg,
                                      uint8_t& value,
                                      TickType_t timeoutTicks = pdMS_TO_TICKS(50)) {
    AudioI2cLockGuard lock(tca9554WireMutex, timeoutTicks);
    if (!lock.ok()) {
        return lock.result();
    }
    return audioI2cReadRegister(audioWire, ES8311_ADDR, reg, value);
}

// Full ES8311 initialization - exact copy of ESP-ADF es8311_codec_init
// For 24kHz, MCLK=6.144MHz (256*fs), slave mode, DAC output
bool es8311_init() {
    if (es8311_initialized) return true;

    AUDIO_LOGLN("[AUDIO] ES8311 init (ESP-ADF pattern)");

    auto writeOrFail = [](const char* context, uint8_t reg, uint8_t value) -> bool {
        const AudioI2cResult result = es8311_write_reg(reg, value);
        if (result == AudioI2cResult::Ok) {
            return true;
        }
        audio_log_i2c_failure(context, result);
        return false;
    };

    auto readOrFail = [](const char* context, uint8_t reg, uint8_t& value) -> bool {
        const AudioI2cResult result = es8311_read_reg(reg, value);
        if (result == AudioI2cResult::Ok) {
            return true;
        }
        audio_log_i2c_failure(context, result);
        return false;
    };

    // Coefficient for 24kHz with 6.144MHz MCLK from coeff_div table:
    // {6144000 , 24000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10}
    // pre_div=1, pre_multi=1, adc_div=1, dac_div=1, fs_mode=0, lrck_h=0, lrck_l=0xff, bclk_div=4, adc_osr=0x10, dac_osr=0x10

    // Step 1: Enhance I2C noise immunity (write twice per ESP-ADF)
    if (!writeOrFail("es8311_init gpio44 noise immunity 1", ES8311_GPIO_REG44, 0x08)) return false;
    if (!writeOrFail("es8311_init gpio44 noise immunity 2", ES8311_GPIO_REG44, 0x08)) return false;

    // Step 2: Initial clock setup
    if (!writeOrFail("es8311_init reg01", ES8311_CLK_MANAGER_REG01, 0x30)) return false;
    if (!writeOrFail("es8311_init reg02", ES8311_CLK_MANAGER_REG02, 0x00)) return false;
    if (!writeOrFail("es8311_init reg03", ES8311_CLK_MANAGER_REG03, 0x10)) return false;
    if (!writeOrFail("es8311_init reg16", ES8311_ADC_REG16, 0x24)) return false;
    if (!writeOrFail("es8311_init reg04", ES8311_CLK_MANAGER_REG04, 0x10)) return false;
    if (!writeOrFail("es8311_init reg05", ES8311_CLK_MANAGER_REG05, 0x00)) return false;
    if (!writeOrFail("es8311_init reg0b", ES8311_SYSTEM_REG0B, 0x00)) return false;
    if (!writeOrFail("es8311_init reg0c", ES8311_SYSTEM_REG0C, 0x00)) return false;
    if (!writeOrFail("es8311_init reg10", ES8311_SYSTEM_REG10, 0x1F)) return false;
    if (!writeOrFail("es8311_init reg11", ES8311_SYSTEM_REG11, 0x7F)) return false;

    // Step 3: Enable CSM (clock state machine) in slave mode
    if (!writeOrFail("es8311_init reset", ES8311_RESET_REG00, 0x80)) return false;

    // Step 4: Enable all clocks, MCLK from external pin
    if (!writeOrFail("es8311_init reg01 enable clocks", ES8311_CLK_MANAGER_REG01, 0x3F)) return false;

    // Step 5: Configure clock dividers for 24kHz @ 6.144MHz MCLK
    // pre_div=1, pre_multi=1 => REG02 = ((1-1)<<5) | (0<<3) = 0x00
    if (!writeOrFail("es8311_init reg02 dividers", ES8311_CLK_MANAGER_REG02, 0x00)) return false;

    // adc_div=1, dac_div=1 => REG05 = ((1-1)<<4) | ((1-1)<<0) = 0x00
    if (!writeOrFail("es8311_init reg05 dividers", ES8311_CLK_MANAGER_REG05, 0x00)) return false;

    // fs_mode=0, adc_osr=0x10 => REG03 = (0<<6) | 0x10 = 0x10
    if (!writeOrFail("es8311_init reg03 osr", ES8311_CLK_MANAGER_REG03, 0x10)) return false;

    // dac_osr=0x10 => REG04 = 0x10
    if (!writeOrFail("es8311_init reg04 osr", ES8311_CLK_MANAGER_REG04, 0x10)) return false;

    // lrck_h=0x00, lrck_l=0xff => LRCK divider = 256
    if (!writeOrFail("es8311_init reg07", ES8311_CLK_MANAGER_REG07, 0x00)) return false;
    if (!writeOrFail("es8311_init reg08", ES8311_CLK_MANAGER_REG08, 0xFF)) return false;

    // bclk_div=4 => REG06 = (4-1)<<0 = 0x03
    if (!writeOrFail("es8311_init reg06", ES8311_CLK_MANAGER_REG06, 0x03)) return false;

    // Step 6: Additional setup from ESP-ADF
    if (!writeOrFail("es8311_init reg13", ES8311_SYSTEM_REG13, 0x10)) return false;
    if (!writeOrFail("es8311_init reg1b", ES8311_ADC_REG1B, 0x0A)) return false;
    if (!writeOrFail("es8311_init reg1c", ES8311_ADC_REG1C, 0x6A)) return false;

    // Step 7: START the DAC (from es8311_start)
    // REG09: DAC input config - bit6=0 for DAC enabled
    uint8_t dac_iface = 0;
    if (!readOrFail("es8311_init read reg09", ES8311_SDPIN_REG09, dac_iface)) return false;
    dac_iface &= 0xBF;  // Clear bit 6 to enable
    dac_iface |= 0x0C;  // 16-bit samples (bits 4:2 = 0b11)
    if (!writeOrFail("es8311_init write reg09", ES8311_SDPIN_REG09, dac_iface)) return false;

    if (!writeOrFail("es8311_init reg17", ES8311_ADC_REG17, 0xBF)) return false;
    if (!writeOrFail("es8311_init reg0e", ES8311_SYSTEM_REG0E, 0x02)) return false;
    if (!writeOrFail("es8311_init reg12", ES8311_SYSTEM_REG12, 0x00)) return false;
    if (!writeOrFail("es8311_init reg14", ES8311_SYSTEM_REG14, 0x1A)) return false;
    if (!writeOrFail("es8311_init reg0d", ES8311_SYSTEM_REG0D, 0x01)) return false;
    if (!writeOrFail("es8311_init reg15", ES8311_ADC_REG15, 0x40)) return false;
    if (!writeOrFail("es8311_init reg37", ES8311_DAC_REG37, 0x08)) return false;
    if (!writeOrFail("es8311_init reg45", ES8311_GP_REG45, 0x00)) return false;

    // Step 8: Set internal reference signal
    if (!writeOrFail("es8311_init gpio44 ref signal", ES8311_GPIO_REG44, 0x58)) return false;

    // Step 9: Set DAC volume based on saved setting
    // Use same mapping as audio_set_volume(): 0%=mute, 1-100%=0x90-0xBF
    uint8_t volReg;
    if (current_volume_percent == 0) {
        volReg = 0x00;
    } else {
        volReg = 0x90 + ((current_volume_percent - 1) * (0xBF - 0x90)) / 99;
    }
    if (!writeOrFail("es8311_init reg32 volume", ES8311_DAC_REG32, volReg)) return false;

    // Step 10: Unmute DAC (clear bits 6:5 of REG31)
    uint8_t regv = 0;
    if (!readOrFail("es8311_init read reg31", ES8311_DAC_REG31, regv)) return false;
    regv &= 0x9F;
    if (!writeOrFail("es8311_init write reg31", ES8311_DAC_REG31, regv)) return false;

    es8311_initialized = true;

    delay(50);  // Let clocks stabilize

    // Debug: Dump key registers (only when debug logging enabled)
    if (AUDIO_DEBUG_LOGS) {
        auto readDebugReg = [](uint8_t reg) -> uint8_t {
            uint8_t value = 0;
            return es8311_read_reg(reg, value) == AudioI2cResult::Ok ? value : 0;
        };
        Serial.println("[AUDIO] ES8311 registers after init:");
        Serial.printf("  REG00: 0x%02X\n", readDebugReg(ES8311_RESET_REG00));
        Serial.printf("  REG01: 0x%02X\n", readDebugReg(ES8311_CLK_MANAGER_REG01));
        Serial.printf("  REG06: 0x%02X\n", readDebugReg(ES8311_CLK_MANAGER_REG06));
        Serial.printf("  REG09: 0x%02X\n", readDebugReg(ES8311_SDPIN_REG09));
        Serial.printf("  REG0D: 0x%02X\n", readDebugReg(ES8311_SYSTEM_REG0D));
        Serial.printf("  REG0E: 0x%02X\n", readDebugReg(ES8311_SYSTEM_REG0E));
        Serial.printf("  REG12: 0x%02X\n", readDebugReg(ES8311_SYSTEM_REG12));
        Serial.printf("  REG14: 0x%02X\n", readDebugReg(ES8311_SYSTEM_REG14));
        Serial.printf("  REG31: 0x%02X\n", readDebugReg(ES8311_DAC_REG31));
        Serial.printf("  REG32: 0x%02X\n", readDebugReg(ES8311_DAC_REG32));
        Serial.printf("  REG44: 0x%02X\n", readDebugReg(ES8311_GPIO_REG44));
    }
    return true;
}

// Set audio volume (0-100%)
// ES8311 DAC register 0x32: 0x00 = -95.5dB (mute), 0xBF = 0dB, 0xFF = +32dB
// The lower range (-95dB to -30dB) is inaudible, so we remap:
//   0% = 0x00 (mute)
//   1-100% = 0x90-0xBF (usable range: ~-24dB to 0dB)
void audio_set_volume(uint8_t volumePercent) {
    if (volumePercent > 100) volumePercent = 100;
    current_volume_percent = volumePercent;

    uint8_t regVal;
    if (volumePercent == 0) {
        regVal = 0x00;  // Mute
    } else {
        // Map 1-100% to 0x90-0xBF (144-191, a 47-step usable range)
        // This gives audible volume across the full slider
        regVal = 0x90 + ((volumePercent - 1) * (0xBF - 0x90)) / 99;
    }

    // Apply volume if ES8311 is initialized
    if (es8311_initialized) {
        const AudioI2cResult result = es8311_write_reg(ES8311_DAC_REG32, regVal);
        if (result != AudioI2cResult::Ok) {
            audio_log_i2c_failure("audio_set_volume", result);
        }
        AUDIO_LOGF("[AUDIO] Volume set to %d%% (reg=0x%02X)\n", volumePercent, regVal);
    }
}

// I2S init for playback using NEW I2S STD driver (like Waveshare BSP)
void i2s_init() {
    if (i2s_initialized) return;

    AUDIO_LOGLN("[AUDIO] Initializing I2S (new STD driver)...");


    // Step 1: Create I2S channel
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;  // Auto clear legacy data in DMA buffer

    esp_err_t err = i2s_new_channel(&chan_cfg, &i2s_tx_chan, nullptr);  // TX only, no RX
    if (err != ESP_OK) {
        AUDIO_LOGF("[AUDIO] i2s_new_channel failed: %d\\n", err);
        return;
    }

    // Step 2: Configure I2S standard mode (Philips format, STEREO, 16-bit)
    // Note: ES8311 may expect stereo I2S even for mono output
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din = GPIO_NUM_NC,  // Not using input
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(i2s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        AUDIO_LOGF("[AUDIO] i2s_channel_init_std_mode failed: %d\\n", err);
        i2s_del_channel(i2s_tx_chan);
        i2s_tx_chan = nullptr;
        return;
    }

    // Step 3: Enable the channel
    err = i2s_channel_enable(i2s_tx_chan);
    if (err != ESP_OK) {
        AUDIO_LOGF("[AUDIO] i2s_channel_enable failed: %d\\n", err);
        i2s_del_channel(i2s_tx_chan);
        i2s_tx_chan = nullptr;
        return;
    }

    i2s_initialized = true;
    AUDIO_LOGF("[AUDIO] I2S initialized: %dHz, MCLK=%d BCLK=%d WS=%d DOUT=%d\\n",
               SAMPLE_RATE, I2S_MCLK_PIN, I2S_BCLK_PIN, I2S_WS_PIN, I2S_DOUT_PIN);
}

// Include pre-recorded volume-zero warning audio
#include "../include/warning_audio.h"

// Track if audio is currently playing to prevent overlapping
std::atomic<bool> audio_playing{false};

// Amp warm-keeping: keep amp on for a few seconds after playback for faster subsequent plays
std::atomic<bool> amp_is_warm{false};
std::atomic<unsigned long> amp_last_used_ms{0};

// ============================================================================
// Pre-allocated audio buffers (no malloc in audio tasks)
// These are safe to use without mutex because audio_playing atomic flag
// ensures only one audio task runs at a time.
//
// Allocated in PSRAM by audio_init_buffers() to keep ~5 KiB off internal
// .bss — this prevents WiFi/BLE transient allocations from fragmenting
// the largest contiguous DMA block below the 10 KiB SLO floor.
// i2s_channel_write() copies from src to its DMA ring, so PSRAM is fine.
// ============================================================================
int16_t* g_stereoChunkBuffer = nullptr;
uint8_t* g_mulawChunkBuffer = nullptr;

// --- PSRAM buffer allocation (call once from setup, before audio_init_sd) ---
void audio_init_buffers() {
    g_stereoChunkBuffer = static_cast<int16_t*>(
        heap_caps_malloc(AUDIO_STEREO_CHUNK_SIZE * sizeof(int16_t),
                         MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    g_mulawChunkBuffer = static_cast<uint8_t*>(
        heap_caps_malloc(AUDIO_CHUNK_SAMPLES * sizeof(uint8_t),
                         MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    if (!g_stereoChunkBuffer || !g_mulawChunkBuffer) {
        Serial.println("[AUDIO] FATAL: PSRAM buffer alloc failed");
    }
}

// Pre-allocated task params (avoids malloc for param passing)
static struct {
    const int16_t* pcm_data;
    int num_samples;
    int duration_ms;
} g_pcmTaskParams;

SDAudioTaskParams g_sdAudioTaskParams;

std::atomic<TaskHandle_t> audioTaskHandle{nullptr};
// UINT32_MAX distinguishes "not sampled" from the critical valid result of
// zero bytes remaining.
std::atomic<uint32_t> g_audioPcmStackHighWaterBytes{UINT32_MAX};
std::atomic<uint32_t> g_audioSdStackHighWaterBytes{UINT32_MAX};

uint32_t audio_pcm_stack_high_water_bytes() {
    return g_audioPcmStackHighWaterBytes.load(std::memory_order_relaxed);
}

uint32_t audio_sd_stack_high_water_bytes() {
    return g_audioSdStackHighWaterBytes.load(std::memory_order_relaxed);
}

// ============================================================================
// Static task allocation for SD audio playback.
// Pre-allocates stack and TCB at compile time to avoid heap allocation failures
// when heap is low during alerts.
//
// IMPORTANT: g_sdAudioTaskStack MUST live in internal SRAM (not PSRAM).
// The sd_audio task reads LittleFS (SPI flash).  Flash reads require
// disabling the SPI cache, which makes PSRAM inaccessible.  A PSRAM
// stack would cause an assertion failure in cache_utils.c.
// ============================================================================
StackType_t g_sdAudioTaskStack[SD_AUDIO_TASK_STACK_SIZE];
StaticTask_t g_sdAudioTaskTCB;

static void finish_pcm_audio_task() {
    audioRecordStackHighWater(g_audioPcmStackHighWaterBytes,
                              static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)));
    audioResetTaskState(audio_playing, audioTaskHandle);
    // This task uses xTaskCreatePinnedToCoreWithCaps, so its matching delete
    // path must release the capability-allocated stack.
    vTaskDeleteWithCaps(nullptr);
}

// Background task for audio playback - runs on separate core to avoid blocking main loop
// Uses pre-allocated g_stereoChunkBuffer - streams in chunks instead of full buffer
static void audio_playback_task(void* pvParameters) {
    // Use pre-allocated params (no malloc needed)
    const int16_t* pcm_data = g_pcmTaskParams.pcm_data;
    int num_samples = g_pcmTaskParams.num_samples;
    int duration_ms = g_pcmTaskParams.duration_ms;
    (void)pvParameters;  // Unused - params are in global struct

    if (!g_stereoChunkBuffer) {
        Serial.println("[AUDIO] ERROR: PSRAM buffers not allocated!");
        finish_pcm_audio_task();
        return;
    }

    if (i2s_tx_chan == nullptr) {
        // CRITICAL: Start I2S FIRST so MCLK is running before ES8311 init
        i2s_init();
        vTaskDelay(pdMS_TO_TICKS(50));  // Let clocks stabilize
    }

    if (!i2s_initialized) {
        Serial.println("[AUDIO] ERROR: I2S init failed!");
        finish_pcm_audio_task();
        return;
    }

    if (!es8311_init()) {
        finish_pcm_audio_task();
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));  // Let ES8311 lock to MCLK

    // Enable speaker amp - let it fully stabilize
    const AudioI2cResult ampEnableResult = set_speaker_amp(true);
    if (ampEnableResult != AudioI2cResult::Ok) {
        audio_log_i2c_failure("audio_playback_task amp enable", ampEnableResult);
        finish_pcm_audio_task();
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    // Stream mono PCM to stereo in chunks using pre-allocated buffer
    // This avoids large dynamic allocation (was up to 147KB for warning audio)
    int samples_remaining = num_samples;
    int sample_offset = 0;

    while (samples_remaining > 0) {
        int chunk_samples = (samples_remaining > AUDIO_CHUNK_SAMPLES)
                          ? AUDIO_CHUNK_SAMPLES : samples_remaining;

        // Convert chunk from mono to stereo using pre-allocated buffer
        for (int i = 0; i < chunk_samples; ++i) {
            int16_t sample = pgm_read_word(&pcm_data[sample_offset + i]);
            g_stereoChunkBuffer[i * 2] = sample;       // Left channel
            g_stereoChunkBuffer[i * 2 + 1] = sample;   // Right channel
        }

        size_t bytes_written = 0;
        const AudioWriteResult writeResult = audioWriteWithTimeout([&](TickType_t timeoutTicks) {
            return i2s_channel_write(i2s_tx_chan,
                                     g_stereoChunkBuffer,
                                     chunk_samples * 2 * sizeof(int16_t),
                                     &bytes_written,
                                     timeoutTicks);
        });

        if (writeResult.status != AudioWriteStatus::Ok) {
            AUDIO_LOGF("[AUDIO] i2s_channel_write %s: %d\\n",
                       writeResult.status == AudioWriteStatus::Timeout ? "timed out" : "failed",
                       writeResult.error);
            break;
        }

        sample_offset += chunk_samples;
        samples_remaining -= chunk_samples;
    }

    // Wait for DMA to finish
    vTaskDelay(pdMS_TO_TICKS(duration_ms > 0 ? 100 : 50));

    const AudioI2cResult ampDisableResult = set_speaker_amp(false);
    if (ampDisableResult != AudioI2cResult::Ok) {
        audio_log_i2c_failure("audio_playback_task amp disable", ampDisableResult);
        amp_is_warm = true;
        amp_last_used_ms = millis();
    } else {
        amp_is_warm = false;
    }
    finish_pcm_audio_task();
}

// Non-blocking PCM playback on a FreeRTOS task. Mono input, converted to stereo for I2S.
// Pre-allocated buffers — no malloc inside the task.
static void play_pcm_audio(const int16_t* pcm_data, int num_samples, int duration_ms) {
    // Atomic exchange: if already true, return; otherwise set to true
    if (audio_playing.exchange(true)) {
        AUDIO_LOGLN("[AUDIO] Already playing, skipping");
        PERF_INC(audioPlayBusy);
        return;
    }

    // Copy params to pre-allocated struct (protected by audio_playing flag)
    g_pcmTaskParams.pcm_data = pcm_data;
    g_pcmTaskParams.num_samples = num_samples;
    g_pcmTaskParams.duration_ms = duration_ms;

    // Create task on core 1 (core 0 is for WiFi/BLE) with adequate stack
    // Stack allocated in PSRAM via WithCaps API to reduce internal SRAM fragmentation.
    // Task params are passed via g_pcmTaskParams global (no malloc needed)
    TaskHandle_t tempHandle;
    BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
        audio_playback_task,
        "audio_play",
        4096,           // Stack size
        nullptr,        // Params passed via global struct
        1,              // Priority (low)
        &tempHandle,
        1,              // Core 1
        MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM
    );
    if (result == pdPASS) {
        audioTaskHandle.store(tempHandle);
    }

    if (result != pdPASS) {
        Serial.println("[AUDIO] ERROR: Failed to create audio task!");
        PERF_INC(audioTaskFail);
        audio_playing = false;
    } else {
        PERF_INC(audioPlayCount);
    }
}

// Play "Warning Volume Zero" speech (non-blocking)
void play_vol0_beep() {
    AUDIO_LOGLN("[AUDIO] play_vol0_beep() called");

    if (audio_playing) {
        AUDIO_LOGLN("[AUDIO] Already playing, skipping");
        PERF_INC(audioPlayBusy);
        return;
    }

    AUDIO_LOGF("[AUDIO] Playing 'Warning Volume Zero' (%dms)\\n", WARNING_VOLUME_ZERO_PCM_DURATION_MS);
    play_pcm_audio(warning_volume_zero_pcm, WARNING_VOLUME_ZERO_PCM_SAMPLES, WARNING_VOLUME_ZERO_PCM_DURATION_MS);
}
