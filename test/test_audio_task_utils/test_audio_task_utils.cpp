#include <unity.h>

#include <atomic>

#include "../mocks/freertos/FreeRTOS.h"
#include "../mocks/esp_err.h"
#include "../../include/audio_task_utils.h"

void setUp() {}
void tearDown() {}

void test_audio_write_with_timeout_returns_ok_and_uses_bounded_timeout() {
    TickType_t observedTicks = 0;

    const AudioWriteResult result = audioWriteWithTimeout([&](TickType_t timeoutTicks) {
        observedTicks = timeoutTicks;
        return ESP_OK;
    });

    TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(2000), observedTicks);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AudioWriteStatus::Ok),
                            static_cast<uint8_t>(result.status));
    TEST_ASSERT_EQUAL(ESP_OK, result.error);
}

void test_audio_write_with_timeout_maps_timeout_errors() {
    const AudioWriteResult result = audioWriteWithTimeout([](TickType_t timeoutTicks) {
        TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(2000), timeoutTicks);
        return ESP_ERR_TIMEOUT;
    });

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AudioWriteStatus::Timeout),
                            static_cast<uint8_t>(result.status));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, result.error);
}

void test_audio_write_with_timeout_maps_non_timeout_errors() {
    constexpr esp_err_t kI2sFailure = -42;

    const AudioWriteResult result = audioWriteWithTimeout([](TickType_t timeoutTicks) {
        TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(2000), timeoutTicks);
        return static_cast<esp_err_t>(-42);
    });

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AudioWriteStatus::Error),
                            static_cast<uint8_t>(result.status));
    TEST_ASSERT_EQUAL(kI2sFailure, result.error);
}

void test_audio_reset_task_state_clears_busy_flag_and_handle() {
    std::atomic<bool> audioPlaying{true};
    std::atomic<TaskHandle_t> handle{reinterpret_cast<TaskHandle_t>(0x1234)};

    audioResetTaskState(audioPlaying, handle);

    TEST_ASSERT_FALSE(audioPlaying.load());
    TEST_ASSERT_NULL(handle.load());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_audio_write_with_timeout_returns_ok_and_uses_bounded_timeout);
    RUN_TEST(test_audio_write_with_timeout_maps_timeout_errors);
    RUN_TEST(test_audio_write_with_timeout_maps_non_timeout_errors);
    RUN_TEST(test_audio_reset_task_state_clears_busy_flag_and_handle);
    return UNITY_END();
}
