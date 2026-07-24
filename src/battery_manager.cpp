/**
 * Battery Manager for Waveshare ESP32-S3-Touch-LCD-3.49
 */

#include "battery_manager.h"
#if defined(V1SIMPLE_HIL_FAULT_CONTROL)
#include "modules/power/battery_bsc16_hil_fault_module.h"
#endif
#include "battery_source_policy.h"
#include "storage_manager.h"
#include "audio_i2c_utils.h"
#include "display_driver.h"
#include "poweroff_policy.h"
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

#define BATTERY_LOGF(...)                                                                                              \
    do {                                                                                                               \
    } while (0)
#define BATTERY_LOGLN(msg)                                                                                             \
    do {                                                                                                               \
    } while (0)

// ADC handles
static adc_oneshot_unit_handle_t adc1_handle = nullptr;
static adc_cali_handle_t adc_cali_handle = nullptr;

namespace {

constexpr uint16_t kShutdownReadbackTimeoutMs = 50;
constexpr uint32_t kPowerButtonReleaseWaitMs = 1500;
constexpr uint32_t kBootButtonReleaseWaitMs = 250;
constexpr uint32_t kWakePinPollMs = 10;

// Power-source classification tuning. The decisions themselves live in
// include/battery_source_policy.h; only the numbers are board-specific.
constexpr battery_source_policy::Config kSourcePolicy{};

// Boot seeds the classifier with a full two-round agreement cycle. begin() is
// allowed to block, so the rounds are separated with a real delay here; the
// periodic refresh spaces them across loop iterations instead.
constexpr uint8_t kBootClassificationRounds = 2;

class ScopedWireTimeout {
  public:
    ScopedWireTimeout(TwoWire& wire, uint16_t timeoutMs) : wire_(wire), previousTimeoutMs_(wire.getTimeOut()) {
        wire_.setTimeOut(timeoutMs);
    }

    ~ScopedWireTimeout() { wire_.setTimeOut(previousTimeoutMs_); }

  private:
    TwoWire& wire_;
    uint16_t previousTimeoutMs_;
};

AudioI2cResult readTca9554RegisterWithTimeout(uint8_t reg, uint8_t& value, TickType_t mutexTimeoutTicks,
                                              uint16_t timeoutMs) {
    AudioI2cLockGuard lock(tca9554WireMutex, mutexTimeoutTicks);
    if (!lock.ok()) {
        return lock.result();
    }

    ScopedWireTimeout timeoutGuard(tca9554Wire, timeoutMs);
    return audioI2cReadRegister(tca9554Wire, TCA9554_I2C_ADDR, reg, value);
}

bool waitForPinHigh(uint8_t pin, uint32_t timeoutMs) {
    const uint32_t startedAtMs = static_cast<uint32_t>(millis());
    do {
        if (digitalRead(pin) == HIGH) {
            return true;
        }
        delay(kWakePinPollMs);
    } while (static_cast<uint32_t>(millis() - startedAtMs) < timeoutMs);
    return digitalRead(pin) == HIGH;
}

uint8_t wakePinForPlan(const poweroff_policy::WakePlan& plan) {
    return plan.input == poweroff_policy::WakeInput::PWR_GPIO16 ? PWR_BUTTON_GPIO : BOOT_BUTTON_GPIO;
}

uint64_t wakeMaskForPlan(const poweroff_policy::WakePlan& plan) {
    return 1ULL << wakePinForPlan(plan);
}

bool wakeMaskIsInactive(uint64_t wakeMask) {
    constexpr uint64_t kRtcGpioMask = (1ULL << 22) - 1;
    if (wakeMask == 0 || (wakeMask & ~kRtcGpioMask) != 0) {
        return false;
    }
    for (int pin = 0; pin <= 21; ++pin) {
        if (!(wakeMask & (1ULL << pin))) {
            continue;
        }
        const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
        if (!rtc_gpio_is_valid_gpio(gpio)) {
            return false;
        }
        const bool pinHigh = gpio_get_level(gpio) != 0;
        if (!pinHigh) {
            return false;
        }
    }
    return true;
}

void releaseBacklightSleepHoldAfterAbort() {
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(static_cast<gpio_num_t>(LCD_BL));
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);
}

} // namespace

