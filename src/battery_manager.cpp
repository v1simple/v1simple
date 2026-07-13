/**
 * Battery Manager for Waveshare ESP32-S3-Touch-LCD-3.49
 */

#include "battery_manager.h"
#include "storage_manager.h"
#include "audio_i2c_utils.h"
#include "display_driver.h"
#include <Wire.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <SD_MMC.h>

// Only compile for Waveshare 3.49 board

BatteryManager batteryManager;

#define BATTERY_LOGF(...) do { } while (0)
#define BATTERY_LOGLN(msg) do { } while (0)

// ADC handles
static adc_oneshot_unit_handle_t adc1_handle = nullptr;
static adc_cali_handle_t adc_cali_handle = nullptr;

namespace {

constexpr uint16_t kShutdownReadbackTimeoutMs = 50;

class ScopedWireTimeout {
public:
    ScopedWireTimeout(TwoWire& wire, uint16_t timeoutMs)
        : wire_(wire)
        , previousTimeoutMs_(wire.getTimeOut()) {
        wire_.setTimeOut(timeoutMs);
    }

    ~ScopedWireTimeout() {
        wire_.setTimeOut(previousTimeoutMs_);
    }

private:
    TwoWire& wire_;
    uint16_t previousTimeoutMs_;
};

AudioI2cResult readTca9554RegisterWithTimeout(uint8_t reg,
                                              uint8_t& value,
                                              TickType_t mutexTimeoutTicks,
                                              uint16_t timeoutMs) {
    AudioI2cLockGuard lock(tca9554WireMutex, mutexTimeoutTicks);
    if (!lock.ok()) {
        return lock.result();
    }

    ScopedWireTimeout timeoutGuard(tca9554Wire, timeoutMs);
    return audioI2cReadRegister(tca9554Wire, TCA9554_I2C_ADDR, reg, value);
}

}  // namespace

// I2C for TCA9554 (separate from touch I2C) - also used by ES8311 codec
TwoWire tca9554Wire(1);  // Use I2C port 1
SemaphoreHandle_t tca9554WireMutex = nullptr;

BatteryManager::BatteryManager()
    : initialized_(false)
    , onBattery_(false)
    , lastVoltage_(0)
    , lastButtonPress_(0)
    , buttonPressStart_(0)
    , buttonWasPressed_(false)
    , buttonSeenReleasedSinceBoot_(false)
    , cachedVoltage_(0)
    , cachedPercent_(0)
    , lastUpdateMs_(0)
    , simulatedVoltage_(0)
{
}

