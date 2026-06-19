// Mock esp_adc/adc_oneshot.h for native unit testing
#pragma once

#include <cstdint>

typedef int adc_oneshot_unit_handle_t;
typedef int adc_channel_t;
typedef int adc_atten_t;
typedef int adc_bitwidth_t;
typedef int adc_unit_t;

#define ADC_UNIT_1          0
#define ADC_UNIT_2          1
#define ADC_ATTEN_DB_0      0
#define ADC_ATTEN_DB_2_5    1
#define ADC_ATTEN_DB_6      2
#define ADC_ATTEN_DB_11     3
#define ADC_ATTEN_DB_12     ADC_ATTEN_DB_11
#define ADC_RTC_CLK_SRC_DEFAULT  0
#define ADC_ULP_MODE_DISABLE     0
#define ADC_BITWIDTH_DEFAULT 12
#define ADC_BITWIDTH_12     12
#define ADC_CHANNEL_0       0
#define ADC_CHANNEL_1       1
#define ADC_CHANNEL_2       2
#define ADC_CHANNEL_3       3
#define ADC_CHANNEL_4       4

typedef struct {
    adc_unit_t unit_id;
    int clk_src;   // adc_oneshot_clk_src_t
    int ulp_mode;  // adc_ulp_mode_t
} adc_oneshot_unit_init_cfg_t;

typedef struct {
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
} adc_oneshot_chan_cfg_t;

#define ESP_OK 0
typedef int esp_err_t;

inline esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t*, adc_oneshot_unit_handle_t* handle) {
    if (handle) *handle = 1;
    return ESP_OK;
}
inline esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t, adc_channel_t, const adc_oneshot_chan_cfg_t*) {
    return ESP_OK;
}
inline esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t, adc_channel_t, int* out_raw) {
    if (out_raw) *out_raw = 2048;
    return ESP_OK;
}
inline esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t) { return ESP_OK; }
