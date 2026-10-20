#include "audio_internals.h"
#include "battery_manager.h"
#include <Arduino.h>
#include <Wire.h>
#include "driver/gpio.h"
#include <esp_heap_caps.h>

#define ES8311_ADDR 0x18

#define TCA9554_ADDR 0x20
#define TCA9554_SPK_AMP_PIN 7

// I2S pins (from Waveshare board_cfg.txt for S3_LCD_3_49)
#define I2S_MCLK_PIN GPIO_NUM_7
#define I2S_BCLK_PIN GPIO_NUM_15
#define I2S_WS_PIN GPIO_NUM_46
#define I2S_DOUT_PIN GPIO_NUM_45 // Data OUT for playback (not DIN=6 which is for recording)

#define SAMPLE_RATE 22050 // Match Waveshare BSP default (22.05kHz)

// ES8311 and TCA9554 share the battery manager's bus (SDA=47, SCL=48).
static TwoWire& audioWire = tca9554Wire;
std::atomic<bool> es8311_initialized{false};
bool i2s_initialized = false;
i2s_chan_handle_t i2s_tx_chan = nullptr;

// Requests arrive on the loop task while the worker applies them on core 1.
// The I2C mutex serializes hardware writes; a newer request must remain pending
// even when an older write completes after it was requested.
static std::atomic<uint8_t> current_volume_percent{75};
static std::atomic<int> applied_volume_percent{-1};

namespace {

constexpr unsigned long AUDIO_I2C_LOG_MIN_INTERVAL_MS = 2000;
std::atomic<unsigned long> lastAudioI2cErrorLogMs{0};

bool shouldLogAudioI2cFailureNow() {
    const unsigned long now = millis();
    const unsigned long previous = lastAudioI2cErrorLogMs.load();
    if (previous != 0 && now - previous < AUDIO_I2C_LOG_MIN_INTERVAL_MS) {
        return false;
    }
    lastAudioI2cErrorLogMs = now;
    return true;
}

} // namespace

void audio_log_i2c_failure(const char* context, AudioI2cResult result) {
    if (!context || result == AudioI2cResult::Ok || !shouldLogAudioI2cFailureNow()) {
        return;
    }

    Serial.printf("[AUDIO][I2C] %s: %s\n", context, audioI2cResultToString(result));
}

static AudioI2cResult es8311_write_reg(uint8_t reg, uint8_t val, TickType_t timeoutTicks = pdMS_TO_TICKS(50)) {
    AudioI2cLockGuard lock(tca9554WireMutex, timeoutTicks);
    if (!lock.ok()) {
        return lock.result();
    }
    return audioI2cWriteRegister(audioWire, ES8311_ADDR, reg, val);
}

// TCA9554 pin 7 drives the active-high speaker amplifier enable; pin 6 is the
// battery power latch.
AudioI2cResult set_speaker_amp(bool enable, TickType_t timeoutTicks) {
    AudioI2cLockGuard lock(tca9554WireMutex, timeoutTicks);
    if (!lock.ok()) {
        return lock.result();
    }

    uint8_t nextConfig = 0;
    uint8_t nextOutput = 0;
    const AudioI2cResult result =
        audioI2cSetTca9554Pin(audioWire, TCA9554_ADDR, TCA9554_CONFIG_PORT, TCA9554_OUTPUT_PORT, TCA9554_SPK_AMP_PIN,
                              enable, &nextConfig, &nextOutput);
    return result;
}