bool BatteryManager::begin() {
    BATTERY_LOGLN("[Battery] Initializing battery manager...");

    if (!tca9554WireMutex) {
        tca9554WireMutex = xSemaphoreCreateMutex();
        if (!tca9554WireMutex) {
            Serial.println("[Battery] ERROR: failed to create shared TCA9554 I2C mutex");
            return false;
        }
    }

    // CRITICAL: Initialize TCA9554 and latch power FIRST, before anything else
    // This MUST happen immediately on ANY boot to handle button-press boot scenarios
    // During button boot, GPIO16 is LOW (button pressed) but we still need the latch
    BATTERY_LOGLN("[Battery] Initializing power latch (required for battery operation)...");
    if (!initTCA9554()) {
        Serial.println("[Battery] WARN: TCA9554 init failed - power latch unavailable");
    } else {
        if (latchPowerOn()) {
            BATTERY_LOGLN("[Battery] Power latch engaged - device will stay on after button release");
        } else {
            Serial.println("[Battery] WARN: Power latch verification failed!");
        }
    }

    // GPIO16 is HIGH when on battery, LOW when on USB (or button pressed).
    // INPUT without pullup — a pullup would bias the reading if the pin is driven externally.
    pinMode(PWR_BUTTON_GPIO, INPUT);

    // Sample GPIO16 multiple times to debounce.
    constexpr int samples = 10;
    constexpr int sampleDelayMs = 5;
    int highCount = 0;
    Serial.printf("[Battery] Power debounce samples=%d delayMs=%d\n",
                  samples,
                  sampleDelayMs);

    BATTERY_LOGLN("[Battery] Sampling power source detection...");
    for (int i = 0; i < samples; i++) {
        if (digitalRead(PWR_BUTTON_GPIO) == HIGH) {
            highCount++;
        }
        delay(sampleDelayMs);
    }

    // Majority vote
    onBattery_ = (highCount > samples / 2);

    BATTERY_LOGF("[Battery] Power detection: GPIO16 samples=%d/%d (HIGH), decision=%s\n",
                  highCount, samples, onBattery_ ? "BATTERY" : "USB");

    // Initialize ADC for battery voltage reading
    if (!initADC()) {
        Serial.println("[Battery] WARN: ADC init failed, voltage monitoring disabled");
    }

    // Read initial voltage for diagnostics
    uint16_t initialVoltage = 0;
    if (adc1_handle) {
        initialVoltage = readADCMillivolts();
        BATTERY_LOGF("[Battery] Initial voltage reading: %dmV\n", initialVoltage);

        // Sanity check: if we think we're on USB but voltage looks like battery
        if (!onBattery_ && initialVoltage > BATTERY_EMPTY_MV && initialVoltage < BATTERY_FULL_MV + 500) {
            Serial.printf("[Battery] WARN: USB mode but battery voltage detected (%dmV)\n", initialVoltage);
        }
        // Sanity check: if we think we're on battery but voltage is too low or zero
        if (onBattery_ && initialVoltage < BATTERY_EMPTY_MV) {
            Serial.printf("[Battery] WARN: Battery mode but voltage too low (%dmV)\n", initialVoltage);
        }
    }

    // TCA9554 already initialized if on battery (done early)
    // Just log the final status
    if (onBattery_ && !initialized_) {
        BATTERY_LOGLN("[Battery] Power latch already set (early init)");
    }

    initialized_ = true;

    // Do initial cache update to populate voltage reading
    update();
    Serial.printf("[Battery] Init OK (%s, %dmV, %d%%, hasBattery=%d)\n",
                  onBattery_ ? "BATTERY" : "USB",
                  cachedVoltage_, cachedPercent_, hasBattery());
    return true;
}

bool BatteryManager::initADC() {
    // Create calibration handle
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_3,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };

    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle);
    if (ret != ESP_OK) {
        Serial.printf("[Battery] ADC calibration init failed: %d\n", ret);
        return false;
    }

    // Create oneshot unit
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    ret = adc_oneshot_new_unit(&init_config, &adc1_handle);
    if (ret != ESP_OK) {
        Serial.printf("[Battery] ADC unit init failed: %d\n", ret);
        return false;
    }

    // Configure channel
    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };

    ret = adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &chan_config);
    if (ret != ESP_OK) {
        Serial.printf("[Battery] ADC channel config failed: %d\n", ret);
        return false;
    }

    BATTERY_LOGLN("[Battery] ADC initialized_ for battery monitoring");
    return true;
}

