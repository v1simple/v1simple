#pragma once
#ifndef NVS_H
#define NVS_H

#include <cstdint>
#include <cstddef>

using esp_err_t = int;

#ifndef ESP_OK
#define ESP_OK 0
#endif

struct nvs_stats_t {
    size_t used_entries = 0;
    size_t free_entries = 0;
    size_t total_entries = 0;
    size_t namespace_count = 0;
};

namespace mock_nvs {

inline nvs_stats_t& stats() {
    static nvs_stats_t g_stats{};
    return g_stats;
}

inline void reset() {
    stats() = nvs_stats_t{};
}

inline void set_stats(const nvs_stats_t& value) {
    stats() = value;
}

}  // namespace mock_nvs

inline esp_err_t nvs_get_stats(const char* /*part_name*/, nvs_stats_t* out_stats) {
    if (!out_stats) {
        return -1;
    }
    *out_stats = mock_nvs::stats();
    return ESP_OK;
}

#endif  // NVS_H