// ES8311 Register definitions (from ESP-ADF)
#define ES8311_RESET_REG00 0x00
#define ES8311_CLK_MANAGER_REG01 0x01
#define ES8311_CLK_MANAGER_REG02 0x02
#define ES8311_CLK_MANAGER_REG03 0x03
#define ES8311_CLK_MANAGER_REG04 0x04
#define ES8311_CLK_MANAGER_REG05 0x05
#define ES8311_CLK_MANAGER_REG06 0x06
#define ES8311_CLK_MANAGER_REG07 0x07
#define ES8311_CLK_MANAGER_REG08 0x08
#define ES8311_SDPIN_REG09 0x09
#define ES8311_SDPOUT_REG0A 0x0A
#define ES8311_SYSTEM_REG0B 0x0B
#define ES8311_SYSTEM_REG0C 0x0C
#define ES8311_SYSTEM_REG0D 0x0D
#define ES8311_SYSTEM_REG0E 0x0E
#define ES8311_SYSTEM_REG0F 0x0F
#define ES8311_SYSTEM_REG10 0x10
#define ES8311_SYSTEM_REG11 0x11
#define ES8311_SYSTEM_REG12 0x12
#define ES8311_SYSTEM_REG13 0x13
#define ES8311_SYSTEM_REG14 0x14
#define ES8311_ADC_REG15 0x15
#define ES8311_ADC_REG16 0x16
#define ES8311_ADC_REG17 0x17
#define ES8311_ADC_REG1B 0x1B
#define ES8311_ADC_REG1C 0x1C
#define ES8311_DAC_REG31 0x31
#define ES8311_DAC_REG32 0x32
#define ES8311_DAC_REG37 0x37
#define ES8311_GPIO_REG44 0x44
#define ES8311_GP_REG45 0x45

static AudioI2cResult es8311_read_reg(uint8_t reg, uint8_t& value, TickType_t timeoutTicks = pdMS_TO_TICKS(50)) {
    AudioI2cLockGuard lock(tca9554WireMutex, timeoutTicks);
    if (!lock.ok()) {
        return lock.result();
    }
    return audioI2cReadRegister(audioWire, ES8311_ADDR, reg, value);
}

