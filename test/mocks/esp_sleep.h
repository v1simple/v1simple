// Mock esp_sleep.h for native unit tests
#pragma once
#include <cstdint>

typedef enum {
    ESP_EXT1_WAKEUP_ALL_LOW   = 0,
    ESP_EXT1_WAKEUP_ANY_HIGH  = 1,
    ESP_EXT1_WAKEUP_ANY_LOW   = 2,
} esp_sleep_ext1_wakeup_mode_t;

inline int esp_sleep_enable_ext1_wakeup(uint64_t, esp_sleep_ext1_wakeup_mode_t) { return 0; }
inline int esp_sleep_enable_timer_wakeup(uint64_t) { return 0; }
inline void esp_deep_sleep_start() {}
inline void esp_light_sleep_start() {}
