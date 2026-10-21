#pragma once
#ifndef NVS_H
#define NVS_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <cstring>
#include "Preferences.h"

using esp_err_t = int;

#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_ERR_NVS_NOT_FOUND
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#endif
#ifndef ESP_ERR_NVS_TYPE_MISMATCH
#define ESP_ERR_NVS_TYPE_MISMATCH 0x1103
#endif
#ifndef ESP_ERR_INVALID_ARG
#define ESP_ERR_INVALID_ARG 0x102
#endif

#ifndef ESP_ERR_INVALID_SIZE
#define ESP_ERR_INVALID_SIZE 0x104
#endif
#ifndef ESP_ERR_NO_MEM
#define ESP_ERR_NO_MEM 0x101
#endif
using nvs_type_t = PreferenceType;
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
    size_t skip = 0;
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

inline std::unordered_map<std::string, ProbeResponse>& uint32ProbeResults() {
    static std::unordered_map<std::string, ProbeResponse> g_results;
    return g_results;
}

inline std::unordered_map<nvs_handle_t, std::string>& handles() {
    static std::unordered_map<nvs_handle_t, std::string> g_handles;
    return g_handles;
}

inline std::unordered_map<std::string, ProbeResponse>& readResults() {
    static std::unordered_map<std::string, ProbeResponse> g_results;
    return g_results;
}

enum class ReadStage { Any, Presence, StringSize, StringValue, Scalar };
inline std::string readResultKey(const char* name, const char* key, ReadStage stage) {
    return std::string(name) + '\x1f' + key + '\x1f' + std::to_string(static_cast<int>(stage));
}
inline void set_read_result(const char* name, const char* key, esp_err_t result,
                            ReadStage stage = ReadStage::Any) {
    readResults()[readResultKey(name, key, stage)] = {result, false};
}
inline void set_nth_read_result(const char* name, const char* key, esp_err_t result,
                                ReadStage stage, size_t occurrence) {
    readResults()[readResultKey(name, key, stage)] = {result, true, occurrence - 1u};
}
inline esp_err_t readResult(const char* name, const char* key, ReadStage stage) {
    for (ReadStage candidate : {stage, ReadStage::Any}) {
        const auto it = readResults().find(readResultKey(name, key, candidate));
        if (it != readResults().end()) {
            if (it->second.skip > 0) { --it->second.skip; return ESP_OK; }
            const esp_err_t result = it->second.result;
            if (it->second.once) readResults().erase(it);
            return result;
        }
    }
    return ESP_OK;
}

inline std::string stringProbeKey(const char* name, const char* key) {
    return std::string(name ? name : "") + '\x1f' + (key ? key : "");
}

inline void reset() {
    stats() = nvs_stats_t{};
    namespaceOpenResults().clear();
    stringProbeResults().clear();
    uint32ProbeResults().clear();
    handles().clear();
    readResults().clear();
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
inline void set_nth_namespace_open_result(const char* name, esp_err_t result, size_t occurrence) {
    namespaceOpenResults()[name] = {result, true, occurrence - 1u};
}

inline void set_string_probe_result(const char* name, const char* key, esp_err_t result) {
    if (!name || !key) return;
    stringProbeResults()[stringProbeKey(name, key)] = {result, false};
}

inline void set_next_string_probe_result(const char* name, const char* key, esp_err_t result) {
    if (!name || !key) return;
    stringProbeResults()[stringProbeKey(name, key)] = {result, true};
}

inline void set_uint32_probe_result(const char* name, const char* key, esp_err_t result) {
    if (!name || !key) return;
    uint32ProbeResults()[stringProbeKey(name, key)] = {result, false};
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
        if (it->second.skip > 0) {
            --it->second.skip;
        } else {
            result = it->second.result;
            if (it->second.once) mock_nvs::namespaceOpenResults().erase(it);
        }
    }
    if (result == ESP_OK && mock_preferences::failBeginNamespace() == name) result = -1;
    if (result == ESP_OK) {
        static nvs_handle_t nextHandle = 0;
        *out_handle = ++nextHandle;
        mock_nvs::handles()[*out_handle] = name;
    } else {
        *out_handle = 0;
    }
    return result;
}

inline void nvs_close(nvs_handle_t handle) {
    mock_nvs::handles().erase(handle);
}

inline const std::string* mock_nvs_namespace(nvs_handle_t handle) {
    const auto it = mock_nvs::handles().find(handle);
    return it == mock_nvs::handles().end() ? nullptr : &it->second;
}

