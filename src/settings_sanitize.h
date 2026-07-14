/**
 * Shared input-sanitization helpers used by both settings.cpp and wifi_manager.cpp.
 *
 * All functions are inline so including this header adds no linkage overhead.
 * Keep only truly-shared logic here; file-specific sanitizers stay in their
 * respective .cpp files.
 */

#pragma once

#include <Arduino.h>
#include <algorithm>
#include "settings.h" // V1Mode enum

// ── Length limits ────────────────────────────────────────────────────────────

inline constexpr size_t MAX_WIFI_SSID_LEN = 32;
inline constexpr size_t MAX_WIFI_PASSWORD_LEN = 63;
inline constexpr size_t MAX_WIFI_STA_LABEL_LEN = 32;
inline constexpr size_t MAX_AP_PASSWORD_LEN = 63;
inline constexpr size_t MIN_AP_PASSWORD_LEN = 8;
inline constexpr size_t MAX_PROXY_NAME_LEN = 32;
inline constexpr size_t MAX_SLOT_NAME_LEN = 20;
inline constexpr size_t MAX_PROFILE_NAME_LEN = 64;
inline constexpr size_t MAX_PROFILE_DESCRIPTION_LEN = 160;

// ── Numeric clamps ──────────────────────────────────────────────────────────

inline uint8_t clampU8(int value, int minVal, int maxVal) {
    return static_cast<uint8_t>(std::max(minVal, std::min(value, maxVal)));
}

inline uint32_t clampU32(int64_t value, uint32_t minVal, uint32_t maxVal) {
    if (value < static_cast<int64_t>(minVal)) {
        return minVal;
    }
    if (value > static_cast<int64_t>(maxVal)) {
        return maxVal;
    }
    return static_cast<uint32_t>(value);
}

inline uint8_t clampSlotVolumeValue(int value) {
    // 0xFF means "no change"; otherwise valid range is 0..9.
    if (value == 0xFF) {
        return 0xFF;
    }
    return clampU8(value, 0, 9);
}

inline uint8_t clampApTimeoutValue(int value) {
    // 0 means always-on, otherwise enforce 5..60 minutes.
    if (value == 0) {
        return 0;
    }
    return clampU8(value, 5, 60);
}

#ifndef CONNECTION_CYCLE_SETTINGS_CONSTANTS_DEFINED
#define CONNECTION_CYCLE_SETTINGS_CONSTANTS_DEFINED
inline constexpr uint32_t kConnectionCycleObdScanWindowMsDefault = 15000;
inline constexpr uint32_t kConnectionCycleObdScanWindowMsMin = 1000;
inline constexpr uint32_t kConnectionCycleObdScanWindowMsMax = 60000;

inline constexpr uint32_t kConnectionCycleObdRetryIntervalMsDefault = 120000;
inline constexpr uint32_t kConnectionCycleObdRetryIntervalMsMin = 30000;
inline constexpr uint32_t kConnectionCycleObdRetryIntervalMsMax = 600000;

inline constexpr uint32_t kConnectionCycleProxyOpenWindowMsDefault = 60000;
inline constexpr uint32_t kConnectionCycleProxyOpenWindowMsMin = 1000;
inline constexpr uint32_t kConnectionCycleProxyOpenWindowMsMax = 300000;

inline constexpr uint32_t kConnectionCycleWifiOpenTimeoutMsDefault = 30000;
inline constexpr uint32_t kConnectionCycleWifiOpenTimeoutMsMin = 1000;
inline constexpr uint32_t kConnectionCycleWifiOpenTimeoutMsMax = 120000;

inline constexpr uint32_t kConnectionCycleV1SettleQuietMsDefault = 500;
inline constexpr uint32_t kConnectionCycleV1SettleQuietMsMin = 100;
inline constexpr uint32_t kConnectionCycleV1SettleQuietMsMax = 5000;

inline constexpr uint32_t kConnectionCycleV1SettleFallbackMsDefault = 1500;
inline constexpr uint32_t kConnectionCycleV1SettleFallbackMsMin = 500;
inline constexpr uint32_t kConnectionCycleV1SettleFallbackMsMax = 10000;

inline constexpr uint32_t kConnectionCycleTeardownAckTimeoutMsDefault = 100;
inline constexpr uint32_t kConnectionCycleTeardownAckTimeoutMsMin = 25;
inline constexpr uint32_t kConnectionCycleTeardownAckTimeoutMsMax = 1000;
#endif

inline uint32_t clampConnectionCycleObdScanWindowMsValue(int64_t value) {
    return clampU32(value, kConnectionCycleObdScanWindowMsMin, kConnectionCycleObdScanWindowMsMax);
}

inline uint32_t clampConnectionCycleObdRetryIntervalMsValue(int64_t value) {
    return clampU32(value, kConnectionCycleObdRetryIntervalMsMin, kConnectionCycleObdRetryIntervalMsMax);
}

inline uint32_t clampConnectionCycleProxyOpenWindowMsValue(int64_t value) {
    return clampU32(value, kConnectionCycleProxyOpenWindowMsMin, kConnectionCycleProxyOpenWindowMsMax);
}