bool BatteryManager::initTCA9554() {
    // Initialize I2C for TCA9554 on port 1 (separate from touch)
    tca9554Wire.begin(TCA9554_SDA_GPIO, TCA9554_SCL_GPIO, 100000);

    // Brief delay for I2C bus to stabilize
    delay(10);

    // Check if TCA9554 responds with retries
    uint8_t error = 1;
    for (int retry = 0; retry < 5; retry++) {
        tca9554Wire.beginTransmission(TCA9554_I2C_ADDR);
        error = tca9554Wire.endTransmission();
        if (error == 0) break;
        BATTERY_LOGF("[Battery] TCA9554 probe attempt %d failed\n", retry + 1);
        delay(5);
    }

    if (error != 0) {
        Serial.printf("[Battery] TCA9554 not found at 0x%02X after retries\n", TCA9554_I2C_ADDR);
        return false;
    }

    // CRITICAL: Set output HIGH FIRST before configuring as output
    // Preserve other output states by read-modify-write
    tca9554Wire.beginTransmission(TCA9554_I2C_ADDR);
    tca9554Wire.write(TCA9554_OUTPUT_PORT);
    tca9554Wire.endTransmission(false);
    tca9554Wire.requestFrom((uint8_t)TCA9554_I2C_ADDR, (uint8_t)1);
    uint8_t current = 0;
    if (tca9554Wire.available() >= 1) {
        current = tca9554Wire.read();
    }
    current |= (1 << TCA9554_PWR_LATCH_PIN);  // Ensure latch pin HIGH
    tca9554Wire.beginTransmission(TCA9554_I2C_ADDR);
    tca9554Wire.write(TCA9554_OUTPUT_PORT);
    tca9554Wire.write(current);
    error = tca9554Wire.endTransmission();

    if (error != 0) {
        Serial.printf("[Battery] TCA9554 output set failed: %d\n", error);
        return false;
    }

    // Configure pin 6 as output (remains HIGH)
    tca9554Wire.beginTransmission(TCA9554_I2C_ADDR);
    tca9554Wire.write(TCA9554_CONFIG_PORT);
    tca9554Wire.write(0xBF);  // All inputs except pin 6 (bit 6 = 0 = output)
    error = tca9554Wire.endTransmission();

    if (error != 0) {
        Serial.printf("[Battery] TCA9554 config failed: %d\n", error);
        return false;
    }

    BATTERY_LOGLN("[Battery] TCA9554 initialized_ - power latch engaged");
    return true;
}

bool BatteryManager::setTCA9554Pin(uint8_t pin, bool high) {
    return setTCA9554PinWithBudget(pin, high, pdMS_TO_TICKS(50), 3);
}

bool BatteryManager::setTCA9554PinWithBudget(uint8_t pin,
                                             bool high,
                                             TickType_t timeoutTicks,
                                             int maxRetries) {
    if (!tca9554WireMutex || xSemaphoreTake(tca9554WireMutex, timeoutTicks) != pdTRUE) {
        Serial.printf("[Battery] TCA9554 mutex busy (timeout=%lu ms)\n",
                      static_cast<unsigned long>(timeoutTicks));
        return false;
    }

    static constexpr int RETRY_DELAY_MS = 5;

    for (int attempt = 0; attempt < maxRetries; attempt++) {
        // Read current output state
        tca9554Wire.beginTransmission(TCA9554_I2C_ADDR);
        tca9554Wire.write(TCA9554_OUTPUT_PORT);
        uint8_t error = tca9554Wire.endTransmission(false);

        if (error != 0) {
            if (attempt < maxRetries - 1) {
                BATTERY_LOGF("[Battery] TCA9554 read start failed, retry %d\n", attempt + 1);
                delay(RETRY_DELAY_MS);
                continue;
            }
            Serial.printf("[Battery] TCA9554 read start FAILED after %d attempts\n", maxRetries);
            xSemaphoreGive(tca9554WireMutex);
            return false;
        }

        tca9554Wire.requestFrom((uint8_t)TCA9554_I2C_ADDR, (uint8_t)1);

        if (tca9554Wire.available() < 1) {
            if (attempt < maxRetries - 1) {
                BATTERY_LOGF("[Battery] TCA9554 read failed, retry %d\n", attempt + 1);
                delay(RETRY_DELAY_MS);
                continue;
            }
            Serial.printf("[Battery] TCA9554 read FAILED after %d attempts\n", maxRetries);
            xSemaphoreGive(tca9554WireMutex);
            return false;
        }

        uint8_t current = tca9554Wire.read();

        // Modify the bit
        if (high) {
            current |= (1 << pin);
        } else {
            current &= ~(1 << pin);
        }

        // Write back
        tca9554Wire.beginTransmission(TCA9554_I2C_ADDR);
        tca9554Wire.write(TCA9554_OUTPUT_PORT);
        tca9554Wire.write(current);
        error = tca9554Wire.endTransmission();

        if (error == 0) {
            xSemaphoreGive(tca9554WireMutex);
            return true;  // Success!
        }

        if (attempt < maxRetries - 1) {
            BATTERY_LOGF("[Battery] TCA9554 write failed (err=%d), retry %d\n", error, attempt + 1);
            delay(RETRY_DELAY_MS);
        }
    }

    Serial.printf("[Battery] TCA9554 pin %d set FAILED after %d attempts\n", pin, maxRetries);
    xSemaphoreGive(tca9554WireMutex);
    return false;
}

