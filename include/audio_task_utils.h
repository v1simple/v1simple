#pragma once

#include <atomic>
#include <cstdint>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

enum class AudioWriteStatus : uint8_t {
    Ok = 0,
    Timeout = 1,
    Error = 2,
};

struct AudioWriteResult {
    AudioWriteStatus status = AudioWriteStatus::Ok;
    esp_err_t error = ESP_OK;
};

static constexpr TickType_t AUDIO_I2S_WRITE_TIMEOUT_TICKS = pdMS_TO_TICKS(2000);

template <typename WriteFn> inline AudioWriteResult audioWriteWithTimeout(WriteFn&& writeFn) {
    const esp_err_t err = writeFn(AUDIO_I2S_WRITE_TIMEOUT_TICKS);
    if (err == ESP_OK) {
        return {AudioWriteStatus::Ok, err};
    }
    if (err == ESP_ERR_TIMEOUT) {
        return {AudioWriteStatus::Timeout, err};
    }
    return {AudioWriteStatus::Error, err};
}

inline void audioResetTaskState(std::atomic<bool>& audioPlaying, std::atomic<TaskHandle_t>& audioTaskHandle) {
    // Publish the idle state last.  A producer that observes audioPlaying=false
    // may immediately claim the shared buffers and install a new handle, so the
    // previous task must never clear the handle after releasing that guard.
    audioTaskHandle.store(NULL);
    audioPlaying.store(false);
}

inline void audioRecordStackHighWater(std::atomic<uint32_t>& minimumBytes, uint32_t observedBytes) {
    uint32_t current = minimumBytes.load(std::memory_order_relaxed);
    while (observedBytes < current &&
           !minimumBytes.compare_exchange_weak(current, observedBytes, std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
    }
}
