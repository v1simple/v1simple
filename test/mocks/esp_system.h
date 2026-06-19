#pragma once

#include <cstdint>

typedef enum {
    ESP_RST_UNKNOWN   = 0,
    ESP_RST_POWERON   = 1,
    ESP_RST_EXT       = 2,
    ESP_RST_SW        = 3,
    ESP_RST_PANIC     = 4,
    ESP_RST_INT_WDT   = 5,
    ESP_RST_TASK_WDT  = 6,
    ESP_RST_WDT       = 7,
    ESP_RST_DEEPSLEEP = 8,
    ESP_RST_BROWNOUT  = 9,
    ESP_RST_SDIO      = 10,
} esp_reset_reason_t;

inline esp_reset_reason_t esp_reset_reason() {
    return ESP_RST_POWERON;
}

extern "C" inline uint32_t esp_random() {
    return 0x12345678u;
}

// High-resolution timer — defined in Arduino.h mock to avoid multiple definitions
// extern "C" int64_t esp_timer_get_time();  // stub in Arduino.h