uint16_t BatteryManager::readADCMillivolts() {
    if (!adc1_handle) return 0;

    int raw = 0;
    esp_err_t ret = adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &raw);
    if (ret != ESP_OK) {
        return lastVoltage_;  // Return last known value on error
    }

    int voltage_mv = 0;
    if (adc_cali_handle) {
        adc_cali_raw_to_voltage(adc_cali_handle, raw, &voltage_mv);
    } else {
        // Fallback calculation without calibration
        voltage_mv = (raw * 3300) / 4096;
    }

    // Apply voltage divider factor (3:1 ratio on the board)
    lastVoltage_ = voltage_mv * 3;
    return lastVoltage_;
}

bool BatteryManager::isOnBattery() const {
    return onBattery_;
}

bool BatteryManager::hasBattery() const {
    // Debug simulation mode - if simulatedVoltage_ is set, use it
    if (simulatedVoltage_ > 0) {
        return true;
    }

    // Must be initialized with working ADC to detect battery
    if (!initialized_ || !adc1_handle) {
        return false;
    }

    // Only show battery icon when actually running on battery power
    // When on USB, we don't show the battery icon even if physically present
    if (!onBattery_) {
        return false;
    }

    // Verify with actual voltage - if below minimum, no real battery
    // This catches cases where GPIO16 floats HIGH but no battery is connected
    if (cachedVoltage_ < BATTERY_EMPTY_MV) {
        return false;
    }

    return true;
}

void BatteryManager::update() {
    if (!initialized_) {
        return;
    }

    // Skip normal updates if in simulation mode
    if (simulatedVoltage_ > 0) {
        return;
    }

    const uint32_t now = static_cast<uint32_t>(millis());

    // Refresh power source detection periodically to handle USB/battery swaps
    // Skip while the power button is held (GPIO16 LOW) to avoid misclassifying as USB
    static uint32_t lastPowerCheckMs = 0;
    if (now - lastPowerCheckMs >= 1000 && !isPowerButtonPressed()) {
        const int samples = 5;
        int highCount = 0;
        for (int i = 0; i < samples; i++) {
            if (digitalRead(PWR_BUTTON_GPIO) == HIGH) {
                highCount++;
            }
        }
        bool detectedBattery = (highCount > samples / 2);
        if (detectedBattery != onBattery_) {
            onBattery_ = detectedBattery;
            Serial.printf("[Battery] Power source changed: %s\n", onBattery_ ? "BATTERY" : "USB");
        }
        lastPowerCheckMs = now;
    }

    // Update cached voltage/percentage every 10 seconds
    // Faster polling needed for USB/battery auto-detection via voltage
    // Force immediate read on first call (cachedVoltage_ == 0) so battery icon shows at boot
    if (cachedVoltage_ == 0 || (now - lastUpdateMs_ >= 10000)) {
        uint16_t voltage = readADCMillivolts();
        cachedVoltage_ = voltage;

        cachedPercent_ = battery_math::voltageToPercent(voltage);

        lastUpdateMs_ = now;
    }
}

uint16_t BatteryManager::getVoltageMillivolts() const {
    return cachedVoltage_;
}

uint8_t BatteryManager::getPercentage() const {
    return cachedPercent_;
}

bool BatteryManager::isLow() const {
    return battery_math::isLow(cachedVoltage_);
}

bool BatteryManager::isCritical() const {
    return battery_math::isCritical(cachedVoltage_);
}

