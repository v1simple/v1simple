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

struct Elm327VinParseResult {
    bool valid = false;
    bool noData = false;
    bool error = false;
    char vin[18] = {};
};

enum class Elm327TempDecodeFormat : uint8_t {
    U8_OFFSET40 = 0,
    U16_DIV10_OFFSET40 = 1,
    U16_RAW_OFFSET40 = 2,
};

/// Parse an ELM327/STN2120 response line.
/// Handles "41 0D XX" (service 01 responses), "NO DATA", "?", and
/// "SEARCHING..." / "BUS INIT..." status messages.
/// Input is a null-terminated string (may include trailing \r\n).
Elm327ParseResult parseElm327Response(const char* response, size_t len);

/// Parse a multi-line VIN response for mode 09 PID 02.
Elm327VinParseResult parseVinResponse(const char* response, size_t len);

/// Decode PID 0x0D (vehicle speed) from a parse result.
/// Returns speed in km/h, or -1.0f if the result is not a valid speed response.
float decodeSpeedKmh(const Elm327ParseResult& result);

bool decodeTempC_x10(const Elm327ParseResult& result,
                     Elm327TempDecodeFormat format,
                     int16_t& tempC_x10Out);

/// Convert km/h to mph.
inline float kmhToMph(float kmh) {
    return kmh * 0.621371f;
}
