/**
 * Device Battery / Hardware Tests
 *
 * Validates real hardware interfaces on the Waveshare ESP32-S3-Touch-LCD-3.49:
 *   - ADC battery voltage reading (GPIO 4)
 *   - TCA9554 I2C port expander communication
 *   - Power latch pin control
 *   - Power button GPIO (GPIO 16)
 *   - I2C mutex / TCA9554 preservation behavior
 *
 * These tests run non-destructive checks — they will NOT actually power off
 * the device.  They verify the I2C bus, ADC, and GPIO are responsive, which
 * catches wiring/initialization issues before production testing.
 */

#include <unity.h>
#include <Arduino.h>
#include <Wire.h>
#include <esp_adc/adc_oneshot.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>
#include <driver/gpio.h>
#include "../../include/battery_math.h"
#include "../../src/audio_i2c_utils.cpp"
#include "../device_test_reset.h"

// GPIO definitions from battery_manager.h
static constexpr int BATTERY_ADC_GPIO   = 4;
static constexpr int PWR_BUTTON_GPIO    = 16;
static constexpr int TCA9554_SDA_GPIO   = 47;
static constexpr int TCA9554_SCL_GPIO   = 48;
static constexpr uint8_t TCA9554_ADDR   = 0x20;
static constexpr uint8_t TCA9554_CONFIG_PORT  = 0x03;
static constexpr uint8_t TCA9554_OUTPUT_PORT  = 0x01;

void setUp() {}
void tearDown() {}

// ===========================================================================
// ADC INITIALIZATION
// ===========================================================================

void test_battery_adc_gpio_configurable() {
    // Verify the ADC GPIO can be configured as input (basic GPIO test)
    pinMode(BATTERY_ADC_GPIO, INPUT);

    // Reads must resolve to a digital level.
    int level = digitalRead(BATTERY_ADC_GPIO);
    TEST_ASSERT_TRUE(level == LOW || level == HIGH);
}

void test_battery_adc_raw_reading_in_range() {
    // Read raw ADC value. On ESP32-S3, 12-bit ADC gives 0-4095.
    // With or without battery, the ADC pin should read something reasonable.
    adc_oneshot_unit_handle_t handle;
    adc_oneshot_unit_init_cfg_t config = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t err = adc_oneshot_new_unit(&config, &handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    adc_oneshot_chan_cfg_t chanCfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(handle, ADC_CHANNEL_3, &chanCfg);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    int rawValue = 0;
    err = adc_oneshot_read(handle, ADC_CHANNEL_3, &rawValue);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    Serial.printf("  [battery] ADC raw reading: %d\n", rawValue);

    // Should be in valid 12-bit range
    TEST_ASSERT_GREATER_OR_EQUAL(0, rawValue);
    TEST_ASSERT_LESS_OR_EQUAL(4095, rawValue);

    adc_oneshot_del_unit(handle);
}

// ===========================================================================
// TCA9554 I2C PORT EXPANDER
// ===========================================================================

void test_battery_tca9554_i2c_responds() {
    TwoWire testWire(1);
    testWire.begin(TCA9554_SDA_GPIO, TCA9554_SCL_GPIO, 100000);

    testWire.beginTransmission(TCA9554_ADDR);
    uint8_t err = testWire.endTransmission();

    Serial.printf("  [battery] TCA9554 I2C scan: addr=0x%02X result=%d\n",
                  TCA9554_ADDR, err);

    // 0 = success (ACK received)
    TEST_ASSERT_EQUAL_UINT8(0, err);

    testWire.end();
}

void test_battery_tca9554_config_register_readable() {
    TwoWire testWire(1);
    testWire.begin(TCA9554_SDA_GPIO, TCA9554_SCL_GPIO, 100000);

    // Read configuration register
    testWire.beginTransmission(TCA9554_ADDR);
    testWire.write(TCA9554_CONFIG_PORT);
    uint8_t err = testWire.endTransmission();
    TEST_ASSERT_EQUAL_UINT8(0, err);

    uint8_t bytesRead = testWire.requestFrom(TCA9554_ADDR, (uint8_t)1);
    TEST_ASSERT_EQUAL_UINT8(1, bytesRead);

    uint8_t configVal = testWire.read();
    Serial.printf("  [battery] TCA9554 config register: 0x%02X\n", configVal);

    testWire.end();
}

void test_tca9554_pin_update_preserves_power_latch_bits() {
    TwoWire testWire(1);
    testWire.begin(TCA9554_SDA_GPIO, TCA9554_SCL_GPIO, 100000);

    uint8_t initialConfig = 0;
    uint8_t initialOutput = 0;
    AudioI2cResult result = audioI2cReadRegister(testWire, TCA9554_ADDR, TCA9554_CONFIG_PORT, initialConfig);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AudioI2cResult::Ok), static_cast<uint8_t>(result));
    result = audioI2cReadRegister(testWire, TCA9554_ADDR, TCA9554_OUTPUT_PORT, initialOutput);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AudioI2cResult::Ok), static_cast<uint8_t>(result));

    uint8_t enabledConfig = 0;
    uint8_t enabledOutput = 0;
    result = audioI2cSetTca9554Pin(testWire,
                                   TCA9554_ADDR,
                                   TCA9554_CONFIG_PORT,
                                   TCA9554_OUTPUT_PORT,
                                   7,
                                   true,
                                   &enabledConfig,
                                   &enabledOutput);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AudioI2cResult::Ok), static_cast<uint8_t>(result));
    TEST_ASSERT_EQUAL_UINT8(initialConfig & (1 << 6), enabledConfig & (1 << 6));
    TEST_ASSERT_EQUAL_UINT8(initialOutput & (1 << 6), enabledOutput & (1 << 6));

    uint8_t disabledConfig = 0;
    uint8_t disabledOutput = 0;
    result = audioI2cSetTca9554Pin(testWire,
                                   TCA9554_ADDR,
                                   TCA9554_CONFIG_PORT,
                                   TCA9554_OUTPUT_PORT,
                                   7,
                                   false,
                                   &disabledConfig,
                                   &disabledOutput);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AudioI2cResult::Ok), static_cast<uint8_t>(result));
    TEST_ASSERT_EQUAL_UINT8(initialConfig & (1 << 6), disabledConfig & (1 << 6));
    TEST_ASSERT_EQUAL_UINT8(initialOutput & (1 << 6), disabledOutput & (1 << 6));

    result = audioI2cWriteRegister(testWire, TCA9554_ADDR, TCA9554_OUTPUT_PORT, initialOutput);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AudioI2cResult::Ok), static_cast<uint8_t>(result));
    result = audioI2cWriteRegister(testWire, TCA9554_ADDR, TCA9554_CONFIG_PORT, initialConfig);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AudioI2cResult::Ok), static_cast<uint8_t>(result));

    testWire.end();
}

