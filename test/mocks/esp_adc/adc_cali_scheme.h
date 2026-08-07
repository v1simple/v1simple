// Mock esp-idf esp_adc/adc_cali_scheme.h for native unit tests
#pragma once
#include "adc_oneshot.h"
#include "adc_cali.h"

typedef struct {
    adc_unit_t unit_id;
    adc_channel_t chan;
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
} adc_cali_curve_fitting_config_t;

inline int adc_cali_create_scheme_curve_fitting(
    const adc_cali_curve_fitting_config_t* /*config*/,
    adc_cali_handle_t* out_handle) {
    if (out_handle) *out_handle = (void*)1;
    return 0; // ESP_OK
}
