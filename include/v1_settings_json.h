#pragma once

#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>

namespace V1SettingsJson {

constexpr size_t kSettingsByteCount = 6;

inline bool parseRawBytes(const JsonVariantConst& value, uint8_t (&out)[kSettingsByteCount]) {
    if (!value.is<JsonArrayConst>()) {
        return false;
    }

    const JsonArrayConst values = value.as<JsonArrayConst>();
    if (values.size() != kSettingsByteCount) {
        return false;
    }

    uint8_t parsed[kSettingsByteCount];
    for (size_t i = 0; i < kSettingsByteCount; ++i) {
        const JsonVariantConst element = values[i];
        if (!element.is<uint8_t>()) {
            return false;
        }
        parsed[i] = element.as<uint8_t>();
    }

    for (size_t i = 0; i < kSettingsByteCount; ++i) {
        out[i] = parsed[i];
    }
    return true;
}

} // namespace V1SettingsJson