bool BatteryManager::latchPowerOn() {
    // Verify the latch is HIGH (should already be set by initTCA9554)
    BATTERY_LOGLN("[Battery] Verifying power latch is ON");

    // Read current output state
    tca9554Wire.beginTransmission(TCA9554_I2C_ADDR);
    tca9554Wire.write(TCA9554_OUTPUT_PORT);
    tca9554Wire.endTransmission(false);
    tca9554Wire.requestFrom((uint8_t)TCA9554_I2C_ADDR, (uint8_t)1);

    if (tca9554Wire.available() < 1) {
        Serial.println("[Battery] Failed to read power latch state");
        return false;
    }

    uint8_t current = tca9554Wire.read();
    bool latchHigh = (current & (1 << TCA9554_PWR_LATCH_PIN)) != 0;

    BATTERY_LOGF("[Battery] Power latch pin 6 is %s (0x%02X)\n",
                 latchHigh ? "HIGH" : "LOW", current);

    if (!latchHigh) {
        Serial.println("[Battery] WARN: Latch is LOW - forcing HIGH!");
        return setTCA9554Pin(TCA9554_PWR_LATCH_PIN, true);
    }

    return true;
}

// Helper: append a line to /poweroff.log on SD card (best-effort, no mutex —
// WiFi and other SD users are already stopped by prepareForShutdown).
static bool sdLogEnabled_ = false;
static void sdLog(const char* line) {
    if (!sdLogEnabled_ || !storageManager.isSDCard()) return;
    File f = SD_MMC.open("/poweroff.log", FILE_APPEND);
    if (f) {
        f.printf("[%lu] %s\n", millis(), line);
        f.close();
    }
}

static void blankPanelBacklightForSleepOrPowerOff() {
    Serial.println("[Battery] Fading backlight...");
    for (int i = 0; i <= 255; i += 5) {
        analogWrite(LCD_BL, i);  // Inverted: 255 = off
        delay(10);
    }

    analogWrite(LCD_BL, 255);  // Backlight off (inverted)
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);  // Force off (inverted backlight)
    delay(50);
}

bool BatteryManager::enterDeepSleep(uint64_t wakeMask, bool sdLogEnabled, uint64_t pullupMask) {
    sdLogEnabled_ = sdLogEnabled;

    Serial.println("[Battery] Executing deep sleep entry...");
    sdLog("=== DEEP-SLEEP BEGIN ===");

    char buf[160];
    snprintf(buf, sizeof(buf), "onBattery=%d voltage=%dmV percent=%d%%",
             onBattery_, cachedVoltage_, cachedPercent_);
    sdLog(buf);

    blankPanelBacklightForSleepOrPowerOff();

    snprintf(buf, sizeof(buf),
             "ext1Mask=0x%016llX pu=0x%016llX",
             static_cast<unsigned long long>(wakeMask),
             static_cast<unsigned long long>(pullupMask));
    Serial.printf("[Battery] Deep sleep config: %s\n", buf);
    sdLog(buf);

    // Configure RTC pad pull-ups before enabling the wake source.
    for (int pin = 0; pin <= 21; ++pin) {
        if (!(pullupMask & (1ULL << pin))) continue;
        const gpio_num_t g = static_cast<gpio_num_t>(pin);
        if (!rtc_gpio_is_valid_gpio(g)) {
            Serial.printf("[Battery] deep sleep: pin %d is not an RTC GPIO\n", pin);
            continue;
        }
        rtc_gpio_pullup_en(g);
        rtc_gpio_pulldown_dis(g);
    }

    Serial.flush();
    gpio_hold_en(static_cast<gpio_num_t>(LCD_BL));
    gpio_deep_sleep_hold_en();

    if (wakeMask != 0) {
        esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
    }

    delay(100);  // Let serial flush
    esp_deep_sleep_start();
    return true;
}

