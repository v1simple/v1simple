// Mock driver/rtc_io.h for native unit tests
#pragma once
#include <cstdint>
#include "gpio.h"

inline bool rtc_gpio_is_valid_gpio(int /*gpio_num*/) { return true; }
inline int rtc_gpio_pullup_en(int /*gpio_num*/) { return 0; }
inline int rtc_gpio_pulldown_en(int /*gpio_num*/) { return 0; }
inline int rtc_gpio_pullup_dis(int /*gpio_num*/) { return 0; }
inline int rtc_gpio_pulldown_dis(int /*gpio_num*/) { return 0; }