inline esp_err_t nvs_find_key(nvs_handle_t handle, const char* key, nvs_type_t* type) {
    const auto* ns = mock_nvs_namespace(handle);
    if (!ns || !key || !type) return ESP_ERR_INVALID_ARG;
    const auto status = mock_nvs::readResult(ns->c_str(), key, mock_nvs::ReadStage::Presence);
    if (status != ESP_OK) return status;
    const auto* entry = mock_preferences::find(*ns, key);
    if (!entry) return ESP_ERR_NVS_NOT_FOUND;
    *type = entry->type;
    return ESP_OK;
}

inline esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* out_value, size_t* length) {
    const auto* ns = mock_nvs_namespace(handle);
    if (!ns || !key || !length) return ESP_ERR_INVALID_ARG;
    esp_err_t result = mock_nvs::readResult(ns->c_str(), key,
        out_value ? mock_nvs::ReadStage::StringValue : mock_nvs::ReadStage::StringSize);
    const std::string probeKey = mock_nvs::stringProbeKey(ns->c_str(), key);
    const auto it = mock_nvs::stringProbeResults().find(probeKey);
    if (it != mock_nvs::stringProbeResults().end()) {
        result = it->second.result;
        if (it->second.once) mock_nvs::stringProbeResults().erase(it);
    }
    if (out_value) {
        ++mock_preferences::stringReadCounts()[key];
        if (mock_preferences::failStringReadKey() == key) result = -1;
    }
    if (result != ESP_OK) return result;
    const auto* entry = mock_preferences::find(*ns, key);
    if (!entry) return ESP_ERR_NVS_NOT_FOUND;
    if (entry->type != PT_STR) return ESP_ERR_NVS_TYPE_MISMATCH;
    const auto& text = std::get<std::string>(entry->value);
    if (out_value) {
        if (*length < text.size() + 1u) return ESP_ERR_INVALID_SIZE;
        std::memcpy(out_value, text.c_str(), text.size() + 1u);
    }
    *length = text.size() + 1u;
    return ESP_OK;
}

template <typename T>
inline esp_err_t mock_nvs_get_scalar(nvs_handle_t handle, const char* key, T* value, PreferenceType type) {
    const auto* ns = mock_nvs_namespace(handle);
    if (!ns || !key || !value) return ESP_ERR_INVALID_ARG;
    const auto status = mock_nvs::readResult(ns->c_str(), key, mock_nvs::ReadStage::Scalar);
    if (status != ESP_OK) return status;
    const auto* entry = mock_preferences::find(*ns, key);
    if (!entry) return ESP_ERR_NVS_NOT_FOUND;
    if (type == PT_U8 && entry->type == PT_BOOL) {
        *value = std::get<bool>(entry->value) ? 1 : 0;
        return ESP_OK;
    }
    if (entry->type != type) return ESP_ERR_NVS_TYPE_MISMATCH;
    if (std::holds_alternative<uint64_t>(entry->value)) *value = static_cast<T>(std::get<uint64_t>(entry->value));
    else *value = static_cast<T>(std::get<int64_t>(entry->value));
    return ESP_OK;
}
inline esp_err_t nvs_get_u32(nvs_handle_t handle, const char* key, uint32_t* value) {
    const auto* ns = mock_nvs_namespace(handle);
    if (!ns || !key || !value) return ESP_ERR_INVALID_ARG;
    const std::string probeKey = mock_nvs::stringProbeKey(ns->c_str(), key);
    const auto it = mock_nvs::uint32ProbeResults().find(probeKey);
    if (it != mock_nvs::uint32ProbeResults().end() && it->second.result != ESP_OK) return it->second.result;
    return mock_nvs_get_scalar(handle, key, value, PT_U32);
}
inline esp_err_t nvs_get_i8(nvs_handle_t h, const char* k, int8_t* v) { return mock_nvs_get_scalar(h, k, v, PT_I8); }
inline esp_err_t nvs_get_u8(nvs_handle_t h, const char* k, uint8_t* v) { return mock_nvs_get_scalar(h, k, v, PT_U8); }
inline esp_err_t nvs_get_u16(nvs_handle_t h, const char* k, uint16_t* v) { return mock_nvs_get_scalar(h, k, v, PT_U16); }
inline esp_err_t nvs_get_i32(nvs_handle_t h, const char* k, int32_t* v) { return mock_nvs_get_scalar(h, k, v, PT_I32); }
inline esp_err_t nvs_get_i64(nvs_handle_t h, const char* k, int64_t* v) { return mock_nvs_get_scalar(h, k, v, PT_I64); }

inline const char* esp_err_to_name(esp_err_t error) {
    if (error == ESP_OK) return "ESP_OK";
    if (error == ESP_ERR_NVS_NOT_FOUND) return "ESP_ERR_NVS_NOT_FOUND";
    if (error == ESP_ERR_INVALID_ARG) return "ESP_ERR_INVALID_ARG";
    return "MOCK_ESP_ERROR";
}

#endif  // NVS_H
