#pragma once

#include <cstddef>
#include <cstdint>

struct Elm327ParseResult {
    bool valid = false;
    uint8_t service = 0;
    uint8_t pid = 0;
    uint16_t did = 0;
    uint8_t dataBytes[32] = {};
    uint8_t dataLen = 0;
    bool noData = false;
    bool error = false;
    bool busInit = false;
};

/// Parse an ELM327/STN2120 response line.
/// Handles "41 0D XX" (service 01 responses), "NO DATA", "?", and
/// "SEARCHING..." / "BUS INIT..." status messages.
/// Input is a null-terminated string (may include trailing \r\n).
Elm327ParseResult parseElm327Response(const char* response, size_t len);

/// Decode PID 0x0D (vehicle speed) from a parse result.
/// Returns speed in km/h, or -1.0f if the result is not a valid speed response.
float decodeSpeedKmh(const Elm327ParseResult& result);

/// Convert km/h to mph.
inline float kmhToMph(float kmh) {
    return kmh * 0.621371f;
}