// ESP-ADF initialization sequence: slave mode, DAC output, MCLK = 256 * fs.
bool es8311_init() {
    if (es8311_initialized)
        return true;

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

    // Write twice for I2C noise immunity, per ESP-ADF.
    if (!writeOrFail("es8311_init gpio44 noise immunity 1", ES8311_GPIO_REG44, 0x08))
        return false;
    if (!writeOrFail("es8311_init gpio44 noise immunity 2", ES8311_GPIO_REG44, 0x08))
        return false;

    // Initial clock setup.
    if (!writeOrFail("es8311_init reg01", ES8311_CLK_MANAGER_REG01, 0x30))
        return false;
    if (!writeOrFail("es8311_init reg02", ES8311_CLK_MANAGER_REG02, 0x00))
        return false;
    if (!writeOrFail("es8311_init reg03", ES8311_CLK_MANAGER_REG03, 0x10))
        return false;
    if (!writeOrFail("es8311_init reg16", ES8311_ADC_REG16, 0x24))
        return false;
    if (!writeOrFail("es8311_init reg04", ES8311_CLK_MANAGER_REG04, 0x10))
        return false;
    if (!writeOrFail("es8311_init reg05", ES8311_CLK_MANAGER_REG05, 0x00))
        return false;
    if (!writeOrFail("es8311_init reg0b", ES8311_SYSTEM_REG0B, 0x00))
        return false;
    if (!writeOrFail("es8311_init reg0c", ES8311_SYSTEM_REG0C, 0x00))
        return false;
    if (!writeOrFail("es8311_init reg10", ES8311_SYSTEM_REG10, 0x1F))
        return false;
    if (!writeOrFail("es8311_init reg11", ES8311_SYSTEM_REG11, 0x7F))
        return false;

    // Enable the clock state machine in slave mode.
    if (!writeOrFail("es8311_init reset", ES8311_RESET_REG00, 0x80))
        return false;

    // Enable all clocks with MCLK from the external pin.
    if (!writeOrFail("es8311_init reg01 enable clocks", ES8311_CLK_MANAGER_REG01, 0x3F))
        return false;

    // pre_div=1, pre_multi=1 => REG02 = ((1-1)<<5) | (0<<3) = 0x00
    if (!writeOrFail("es8311_init reg02 dividers", ES8311_CLK_MANAGER_REG02, 0x00))
        return false;

    // adc_div=1, dac_div=1 => REG05 = ((1-1)<<4) | ((1-1)<<0) = 0x00
    if (!writeOrFail("es8311_init reg05 dividers", ES8311_CLK_MANAGER_REG05, 0x00))
        return false;

    // fs_mode=0, adc_osr=0x10 => REG03 = (0<<6) | 0x10 = 0x10
    if (!writeOrFail("es8311_init reg03 osr", ES8311_CLK_MANAGER_REG03, 0x10))
        return false;

    // dac_osr=0x10 => REG04 = 0x10
    if (!writeOrFail("es8311_init reg04 osr", ES8311_CLK_MANAGER_REG04, 0x10))
        return false;

    // lrck_h=0x00, lrck_l=0xff => LRCK divider = 256
    if (!writeOrFail("es8311_init reg07", ES8311_CLK_MANAGER_REG07, 0x00))
        return false;
    if (!writeOrFail("es8311_init reg08", ES8311_CLK_MANAGER_REG08, 0xFF))
        return false;

    // bclk_div=4 => REG06 = (4-1)<<0 = 0x03
    if (!writeOrFail("es8311_init reg06", ES8311_CLK_MANAGER_REG06, 0x03))
        return false;

    if (!writeOrFail("es8311_init reg13", ES8311_SYSTEM_REG13, 0x10))
        return false;
    if (!writeOrFail("es8311_init reg1b", ES8311_ADC_REG1B, 0x0A))
        return false;
    if (!writeOrFail("es8311_init reg1c", ES8311_ADC_REG1C, 0x6A))
        return false;

    // REG09: DAC input config - bit6=0 for DAC enabled
    uint8_t dac_iface = 0;
    if (!readOrFail("es8311_init read reg09", ES8311_SDPIN_REG09, dac_iface))
        return false;
    dac_iface &= 0xBF; // Clear bit 6 to enable
    dac_iface |= 0x0C; // 16-bit samples (bits 4:2 = 0b11)
    if (!writeOrFail("es8311_init write reg09", ES8311_SDPIN_REG09, dac_iface))
        return false;

    if (!writeOrFail("es8311_init reg17", ES8311_ADC_REG17, 0xBF))
        return false;
    if (!writeOrFail("es8311_init reg0e", ES8311_SYSTEM_REG0E, 0x02))
        return false;
    if (!writeOrFail("es8311_init reg12", ES8311_SYSTEM_REG12, 0x00))
        return false;
    if (!writeOrFail("es8311_init reg14", ES8311_SYSTEM_REG14, 0x1A))
        return false;
    if (!writeOrFail("es8311_init reg0d", ES8311_SYSTEM_REG0D, 0x01))
        return false;
    if (!writeOrFail("es8311_init reg15", ES8311_ADC_REG15, 0x40))
        return false;
    if (!writeOrFail("es8311_init reg37", ES8311_DAC_REG37, 0x08))
        return false;
    if (!writeOrFail("es8311_init reg45", ES8311_GP_REG45, 0x00))
        return false;

    // Set internal reference signal.
    if (!writeOrFail("es8311_init gpio44 ref signal", ES8311_GPIO_REG44, 0x58))
        return false;

    // A partial initialization may have reset the codec's volume register.
    applied_volume_percent.store(-1);
    const AudioI2cResult volumeResult = audio_apply_pending_volume();
    if (volumeResult != AudioI2cResult::Ok) {
        audio_log_i2c_failure("es8311_init volume", volumeResult);
        return false;
    }

    // Unmute DAC (clear bits 6:5 of REG31).
    uint8_t regv = 0;
    if (!readOrFail("es8311_init read reg31", ES8311_DAC_REG31, regv))
        return false;
    regv &= 0x9F;
    if (!writeOrFail("es8311_init write reg31", ES8311_DAC_REG31, regv))
        return false;

    es8311_initialized = true;

    delay(50); // Let clocks stabilize

    // Emit a post-initialization register snapshot when debug logging is enabled.
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

// ES8311 DAC register 0x32: 0x00 = -95.5dB (mute), 0xBF = 0dB, 0xFF = +32dB
// The lower range (-95dB to -30dB) is inaudible, so we remap:
//   0% = 0x00 (mute)
//   1-100% = 0x90-0xBF (usable range: ~-24dB to 0dB)
AudioI2cResult audio_apply_pending_volume(TickType_t timeoutTicks) {
    if (applied_volume_percent.load() == current_volume_percent.load()) {
        return AudioI2cResult::Ok;
    }
    AudioI2cLockGuard lock(tca9554WireMutex, timeoutTicks);
    if (!lock.ok()) {
        return lock.result();
    }
    // Sample after taking the bus, so waiting writers cannot restore an older
    // request over a newer volume that has already reached the codec.
    const uint8_t requested = current_volume_percent.load();
    if (applied_volume_percent.load() != requested) {
        const uint8_t regVal = requested == 0 ? 0x00 : 0x90 + ((requested - 1) * (0xBF - 0x90)) / 99;
        const AudioI2cResult result = audioI2cWriteRegister(audioWire, ES8311_ADDR, ES8311_DAC_REG32, regVal);
        if (result != AudioI2cResult::Ok) {
            return result;
        }
        applied_volume_percent.store(requested);
    }
    // Keep a request made during the write pending; callers must not start PCM
    // at the old level when a newer mute/volume request is waiting.
    return current_volume_percent.load() == requested ? AudioI2cResult::Ok : AudioI2cResult::Busy;
}

void audio_set_volume(uint8_t volumePercent) {
    if (volumePercent > 100)
        volumePercent = 100;
    current_volume_percent.store(volumePercent);
    if (es8311_initialized.load()) {
        const AudioI2cResult result = audio_apply_pending_volume();
        if (result != AudioI2cResult::Ok) {
            audio_log_i2c_failure("audio_set_volume", result);
        }
    }
}

void i2s_init() {
    if (i2s_initialized)
        return;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // Auto clear legacy data in DMA buffer

    esp_err_t err = i2s_new_channel(&chan_cfg, &i2s_tx_chan, nullptr); // TX only, no RX
    if (err != ESP_OK) {
        return;
    }

    // The codec uses stereo I2S slots; mono samples are duplicated below.
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_MCLK_PIN,
                .bclk = I2S_BCLK_PIN,
                .ws = I2S_WS_PIN,
                .dout = I2S_DOUT_PIN,
                .din = GPIO_NUM_NC, // Not using input
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };

    err = i2s_channel_init_std_mode(i2s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(i2s_tx_chan);
        i2s_tx_chan = nullptr;
        return;
    }

    err = i2s_channel_enable(i2s_tx_chan);
    if (err != ESP_OK) {
        i2s_del_channel(i2s_tx_chan);
        i2s_tx_chan = nullptr;
        return;
    }

    i2s_initialized = true;
}

