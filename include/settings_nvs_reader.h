#pragma once

#include <Arduino.h>
#include <nvs.h>
#include <cstring>
#include <memory>
#include <new>

// Preferences getters erase the distinction between a missing key and a failed
// read. A settings snapshot may default only proven-missing legacy fields.
class SettingsNvsReader {
public:
    explicit SettingsNvsReader(const char* ns) : error_(nvs_open(ns, NVS_READONLY, &handle_)) {}
    ~SettingsNvsReader() { if (handle_) nvs_close(handle_); }
    SettingsNvsReader(const SettingsNvsReader&) = delete;
    SettingsNvsReader& operator=(const SettingsNvsReader&) = delete;

    bool good() const { return error_ == ESP_OK; }
    esp_err_t error() const { return error_; }
    void requireExactString(bool exact) { if (!exact && good()) error_ = ESP_ERR_NO_MEM; }

    bool isKey(const char* key) {
        nvs_type_t type;
        return accept(nvs_find_key(handle_, key, &type));
    }
    int8_t getChar(const char* key, int8_t fallback = 0) { return scalar(key, fallback, nvs_get_i8); }
    uint8_t getUChar(const char* key, uint8_t fallback = 0) { return scalar(key, fallback, nvs_get_u8); }
    uint16_t getUShort(const char* key, uint16_t fallback = 0) { return scalar(key, fallback, nvs_get_u16); }
    int32_t getInt(const char* key, int32_t fallback = 0) { return scalar(key, fallback, nvs_get_i32); }
    uint32_t getUInt(const char* key, uint32_t fallback = 0) { return scalar(key, fallback, nvs_get_u32); }
    int64_t getLong64(const char* key, int64_t fallback = 0) { return scalar(key, fallback, nvs_get_i64); }
    bool getBool(const char* key, bool fallback = false) { return getUChar(key, fallback ? 1 : 0) == 1; }

    String getString(const char* key, const char* fallback = "") {
        size_t bytes = 0;
        if (!accept(nvs_get_str(handle_, key, nullptr, &bytes))) return exactString(fallback);
        // ESP-IDF NVS strings have a 4000-byte limit including their terminator.
        if (bytes == 0 || bytes > 4000) { error_ = ESP_ERR_INVALID_SIZE; return {}; }
        std::unique_ptr<char[]> buffer(new (std::nothrow) char[bytes]);
        if (!buffer) { error_ = ESP_ERR_NO_MEM; return {}; }
        const size_t expectedBytes = bytes;
        const esp_err_t status = nvs_get_str(handle_, key, buffer.get(), &bytes);
        // A key disappearing between the size and data reads is not an absent
        // optional field: the snapshot has changed and must be retried.
        if (status != ESP_OK) { error_ = status; return {}; }
        if (bytes != expectedBytes || buffer[bytes - 1] != '\0' || std::strlen(buffer.get()) + 1 != bytes) {
            error_ = ESP_ERR_INVALID_SIZE;
            return {};
        }
        return exactString(buffer.get());
    }

private:
    bool accept(esp_err_t status) {
        if (status != ESP_OK && status != ESP_ERR_NVS_NOT_FOUND && good()) error_ = status;
        return status == ESP_OK;
    }
    String exactString(const char* bytes) {
        String value(bytes);
        if (value.length() != std::strlen(bytes)) error_ = ESP_ERR_NO_MEM;
        return value;
    }
    template <typename T> T scalar(const char* key, T fallback, esp_err_t (*read)(nvs_handle_t, const char*, T*)) {
        T value = fallback;
        return accept(read(handle_, key, &value)) ? value : fallback;
    }
    nvs_handle_t handle_ = 0;
    esp_err_t error_ = ESP_OK;
};