bool BatteryManager::powerOff(bool sdLogEnabled) {
#ifdef CAR_MODE_PWR_SHORT
    // Car-install builds have no onboard 18650 and no TCA9554 latch to drop.
    // Calling powerOff() in this configuration would attempt hardware ops that
    // don't exist and could brick the unit. Treat it as a confirmed no-op.
    (void)sdLogEnabled;
    Serial.println("[Battery] powerOff: compile-time disabled (CAR_MODE_PWR_SHORT)");
    return true;
#else
    sdLogEnabled_ = sdLogEnabled;
    // Callers must run shutdown preparation before entering this final
    // hardware-only tail.
    Serial.println("[Battery] Executing final power-off sequence...");

    sdLog("=== POWER-OFF BEGIN ===");
    char buf[128];
    snprintf(buf, sizeof(buf), "onBattery=%d voltage=%dmV percent=%d%%",
             onBattery_, cachedVoltage_, cachedPercent_);
    sdLog(buf);

    blankPanelBacklightForSleepOrPowerOff();

    // Drop the power latch to cut power entirely.  No deep sleep — the 18650
    // is not kept alive for RTC, so battery drain while "off" is zero.
    // Deep sleep is only used as a last-resort fallback if the latch drop fails.
    if (isCritical()) {
        Serial.println("[Battery] Critical battery - hard power off to protect cell");
        sdLog("CRITICAL battery - hard power off");
    } else {
        Serial.println("[Battery] Dropping power latch...");
    }

    const bool latchDropped = setTCA9554PinWithBudget(TCA9554_PWR_LATCH_PIN,
                                                      false,
                                                      pdMS_TO_TICKS(250),
                                                      5);
    snprintf(buf, sizeof(buf), "latchDrop=%s", latchDropped ? "OK" : "FAILED");
    Serial.printf("[Battery] Latch drop result: %s\n", latchDropped ? "OK" : "FAILED");
    sdLog(buf);

    if (latchDropped) {
        // Keep shutdown readback explicitly bounded so a wedged I2C bus cannot
        // block the deep-sleep fallback forever.
        uint8_t readback = 0;
        const AudioI2cResult readbackResult = readTca9554RegisterWithTimeout(
            TCA9554_OUTPUT_PORT,
            readback,
            pdMS_TO_TICKS(kShutdownReadbackTimeoutMs),
            kShutdownReadbackTimeoutMs);
        if (readbackResult == AudioI2cResult::Ok) {
            bool pin6Low = (readback & (1 << TCA9554_PWR_LATCH_PIN)) == 0;
            snprintf(buf, sizeof(buf), "readback=0x%02X pin6=%s",
                     readback, pin6Low ? "LOW" : "HIGH_STUCK");
            Serial.printf("[Battery] TCA9554 %s\n", buf);
            sdLog(buf);
        } else {
            snprintf(buf, sizeof(buf), "readback=%s",
                     audioI2cResultToString(readbackResult));
            Serial.printf("[Battery] TCA9554 readback failed (%s)\n",
                          audioI2cResultToString(readbackResult));
            sdLog(buf);
        }

        delay(500);  // Wait for power rail to collapse
        // If we reach here, the latch drop didn't fully cut power.
        Serial.println("[Battery] WARN: Still running after latch drop - power rail did not collapse");
        sdLog("WARN: still alive after 500ms latch drop wait");
    } else {
        Serial.println("[Battery] ERROR: Failed to drop power latch, falling back to deep sleep");
        sdLog("ERROR: latch drop failed");
    }

    // Fallback: enter deep sleep with button wakeup so the device isn't bricked.
    sdLog("entering deep sleep fallback");
    Serial.println("[Battery] Entering deep sleep fallback...");
    enterDeepSleep(1ULL << PWR_BUTTON_GPIO, sdLogEnabled);
    return latchDropped;
#endif  // CAR_MODE_PWR_SHORT
}

bool BatteryManager::isPowerButtonPressed() {
    // PWR button is on GPIO16, active LOW
    return digitalRead(PWR_BUTTON_GPIO) == LOW;
}

// processPowerButtonState() is defined in battery_manager_button.cpp
// (a hardware-free TU kept separate for testability).

bool BatteryManager::processPowerButton() {
    // Allow shutdown when a battery is present.
    if (!hasBattery()) return false;

    bool pinLow = isPowerButtonPressed();
    // Safety: require at least one HIGH sample (button released) before arming.
    // Prevents a button held at power-on from triggering an immediate shutdown.
    if (!pinLow) buttonSeenReleasedSinceBoot_ = true;
    if (!buttonSeenReleasedSinceBoot_) return false;

    PwrButtonState state{buttonWasPressed_, buttonPressStart_};
    bool result = processPowerButtonState(pinLow, static_cast<uint32_t>(millis()), state);
    buttonWasPressed_ = state.buttonWasPressed;
    buttonPressStart_ = state.buttonPressStart;
    return result;
}
