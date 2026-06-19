#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/Wire.h"
#include "../mocks/freertos/FreeRTOS.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/audio_i2c_utils.cpp"

namespace {

TwoWire testWire;
SemaphoreHandle_t testMutex = reinterpret_cast<SemaphoreHandle_t>(1);

void queueReadRegister(uint8_t value) {
    testWire.queueEndTransmission(0);
    testWire.queueRequestFrom(1, {value});
}

}  // namespace

void setUp() {
    testWire.resetMock();
    mock_reset_semaphore_state();
}

void tearDown() {}

void test_audio_i2c_lock_guard_reports_busy_when_mutex_take_fails() {
    mock_queue_semaphore_take_result(pdFALSE);

    AudioI2cLockGuard lock(testMutex, 0);

    TEST_ASSERT_FALSE(lock.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AudioI2cResult::Busy),
                            static_cast<uint8_t>(lock.result()));
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_semaphore_state.takeCalls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_semaphore_state.giveCalls);
}

void test_audio_i2c_set_tca9554_pin_does_not_guess_when_config_read_fails() {
    testWire.queueEndTransmission(0);
    testWire.queueRequestFrom(0, {});

    const AudioI2cResult result = audioI2cSetTca9554Pin(testWire,
                                                        0x20,
                                                        0x03,
                                                        0x01,
                                                        7,
                                                        true);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AudioI2cResult::ReadFailed),
                            static_cast<uint8_t>(result));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(testWire.transmissionHistory.size()));
    TEST_ASSERT_EQUAL_UINT8(0x03, testWire.transmissionHistory[0].data[0]);
}

void test_audio_i2c_set_tca9554_pin_does_not_guess_when_output_read_fails() {
    queueReadRegister(0xBF);
    testWire.queueEndTransmission(0);
    testWire.queueRequestFrom(0, {});

    const AudioI2cResult result = audioI2cSetTca9554Pin(testWire,
                                                        0x20,
                                                        0x03,
                                                        0x01,
                                                        7,
                                                        true);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AudioI2cResult::ReadFailed),
                            static_cast<uint8_t>(result));
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(testWire.transmissionHistory.size()));
    TEST_ASSERT_EQUAL_UINT8(0x03, testWire.transmissionHistory[0].data[0]);
    TEST_ASSERT_EQUAL_UINT8(0x01, testWire.transmissionHistory[1].data[0]);
}

void test_audio_i2c_set_tca9554_pin_preserves_pin6_when_enabling_pin7() {
    queueReadRegister(0xBF);  // pin6 output, pin7 input
    queueReadRegister(0x40);  // pin6 HIGH, pin7 LOW
    testWire.queueEndTransmission(0);
    testWire.queueEndTransmission(0);

    uint8_t nextConfig = 0;
    uint8_t nextOutput = 0;
    const AudioI2cResult result = audioI2cSetTca9554Pin(testWire,
                                                        0x20,
                                                        0x03,
                                                        0x01,
                                                        7,
                                                        true,
                                                        &nextConfig,
                                                        &nextOutput);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AudioI2cResult::Ok),
                            static_cast<uint8_t>(result));
    TEST_ASSERT_EQUAL_HEX8(0x3F, nextConfig);
    TEST_ASSERT_EQUAL_HEX8(0xC0, nextOutput);
    TEST_ASSERT_EQUAL_UINT32(4u, static_cast<uint32_t>(testWire.transmissionHistory.size()));
    TEST_ASSERT_EQUAL_UINT8(0x01, testWire.transmissionHistory[2].data[0]);
    TEST_ASSERT_EQUAL_UINT8(0xC0, testWire.transmissionHistory[2].data[1]);
    TEST_ASSERT_EQUAL_UINT8(0x03, testWire.transmissionHistory[3].data[0]);
    TEST_ASSERT_EQUAL_UINT8(0x3F, testWire.transmissionHistory[3].data[1]);
}

void test_audio_i2c_set_tca9554_pin_preserves_pin6_when_disabling_pin7() {
    queueReadRegister(0x3F);  // pin6 output, pin7 already output
    queueReadRegister(0xC0);  // pin6 HIGH, pin7 HIGH
    testWire.queueEndTransmission(0);
    testWire.queueEndTransmission(0);

    uint8_t nextConfig = 0;
    uint8_t nextOutput = 0;
    const AudioI2cResult result = audioI2cSetTca9554Pin(testWire,
                                                        0x20,
                                                        0x03,
                                                        0x01,
                                                        7,
                                                        false,
                                                        &nextConfig,
                                                        &nextOutput);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AudioI2cResult::Ok),
                            static_cast<uint8_t>(result));
    TEST_ASSERT_EQUAL_HEX8(0x3F, nextConfig);
    TEST_ASSERT_EQUAL_HEX8(0x40, nextOutput);
    TEST_ASSERT_EQUAL_UINT8(0x01, testWire.transmissionHistory[2].data[0]);
    TEST_ASSERT_EQUAL_UINT8(0x40, testWire.transmissionHistory[2].data[1]);
    TEST_ASSERT_EQUAL_UINT8(0x03, testWire.transmissionHistory[3].data[0]);
    TEST_ASSERT_EQUAL_UINT8(0x3F, testWire.transmissionHistory[3].data[1]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_audio_i2c_lock_guard_reports_busy_when_mutex_take_fails);
    RUN_TEST(test_audio_i2c_set_tca9554_pin_does_not_guess_when_config_read_fails);
    RUN_TEST(test_audio_i2c_set_tca9554_pin_does_not_guess_when_output_read_fails);
    RUN_TEST(test_audio_i2c_set_tca9554_pin_preserves_pin6_when_enabling_pin7);
    RUN_TEST(test_audio_i2c_set_tca9554_pin_preserves_pin6_when_disabling_pin7);
    return UNITY_END();
}