inline uint32_t clampConnectionCycleWifiOpenTimeoutMsValue(int64_t value) {
    return clampU32(value, kConnectionCycleWifiOpenTimeoutMsMin, kConnectionCycleWifiOpenTimeoutMsMax);
}

inline uint32_t clampConnectionCycleV1SettleQuietMsValue(int64_t value) {
    return clampU32(value, kConnectionCycleV1SettleQuietMsMin, kConnectionCycleV1SettleQuietMsMax);
}

inline uint32_t clampConnectionCycleV1SettleFallbackMsValue(int64_t value) {
    return clampU32(value, kConnectionCycleV1SettleFallbackMsMin, kConnectionCycleV1SettleFallbackMsMax);
}

inline uint32_t clampConnectionCycleTeardownAckTimeoutMsValue(int64_t value) {
    return clampU32(value, kConnectionCycleTeardownAckTimeoutMsMin, kConnectionCycleTeardownAckTimeoutMsMax);
}

inline uint32_t sanitizeGpsBaudValue(uint32_t raw) {
    // Allowed UART baud rates for GPS module.
    if (raw == 9600 || raw == 38400 || raw == 115200) {
        return raw;
    }
    return 9600; // Default to standard GPS baud on invalid value.
}

inline V1Mode normalizeV1ModeValue(int raw) {
    switch (raw) {
    case V1_MODE_UNKNOWN:
    case V1_MODE_ALL_BOGEYS:
    case V1_MODE_LOGIC:
    case V1_MODE_ADVANCED_LOGIC:
        return static_cast<V1Mode>(raw);
    default:
        return V1_MODE_UNKNOWN;
    }
}

// ── String sanitizers ───────────────────────────────────────────────────────

inline String clampStringLength(const String& value, size_t maxLen) {
    if (value.length() <= maxLen) {
        return value;
    }
    return value.substring(0, maxLen);
}

inline String sanitizeApSsidValue(const String& raw) {
    String value = clampStringLength(raw, MAX_WIFI_SSID_LEN);
    if (value.length() == 0) {
        return "V1-Simple";
    }
    return value;
}

inline String sanitizeWifiClientSsidValue(const String& raw) {
    return clampStringLength(raw, MAX_WIFI_SSID_LEN);
}

inline String sanitizeWifiClientPasswordValue(const String& raw) {
    return clampStringLength(raw, MAX_WIFI_PASSWORD_LEN);
}

inline String sanitizeWifiStaSlotLabelValue(const String& raw) {
    String value = clampStringLength(raw, MAX_WIFI_STA_LABEL_LEN);
    value.trim();
    return value;
}

inline String sanitizeProxyNameValue(const String& raw) {
    String value = clampStringLength(raw, MAX_PROXY_NAME_LEN);
    if (value.length() == 0) {
        return "V1-Proxy";
    }
    return value;
}

inline String sanitizeSlotNameValue(const String& raw) {
    String value = clampStringLength(raw, MAX_SLOT_NAME_LEN);
    value.toUpperCase();
    return value;
}

inline String sanitizeProfileNameValue(const String& raw) {
    return clampStringLength(raw, MAX_PROFILE_NAME_LEN);
}

inline String sanitizeProfileDescriptionValue(const String& raw) {
    return clampStringLength(raw, MAX_PROFILE_DESCRIPTION_LEN);
}

// ── BLE address validation ──────────────────────────────────────────────────

/**
 * Validate a BLE MAC address string format.
 *
 * Accepts either empty string (meaning "no saved device") or the standard
 * 17-character format AA:BB:CC:DD:EE:FF (uppercase hex with colons).
 * Does NOT normalize — use this as a gate, not a transform.
 *
 * @param address The address string to validate
 * @return true if empty or valid AA:BB:CC:DD:EE:FF format
 */
inline bool isValidBleAddress(const String& address) {
    if (address.length() == 0) {
        return true; // Empty means "no saved device" — valid state
    }
    if (address.length() != 17) {
        return false;
    }
    for (int i = 0; i < 17; ++i) {
        const char c = address[i];
        if ((i + 1) % 3 == 0) {
            if (c != ':')
                return false;
        } else {
            if (!isxdigit(static_cast<unsigned char>(c)))
                return false;
        }
    }
    return true;
}

// ── Color validation ────────────────────────────────────────────────────────

/**
 * Sanitize RGB565 color values loaded from NVS.
 *
 * Ensures color values are valid RGB565 16-bit colors with fallback defaults.
 * Also detects corrupted colors (e.g., 0x0000 black when they shouldn't be)
 * and prevents completely dark displays.
 *
 * @param raw The color value from NVS (may be corrupted)
 * @param defaultColor The fallback color if validation fails
 * @return Valid RGB565 color or defaultColor
 */
inline uint16_t sanitizeRgb565Color(uint16_t raw, uint16_t defaultColor) {
    // Mask to ensure valid RGB565 range (16-bit)
    // This is primarily defensive - should already be uint16_t.
    uint16_t masked = raw & 0xFFFF;

    // If completely black (0x0000), it's likely NVS corruption
    // since it's rarely a valid choice. Return default to prevent unreadable display.
    if (masked == 0x0000) {
        return defaultColor;
    }

    return masked;
}
