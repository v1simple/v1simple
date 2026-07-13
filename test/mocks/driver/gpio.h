// Mock driver/gpio.h for native unit tests
#pragma once
#include <cstdint>

typedef enum {
    GPIO_NUM_NC  = -1,
    GPIO_NUM_0   = 0,
    GPIO_NUM_1   = 1,
    GPIO_NUM_2   = 2,
    GPIO_NUM_3   = 3,
    GPIO_NUM_4   = 4,
    GPIO_NUM_5   = 5,
    GPIO_NUM_6   = 6,
    GPIO_NUM_7   = 7,
    GPIO_NUM_8   = 8,
    GPIO_NUM_9   = 9,
    GPIO_NUM_10  = 10,
    GPIO_NUM_11  = 11,
    GPIO_NUM_12  = 12,
    GPIO_NUM_13  = 13,
    GPIO_NUM_14  = 14,
    GPIO_NUM_15  = 15,
    GPIO_NUM_16  = 16,
    GPIO_NUM_17  = 17,
    GPIO_NUM_18  = 18,
    GPIO_NUM_19  = 19,
    GPIO_NUM_20  = 20,
    GPIO_NUM_21  = 21,
    GPIO_NUM_38  = 38,
    GPIO_NUM_39  = 39,
    GPIO_NUM_40  = 40,
    GPIO_NUM_41  = 41,
    GPIO_NUM_42  = 42,
    GPIO_NUM_43  = 43,
    GPIO_NUM_44  = 44,
    GPIO_NUM_45  = 45,
    GPIO_NUM_46  = 46,
    GPIO_NUM_47  = 47,
    GPIO_NUM_48  = 48,
} gpio_num_t;

typedef enum {
    GPIO_MODE_INPUT           = 1,
    GPIO_MODE_OUTPUT          = 2,
    GPIO_MODE_OUTPUT_OD       = 6,
    GPIO_MODE_INPUT_OUTPUT_OD = 7,
    GPIO_MODE_INPUT_OUTPUT    = 3,
} gpio_mode_t;

typedef enum {
    GPIO_PULLUP_DISABLE  = 0,
    GPIO_PULLUP_ENABLE   = 1,
} gpio_pullup_t;

typedef enum {
    GPIO_PULLDOWN_DISABLE = 0,
    GPIO_PULLDOWN_ENABLE  = 1,
} gpio_pulldown_t;

typedef enum {
    GPIO_INTR_DISABLE   = 0,
    GPIO_INTR_POSEDGE   = 1,
    GPIO_INTR_NEGEDGE   = 2,
    GPIO_INTR_ANYEDGE   = 3,
    GPIO_INTR_LOW_LEVEL = 4,
    GPIO_INTR_HIGH_LEVEL= 5,
} gpio_int_type_t;

typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

inline int gpio_config(const gpio_config_t*) { return 0; }
inline int gpio_set_direction(gpio_num_t, gpio_mode_t) { return 0; }
inline int gpio_set_level(gpio_num_t, uint32_t) { return 0; }
inline int gpio_get_level(gpio_num_t) { return 0; }
inline int gpio_set_pull_mode(gpio_num_t, int) { return 0; }
inline int gpio_reset_pin(gpio_num_t) { return 0; }
inline int gpio_hold_en(gpio_num_t) { return 0; }
inline int gpio_hold_dis(gpio_num_t) { return 0; }
inline void gpio_deep_sleep_hold_en() {}
inline void gpio_deep_sleep_hold_dis() {}
