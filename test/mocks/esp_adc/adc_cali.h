// Mock esp-idf esp_adc/adc_cali.h for native unit tests
#pragma once
#include <cstdint>

typedef void* adc_cali_handle_t;

typedef enum {
    ADC_CALI_SCHEME_VER_LINE_FITTING = 1,
    ADC_CALI_SCHEME_VER_CURVE_FITTING = 2,
} adc_cali_scheme_ver_t;

inline bool adc_cali_check_scheme(adc_cali_scheme_ver_t*) { return false; }
inline int adc_cali_raw_to_voltage(adc_cali_handle_t, int raw, int* voltage) {
    if (voltage) *voltage = raw;
    return 0; // ESP_OK
}
inline int adc_cali_delete_scheme_line_fitting(adc_cali_handle_t) { return 0; }
inline int adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t) { return 0; }
