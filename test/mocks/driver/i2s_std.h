#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <driver/gpio.h>

using i2s_chan_handle_t = void*;
struct i2s_chan_config_t { bool auto_clear = false; };
struct i2s_std_config_t {
    int clk_cfg;
    int slot_cfg;
    struct {
        gpio_num_t mclk, bclk, ws, dout, din;
        struct { bool mclk_inv, bclk_inv, ws_inv; } invert_flags;
    } gpio_cfg;
};
#define I2S_CHANNEL_DEFAULT_CONFIG(a, b) i2s_chan_config_t{}
#define I2S_STD_CLK_DEFAULT_CONFIG(a) 0
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(a, b) 0

struct MockI2sState {
    unsigned createCalls = 0;
    unsigned writeCalls = 0;
    size_t writtenBytes = 0;
    std::deque<esp_err_t> createResults;
    std::deque<esp_err_t> writeResults;
    std::function<void()> onWrite;
};
inline MockI2sState g_mock_i2s_state{};

inline esp_err_t i2s_new_channel(const i2s_chan_config_t*, i2s_chan_handle_t* tx, void*) {
    ++g_mock_i2s_state.createCalls;
    esp_err_t result = ESP_OK;
    if (!g_mock_i2s_state.createResults.empty()) {
        result = g_mock_i2s_state.createResults.front();
        g_mock_i2s_state.createResults.pop_front();
    }
    if (result == ESP_OK) *tx = reinterpret_cast<void*>(1);
    return result;
}
inline esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t, const i2s_std_config_t*) { return ESP_OK; }
inline esp_err_t i2s_channel_enable(i2s_chan_handle_t) { return ESP_OK; }
inline esp_err_t i2s_del_channel(i2s_chan_handle_t) { return ESP_OK; }
inline esp_err_t i2s_channel_write(i2s_chan_handle_t, const void*, size_t size, size_t* written, TickType_t) {
    ++g_mock_i2s_state.writeCalls;
    if (g_mock_i2s_state.onWrite) g_mock_i2s_state.onWrite();
    esp_err_t result = ESP_OK;
    if (!g_mock_i2s_state.writeResults.empty()) {
        result = g_mock_i2s_state.writeResults.front();
        g_mock_i2s_state.writeResults.pop_front();
    }
    *written = result == ESP_OK ? size : 0;
    g_mock_i2s_state.writtenBytes += *written;
    return result;
}
