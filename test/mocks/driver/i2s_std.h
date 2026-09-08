#pragma once

#include <cstddef>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>

using i2s_chan_handle_t = void*;
inline esp_err_t i2s_channel_write(i2s_chan_handle_t, const void*, size_t size, size_t* written, TickType_t) {
    *written = size;
    return ESP_OK;
}