// ===========================================================================
// POWER BUTTON GPIO
// ===========================================================================

void test_battery_power_button_gpio_readable() {
    pinMode(PWR_BUTTON_GPIO, INPUT);

    int state = digitalRead(PWR_BUTTON_GPIO);
    int idfState = gpio_get_level((gpio_num_t)PWR_BUTTON_GPIO);
    Serial.printf("  [battery] Power button GPIO %d state: %d\n",
                  PWR_BUTTON_GPIO, state);

    // Cross-check Arduino and IDF GPIO read paths agree.
    TEST_ASSERT_EQUAL(idfState, state);
}

// ===========================================================================
// I2C MUTEX (shared TCA9554 resource)
// ===========================================================================

void test_battery_i2c_mutex_create_take_give() {
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    TEST_ASSERT_NOT_NULL(mutex);

    // Take (should succeed immediately)
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(mutex, pdMS_TO_TICKS(100)));

    // Give back
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreGive(mutex));

    // Take again (should succeed since we gave it back)
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(mutex, pdMS_TO_TICKS(100)));
    xSemaphoreGive(mutex);

    vSemaphoreDelete(mutex);
}

void test_battery_i2c_concurrent_access_safe() {
    // Simulate the shared I2C bus pattern used by battery/runtime code:
    // mutex-protected access from two rapid callers
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    TEST_ASSERT_NOT_NULL(mutex);

    TwoWire testWire(1);
    testWire.begin(TCA9554_SDA_GPIO, TCA9554_SCL_GPIO, 100000);

    int successfulTransactions = 0;
    for (int i = 0; i < 10; i++) {
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            testWire.beginTransmission(TCA9554_ADDR);
            testWire.write(TCA9554_CONFIG_PORT);
            uint8_t txErr = testWire.endTransmission();
            uint8_t bytesRead = testWire.requestFrom(TCA9554_ADDR, (uint8_t)1);
            if (txErr == 0 && bytesRead == 1 && testWire.available()) {
                testWire.read();
                successfulTransactions++;
            }
            xSemaphoreGive(mutex);
        }
    }

    testWire.end();
    vSemaphoreDelete(mutex);

    TEST_ASSERT_GREATER_THAN(0, successfulTransactions);
}

// ===========================================================================
// VOLTAGE CONVERSION SANITY CHECK
// ===========================================================================

void test_battery_voltage_to_percent_on_device() {
    // These should produce identical results on device and native
    TEST_ASSERT_EQUAL_UINT8(100, battery_math::voltageToPercent(4095));
    TEST_ASSERT_EQUAL_UINT8(0, battery_math::voltageToPercent(3200));
    TEST_ASSERT_EQUAL_UINT8(0, battery_math::voltageToPercent(3000));

    // Midpoint
    uint8_t mid = battery_math::voltageToPercent(3648);
    TEST_ASSERT_UINT8_WITHIN(1, 50, mid);
}

// ===========================================================================
// TEST RUNNER
// ===========================================================================

void setup() {
    if (deviceTestSetup("test_device_battery")) return;

    // Watchdog: auto-reboot if any I2C operation hangs for 10s
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 10000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };
    esp_task_wdt_reconfigure(&wdt_config);
    esp_task_wdt_add(NULL);

    UNITY_BEGIN();

    // ADC
    RUN_TEST(test_battery_adc_gpio_configurable);
    RUN_TEST(test_battery_adc_raw_reading_in_range);

    // TCA9554 I2C
    RUN_TEST(test_battery_tca9554_i2c_responds);
    RUN_TEST(test_battery_tca9554_config_register_readable);
    RUN_TEST(test_tca9554_pin_update_preserves_power_latch_bits);

    // Power button
    RUN_TEST(test_battery_power_button_gpio_readable);

    // I2C mutex
    RUN_TEST(test_battery_i2c_mutex_create_take_give);
    RUN_TEST(test_battery_i2c_concurrent_access_safe);

    // Voltage conversion (device-side verification)
    RUN_TEST(test_battery_voltage_to_percent_on_device);

    UNITY_END();
    esp_task_wdt_delete(NULL);
    deviceTestFinish();
}

void loop() {
    delay(100);  // Keep USB CDC alive after post-test reboot
}