std::atomic<bool> audio_playing{false};

// Keep the amp warm between closely spaced clips.
std::atomic<bool> amp_is_warm{false};
std::atomic<unsigned long> amp_last_used_ms{0};

// audio_playing gives one job exclusive access. PSRAM buffers preserve internal
// DMA headroom; i2s_channel_write() copies them into its internal DMA ring.
int16_t* g_stereoChunkBuffer = nullptr;
uint8_t* g_mulawChunkBuffer = nullptr;

void audio_init_buffers() {
    g_stereoChunkBuffer = static_cast<int16_t*>(
        heap_caps_malloc(AUDIO_STEREO_CHUNK_SIZE * sizeof(int16_t), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    g_mulawChunkBuffer = static_cast<uint8_t*>(
        heap_caps_malloc(AUDIO_CHUNK_SAMPLES * sizeof(uint8_t), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    if (!g_stereoChunkBuffer || !g_mulawChunkBuffer) {
        Serial.println("[AUDIO] FATAL: PSRAM buffer alloc failed");
    }
}

SDAudioTaskParams g_sdAudioTaskParams;

std::atomic<TaskHandle_t> audioTaskHandle{nullptr};
// Static allocation avoids heap pressure during alerts. LittleFS disables the
// SPI cache, so the audio task stack must stay in internal SRAM.
StackType_t g_sdAudioTaskStack[SD_AUDIO_TASK_STACK_SIZE];
StaticTask_t g_sdAudioTaskTCB;