// I2C for TCA9554 (separate from touch I2C) - also used by ES8311 codec
TwoWire tca9554Wire(1); // Use I2C port 1
SemaphoreHandle_t tca9554WireMutex = nullptr;

BatteryManager::BatteryManager()
    : initialized_(false), onBattery_(false), lastVoltage_(0), lastButtonPress_(0), buttonPressStart_(0),
      buttonWasPressed_(false), buttonSeenReleasedSinceBoot_(false), cachedVoltage_(0), cachedPercent_(0),
      lastUpdateMs_(0), simulatedVoltage_(0) {}

bool BatteryManager::begin() {
    BATTERY_LOGLN("[Battery] Initializing battery manager...");

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)
    bool hilLatchInitialized = false;
#endif

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
#if defined(V1SIMPLE_HIL_FAULT_CONTROL)
            hilLatchInitialized = true;
#endif
            BATTERY_LOGLN("[Battery] Power latch engaged - device will stay on after button release");
        } else {
            Serial.println("[Battery] WARN: Power latch verification failed!");
        }
    }

    // GPIO16 is HIGH when on battery, LOW when on USB (or button pressed).
    // INPUT without pullup — a pullup would bias the reading if the pin is driven externally.
    pinMode(PWR_BUTTON_GPIO, INPUT);

    // Seed the classifier through the same policy the periodic refresh uses, so
    // boot and steady state can never disagree. Rounds are separated in time —
    // a burst inside one instant is not a majority vote. Until the policy
    // positively confirms USB the device reports BATTERY, so a PWR-button wake
    // (button still held, GPIO16 LOW) can no longer report USB.
    sourceState_ = battery_source_policy::State{};
    Serial.printf("[Battery] Power classification rounds=%u samples=%u spacingMs=%lu\n",
                  static_cast<unsigned>(kBootClassificationRounds),
                  static_cast<unsigned>(kSourcePolicy.samplesPerRound),
                  static_cast<unsigned long>(kSourcePolicy.roundSpacingMs));

    BATTERY_LOGLN("[Battery] Sampling power source detection...");
    for (uint8_t round = 0; round < kBootClassificationRounds; round++) {
        if (round > 0) {
            delay(kSourcePolicy.roundSpacingMs);
        }
        observeSourceRound(static_cast<uint32_t>(millis()));
    }

    Serial.printf("[Battery] Power detection: classification=%s reported=%s\n",
                  battery_source_policy::sourceName(sourceState_.classification), onBattery_ ? "BATTERY" : "USB");

    // Initialize ADC for battery voltage reading. The HIL-only admission seam
    // runs after the power latch and source policy but before any ADC handle is
    // allocated. Production builds compile this branch out completely.
    bool adcInitialized = false;
#if defined(V1SIMPLE_HIL_FAULT_CONTROL)
    BatteryBsc16HilAdcAdmission hilAdcAdmission{};
    hilAdcAdmission.latchInitialized = hilLatchInitialized;
    hilAdcAdmission.sourceClassification = sourceState_.classification;
    hilAdcAdmission.powerButtonWillBeEnabled = onBattery_;
    const uint32_t hilAdcNowMs = static_cast<uint32_t>(millis());
    if (batteryBsc16HilFaultModule().beginAdcAdmission(hilAdcAdmission, hilAdcNowMs)) {
        batteryBsc16HilFaultModule().completeAdcAdmissionSuppression(static_cast<uint32_t>(millis()));
    } else {
        adcInitialized = initADC();
    }
#else
    adcInitialized = initADC();
