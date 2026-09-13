#pragma once
#ifndef NVS_H
#define NVS_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>

using esp_err_t = int;

#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_ERR_NVS_NOT_FOUND
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#endif
#ifndef ESP_ERR_INVALID_ARG
#define ESP_ERR_INVALID_ARG 0x102
#endif

using nvs_handle_t = uint32_t;
enum nvs_open_mode_t : uint8_t {
    NVS_READONLY,
    NVS_READWRITE,
};

struct nvs_stats_t {
    size_t used_entries = 0;
    size_t free_entries = 0;
    size_t total_entries = 0;
    size_t namespace_count = 0;
};

namespace mock_nvs {

struct ProbeResponse {
    esp_err_t result = ESP_OK;
    bool once = false;
};

inline nvs_stats_t& stats() {
    static nvs_stats_t g_stats{};
    return g_stats;
}

inline std::unordered_map<std::string, ProbeResponse>& namespaceOpenResults() {
    static std::unordered_map<std::string, ProbeResponse> g_results;
    return g_results;
}

inline std::unordered_map<std::string, ProbeResponse>& stringProbeResults() {
    static std::unordered_map<std::string, ProbeResponse> g_results;
    return g_results;
}

inline std::string& openNamespace() {
    static std::string g_namespace;
    return g_namespace;
}

inline std::string stringProbeKey(const char* name, const char* key) {
    return std::string(name ? name : "") + '\x1f' + (key ? key : "");
}

inline void reset() {
    stats() = nvs_stats_t{};
    namespaceOpenResults().clear();
    stringProbeResults().clear();
    openNamespace().clear();
}

inline void set_stats(const nvs_stats_t& value) {
    stats() = value;
}

inline void set_namespace_open_result(const char* name, esp_err_t result) {
    if (!name) return;
    namespaceOpenResults()[name] = {result, false};
}

inline void set_next_namespace_open_result(const char* name, esp_err_t result) {
    if (!name) return;
    namespaceOpenResults()[name] = {result, true};
}

inline void set_string_probe_result(const char* name, const char* key, esp_err_t result) {
    if (!name || !key) return;
    stringProbeResults()[stringProbeKey(name, key)] = {result, false};
}

inline void set_next_string_probe_result(const char* name, const char* key, esp_err_t result) {
    if (!name || !key) return;
    stringProbeResults()[stringProbeKey(name, key)] = {result, true};
}

}  // namespace mock_nvs

inline esp_err_t nvs_get_stats(const char* /*part_name*/, nvs_stats_t* out_stats) {
    if (!out_stats) {
        return -1;
    }
    *out_stats = mock_nvs::stats();
    return ESP_OK;
}

inline esp_err_t nvs_open(const char* name, nvs_open_mode_t /*open_mode*/, nvs_handle_t* out_handle) {
    if (!name || !out_handle) return ESP_ERR_INVALID_ARG;
    esp_err_t result = ESP_OK;
    const auto it = mock_nvs::namespaceOpenResults().find(name);
    if (it != mock_nvs::namespaceOpenResults().end()) {
        result = it->second.result;
        if (it->second.once) mock_nvs::namespaceOpenResults().erase(it);
    }
    if (result == ESP_OK) {
        mock_nvs::openNamespace() = name;
        *out_handle = 1u;
    } else {
        *out_handle = 0;
    }
    return result;
}

inline void nvs_close(nvs_handle_t handle) {
    if (handle == 1u) mock_nvs::openNamespace().clear();
}

inline esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* /*out_value*/, size_t* length) {
    if (handle != 1u || mock_nvs::openNamespace().empty() || !key || !length) {
        return ESP_ERR_INVALID_ARG;
    }
    const std::string probeKey = mock_nvs::stringProbeKey(mock_nvs::openNamespace().c_str(), key);
    esp_err_t result = ESP_OK;
    const auto it = mock_nvs::stringProbeResults().find(probeKey);
    if (it != mock_nvs::stringProbeResults().end()) {
        result = it->second.result;
        if (it->second.once) mock_nvs::stringProbeResults().erase(it);
    }
    if (result == ESP_OK) *length = 1;
    return result;
}

inline const char* esp_err_to_name(esp_err_t error) {
    if (error == ESP_OK) return "ESP_OK";
    if (error == ESP_ERR_NVS_NOT_FOUND) return "ESP_ERR_NVS_NOT_FOUND";
    if (error == ESP_ERR_INVALID_ARG) return "ESP_ERR_INVALID_ARG";
    return "MOCK_ESP_ERROR";
}

#endif  // NVS_H