#endif
    if (!adcInitialized) {
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
    Serial.printf("[Battery] Init OK (%s, %dmV, %d%%, hasBattery=%d)\n", onBattery_ ? "BATTERY" : "USB", cachedVoltage_,
                  cachedPercent_, hasBattery());
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
        if (error == 0)
            break;
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
    current |= (1 << TCA9554_PWR_LATCH_PIN); // Ensure latch pin HIGH
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
    tca9554Wire.write(0xBF); // All inputs except pin 6 (bit 6 = 0 = output)
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

bool BatteryManager::setTCA9554PinWithBudget(uint8_t pin, bool high, TickType_t timeoutTicks, int maxRetries) {
    if (!tca9554WireMutex || xSemaphoreTake(tca9554WireMutex, timeoutTicks) != pdTRUE) {
        Serial.printf("[Battery] TCA9554 mutex busy (timeout=%lu ms)\n", static_cast<unsigned long>(timeoutTicks));
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
            return true; // Success!
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
    if (!adc1_handle)
        return 0;

    int raw = 0;
    esp_err_t ret = adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &raw);
    if (ret != ESP_OK) {
        return lastVoltage_; // Return last known value on error
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

battery_source_policy::Result BatteryManager::observeSourceRound(uint32_t nowMs) {
    battery_source_policy::Observation observation;
    observation.totalSamples = kSourcePolicy.samplesPerRound;

    // A PWR hold pulls GPIO16 LOW regardless of the supply, so a round taken
    // while the button state machine has a press in flight says nothing about
    // the power source. buttonWasPressed_ only ever arms after the pin has been
    // seen HIGH since boot, so it stays false on a genuinely USB-powered unit
    // and therefore never blocks a legitimate USB classification.
    observation.buttonInteraction = buttonWasPressed_;

    for (uint8_t i = 0; i < observation.totalSamples; i++) {
        if (digitalRead(PWR_BUTTON_GPIO) == HIGH) {
            observation.highSamples++;
        }
    }

    const battery_source_policy::Result result =
        battery_source_policy::observe(sourceState_, nowMs, observation, kSourcePolicy);
    onBattery_ = result.onBattery;
    if (result.changed) {
        Serial.printf("[Battery] Power source changed: %s confirmation_ms=%lu\n",
                      battery_source_policy::sourceName(result.classification),
                      static_cast<unsigned long>(result.usbConfirmationElapsedMs));
        battery_source_policy::armEvidenceReplay(sourceEvidenceReplayState_, nowMs, result.usbConfirmationElapsedMs);
    }
    return result;
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

    // Refresh power-source classification on the policy's schedule so USB and
    // battery swaps are picked up. Every decision — two-round agreement, button
    // suppression, fail-toward-battery, rollover-safe timing — belongs to
    // battery_source_policy; this only asks whether a round is due.
    if (battery_source_policy::roundDue(sourceState_, now, kSourcePolicy)) {
        observeSourceRound(now);
    }
    if (battery_source_policy::takeEvidenceReplay(sourceEvidenceReplayState_, now)) {
        Serial.printf("[Battery] Power source stable: %s confirmation_ms=%lu\n",
                      battery_source_policy::sourceName(sourceState_.classification),
                      static_cast<unsigned long>(sourceEvidenceReplayState_.usbConfirmationElapsedMs));
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

    BATTERY_LOGF("[Battery] Power latch pin 6 is %s (0x%02X)\n", latchHigh ? "HIGH" : "LOW", current);

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
    if (!sdLogEnabled_ || !storageManager.isSDCard())
        return;
    File f = SD_MMC.open("/poweroff.log", FILE_APPEND);
    if (f) {
        f.printf("[%lu] %s\n", millis(), line);
        f.close();
    }
}

static void blankPanelBacklightForSleepOrPowerOff() {
    Serial.println("[Battery] Fading backlight...");
    for (int i = 0; i <= 255; i += 5) {
        analogWrite(LCD_BL, i); // Inverted: 255 = off
        delay(10);
    }

    analogWrite(LCD_BL, 255); // Backlight off (inverted)
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH); // Force off (inverted backlight)
    delay(50);
}

bool BatteryManager::enterDeepSleep(uint64_t wakeMask, bool sdLogEnabled, uint64_t pullupMask, const char* outcome) {
    sdLogEnabled_ = sdLogEnabled;

    Serial.println("[Battery] Executing deep sleep entry...");
    sdLog("=== DEEP-SLEEP BEGIN ===");

    char buf[160];
    snprintf(buf, sizeof(buf), "onBattery=%d voltage=%dmV percent=%d%%", onBattery_, cachedVoltage_, cachedPercent_);
    sdLog(buf);

    blankPanelBacklightForSleepOrPowerOff();

    snprintf(buf, sizeof(buf), "ext1Mask=0x%016llX pu=0x%016llX trigger=ANY_LOW",
             static_cast<unsigned long long>(wakeMask), static_cast<unsigned long long>(pullupMask));
    Serial.printf("[Battery] Deep sleep config: %s\n", buf);
    sdLog(buf);

    // Configure RTC pad pull-ups before enabling the wake source.
    for (int pin = 0; pin <= 21; ++pin) {
        if (!(pullupMask & (1ULL << pin)))
            continue;
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

    if (!wakeMaskIsInactive(wakeMask)) {
        Serial.println("[Battery] ERROR: selected deep-sleep wake input is already asserted");
        sdLog("OUTCOME sleep_aborted reason=wake_input_asserted_before_arm");
        releaseBacklightSleepHoldAfterAbort();
        return false;
    }

    if (wakeMask != 0) {
        const esp_err_t wakeResult = esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
        if (wakeResult != ESP_OK) {
            snprintf(buf, sizeof(buf), "OUTCOME sleep_aborted reason=ext1_config_failed err=%d",
                     static_cast<int>(wakeResult));
            Serial.printf("[Battery] %s\n", buf);
            sdLog(buf);
            releaseBacklightSleepHoldAfterAbort();
            return false;
        }
    }

    // Keep the terminal outcome adjacent to the next BOOT record in
    // /poweroff.log so a bounded diagnostics tail still shows the decision.
    if (outcome && outcome[0] != '\0') {
        Serial.printf("[Battery] Deep sleep outcome: %s\n", outcome);
        sdLog(outcome);
    }

    delay(100); // Let serial flush
    Serial.flush();
    if (!wakeMaskIsInactive(wakeMask)) {
        Serial.println("[Battery] ERROR: deep-sleep wake input asserted before sleep entry");
        sdLog("OUTCOME sleep_aborted reason=wake_input_asserted_before_sleep");
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
        releaseBacklightSleepHoldAfterAbort();
        return false;
    }
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
    snprintf(buf, sizeof(buf), "onBattery=%d voltage=%dmV percent=%d%%", onBattery_, cachedVoltage_, cachedPercent_);
    sdLog(buf);

    const poweroff_policy::Strategy strategy = poweroff_policy::selectStrategy(onBattery_);
    snprintf(buf, sizeof(buf), "strategy=%s", poweroff_policy::strategyName(strategy));
    Serial.printf("[Battery] Power-off strategy: %s\n", poweroff_policy::strategyName(strategy));
    sdLog(buf);

    blankPanelBacklightForSleepOrPowerOff();

    auto enterWithWakePlan = [&](const poweroff_policy::WakePlan& wakePlan, const char* outcome) {
        const uint64_t wakeMask = wakeMaskForPlan(wakePlan);
        return enterDeepSleep(wakeMask, sdLogEnabled, wakeMask, outcome);
    };

    if (strategy == poweroff_policy::Strategy::DEEP_SLEEP_EXTERNAL_POWER) {
        // A TCA9554 latch write cannot remove an attached USB/external rail.
        // Still isolate the battery path so unplugging USB later cannot leave
        // the unit drawing from the cell while asleep.
        const bool batteryLatchDropped = setTCA9554PinWithBudget(TCA9554_PWR_LATCH_PIN, false, pdMS_TO_TICKS(250), 5);
        const char* batteryLatchOutcome = batteryLatchDropped ? "WRITE_OK" : "WRITE_FAILED";
        if (batteryLatchDropped) {
            uint8_t readback = 0;
            const AudioI2cResult readbackResult = readTca9554RegisterWithTimeout(
                TCA9554_OUTPUT_PORT, readback, pdMS_TO_TICKS(kShutdownReadbackTimeoutMs), kShutdownReadbackTimeoutMs);
            if (readbackResult == AudioI2cResult::Ok) {
                batteryLatchOutcome = (readback & (1 << TCA9554_PWR_LATCH_PIN)) == 0 ? "LOW" : "HIGH_STUCK";
            } else {
                batteryLatchOutcome = "READBACK_FAILED";
            }
        }
        waitForPinHigh(BOOT_BUTTON_GPIO, kBootButtonReleaseWaitMs);
        poweroff_policy::WakePlan wakePlan = poweroff_policy::planExternalPowerWake();
        Serial.printf("[Battery] External power remains; battery latch=%s, wake=%s trigger=active_low\n",
                      batteryLatchOutcome, poweroff_policy::wakeInputName(wakePlan.input));
        snprintf(buf, sizeof(buf), "OUTCOME mode=deep_sleep_external_power batteryLatch=%s wake=%s trigger=active_low",
                 batteryLatchOutcome, poweroff_policy::wakeInputName(wakePlan.input));
        if (!enterWithWakePlan(wakePlan, buf)) {
            wakePlan = poweroff_policy::planExternalPowerWake();
            snprintf(buf, sizeof(buf),
                     "OUTCOME mode=deep_sleep_external_power retry=1 batteryLatch=%s wake=%s trigger=active_low",
                     batteryLatchOutcome, poweroff_policy::wakeInputName(wakePlan.input));
            if (!enterWithWakePlan(wakePlan, buf)) {
                Serial.println("[Battery] ERROR: external-power sleep aborted; no stable inactive wake input");
                sdLog("OUTCOME shutdown_failed mode=external reason=no_stable_inactive_wake device=awake");
            }
        }
        return false; // Successful deep-sleep entry never returns on hardware.
    }

    // Drop the power latch to cut power entirely.  No deep sleep — the 18650
    // is not kept alive for RTC, so battery drain while "off" is zero.
    // Deep sleep is only used as a last-resort fallback if the latch drop fails.
    if (isCritical()) {
        Serial.println("[Battery] Critical battery - hard power off to protect cell");
        sdLog("CRITICAL battery - hard power off");
    } else {
        Serial.println("[Battery] Dropping power latch...");
    }

    const bool latchDropped = setTCA9554PinWithBudget(TCA9554_PWR_LATCH_PIN, false, pdMS_TO_TICKS(250), 5);
    snprintf(buf, sizeof(buf), "latchDrop=%s", latchDropped ? "OK" : "FAILED");
    Serial.printf("[Battery] Latch drop result: %s\n", latchDropped ? "OK" : "FAILED");
    sdLog(buf);

    const char* fallbackReason = latchDropped ? "rail_alive_after_latch" : "latch_write_failed";
    if (latchDropped) {
        // Keep shutdown readback explicitly bounded so a wedged I2C bus cannot
        // block the deep-sleep fallback forever.
        uint8_t readback = 0;
        const AudioI2cResult readbackResult = readTca9554RegisterWithTimeout(
            TCA9554_OUTPUT_PORT, readback, pdMS_TO_TICKS(kShutdownReadbackTimeoutMs), kShutdownReadbackTimeoutMs);
        if (readbackResult == AudioI2cResult::Ok) {
            bool pin6Low = (readback & (1 << TCA9554_PWR_LATCH_PIN)) == 0;
            snprintf(buf, sizeof(buf), "readback=0x%02X pin6=%s", readback, pin6Low ? "LOW" : "HIGH_STUCK");
            Serial.printf("[Battery] TCA9554 %s\n", buf);
            sdLog(buf);
            if (!pin6Low) {
                fallbackReason = "latch_readback_high";
            }
        } else {
            snprintf(buf, sizeof(buf), "readback=%s", audioI2cResultToString(readbackResult));
            Serial.printf("[Battery] TCA9554 readback failed (%s)\n", audioI2cResultToString(readbackResult));
            sdLog(buf);
            fallbackReason = "latch_readback_failed";
        }

        delay(500); // Wait for power rail to collapse
        // If we reach here, the latch drop didn't fully cut power.
        Serial.println("[Battery] WARN: Still running after latch drop - power rail did not collapse");
        sdLog("WARN: still alive after 500ms latch drop wait");
    } else {
        Serial.println("[Battery] ERROR: Failed to drop power latch, falling back to deep sleep");
        sdLog("ERROR: latch drop failed");
    }

    // Fallback: wait for the manual PWR hold to release. GPIO16 is safe as an
    // active-low wake source only while it is HIGH; otherwise BOOT owns wake.
    bool pwrPinHigh = waitForPinHigh(PWR_BUTTON_GPIO, kPowerButtonReleaseWaitMs);
    poweroff_policy::WakePlan wakePlan = poweroff_policy::planBatteryFallbackWake(pwrPinHigh);
    snprintf(buf, sizeof(buf), "OUTCOME mode=deep_sleep_fallback reason=%s wake=%s trigger=active_low", fallbackReason,
             poweroff_policy::wakeInputName(wakePlan.input));
    Serial.printf("[Battery] Entering deep sleep fallback: reason=%s wake=%s trigger=active_low\n", fallbackReason,
                  poweroff_policy::wakeInputName(wakePlan.input));
    if (!enterWithWakePlan(wakePlan, buf)) {
        pwrPinHigh = digitalRead(PWR_BUTTON_GPIO) == HIGH;
        wakePlan = poweroff_policy::planBatteryFallbackWake(pwrPinHigh);
        snprintf(buf, sizeof(buf), "OUTCOME mode=deep_sleep_fallback retry=1 reason=%s wake=%s trigger=active_low",
                 fallbackReason, poweroff_policy::wakeInputName(wakePlan.input));
        if (!enterWithWakePlan(wakePlan, buf)) {
            Serial.println("[Battery] ERROR: battery fallback sleep aborted; no stable inactive wake input");
            sdLog("OUTCOME shutdown_failed mode=battery_fallback reason=no_stable_inactive_wake device=awake");
        }
    }
    return false; // Successful hard cut or deep-sleep entry never returns.
#endif // CAR_MODE_PWR_SHORT
}

bool BatteryManager::isPowerButtonPressed() {
    // PWR button is on GPIO16, active LOW
    return digitalRead(PWR_BUTTON_GPIO) == LOW;
}

// processPowerButtonState() is defined in battery_manager_button.cpp
// (a hardware-free TU kept separate for testability).

bool BatteryManager::processPowerButton() {
    // Bug #17 defect C: this used to be gated on hasBattery(), which returns
    // false whenever ADC init failed — a unit with a dead ADC could not be
    // powered off at all. The gate below is handed ADC health explicitly and
    // deliberately ignores it: a failed ADC degrades voltage reporting only,
    // never input handling.
    battery_source_policy::ButtonGateInputs gate;
    gate.managerInitialized = initialized_;
    gate.classification = sourceState_.classification;
    // adc1_handle is the ADC-health flag — null whenever initADC() failed.
    gate.adcHealthy = static_cast<bool>(adc1_handle);
    gate.batteryVoltageValid = cachedVoltage_ >= BATTERY_EMPTY_MV;
    if (!battery_source_policy::powerButtonHandlingEnabled(gate))
        return false;

    bool pinLow = isPowerButtonPressed();
    // Safety: require at least one HIGH sample (button released) before arming.
    // Prevents a button held at power-on from triggering an immediate shutdown.
    if (!pinLow)
        buttonSeenReleasedSinceBoot_ = true;
    if (!buttonSeenReleasedSinceBoot_)
        return false;

    PwrButtonState state{buttonWasPressed_, buttonPressStart_};
    bool result = processPowerButtonState(pinLow, static_cast<uint32_t>(millis()), state);
    buttonWasPressed_ = state.buttonWasPressed;
    buttonPressStart_ = state.buttonPressStart;
    return result;
}
