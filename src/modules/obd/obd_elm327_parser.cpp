#include "obd_elm327_parser.h"

#include <cctype>
#include <cstring>

namespace {

constexpr size_t kMaxNormalizedLines = 12;
constexpr size_t kMaxLineLength = 96;
constexpr size_t kMaxPayloadBytes = 48;

bool isTrimChar(char c) {
    return c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '>';
}

const char* trimResponse(const char* response, size_t len, size_t& trimmedLen) {
    size_t start = 0;
    while (start < len && isTrimChar(response[start])) {
        ++start;
    }

    size_t end = len;
    while (end > start && isTrimChar(response[end - 1])) {
        --end;
    }

    trimmedLen = end - start;
    return response + start;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int hexByte(const char* s) {
    const int hi = hexDigit(s[0]);
    const int lo = hexDigit(s[1]);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

bool startsWithCI(const char* s, size_t sLen, const char* prefix) {
    const size_t pLen = strlen(prefix);
    if (sLen < pLen) return false;
    for (size_t i = 0; i < pLen; ++i) {
        if (toupper(static_cast<unsigned char>(s[i])) !=
            toupper(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

bool equalsCI(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (toupper(static_cast<unsigned char>(*a)) !=
            toupper(static_cast<unsigned char>(*b))) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

void trimLineInPlace(char* line) {
    if (!line) return;

    size_t len = strlen(line);
    size_t start = 0;
    while (start < len && isTrimChar(line[start])) {
        ++start;
    }

    size_t end = len;
    while (end > start && isTrimChar(line[end - 1])) {
        --end;
    }

    if (start > 0 && start < len) {
        memmove(line, line + start, end - start);
    }

    line[end - start] = '\0';
}

bool containsDigit(const char* s) {
    if (!s) return false;
    for (; *s; ++s) {
        if (isdigit(static_cast<unsigned char>(*s))) {
            return true;
        }
    }
    return false;
}

bool isLikelyEchoLine(const char* line) {
    if (!line || line[0] == '\0') return false;
    if (startsWithCI(line, strlen(line), "AT")) return true;
    if (startsWithCI(line, strlen(line), "ST")) return true;

    char compact[kMaxLineLength] = {};
    size_t compactLen = 0;
    for (const char* p = line; *p; ++p) {
        if (*p == ' ') continue;
        if (!isxdigit(static_cast<unsigned char>(*p))) return false;
        if (compactLen + 1 >= sizeof(compact)) return false;
        compact[compactLen++] = *p;
    }
    if (compactLen < 2 || (compactLen % 2) != 0) {
        return false;
    }

    const int firstByte = hexByte(compact);
    if (firstByte < 0) {
        return false;
    }

    switch (firstByte) {
        case 0x41:
        case 0x49:
        case 0x61:
        case 0x62:
            return false;
        case 0x01:
        case 0x09:
        case 0x21:
        case 0x22:
            return true;
        default:
            return compactLen <= 8;
    }
}

bool isStatusLine(const char* line) {
    if (!line || line[0] == '\0') return false;
    return startsWithCI(line, strlen(line), "SEARCHING") ||
           startsWithCI(line, strlen(line), "BUS INIT") ||
           startsWithCI(line, strlen(line), "OK") ||
           startsWithCI(line, strlen(line), "ELM327") ||
           startsWithCI(line, strlen(line), "OBDLINK") ||
           startsWithCI(line, strlen(line), "STN");
}

bool isErrorLine(const char* line) {
    if (!line || line[0] == '\0') return false;
    return equalsCI(line, "?") ||
           startsWithCI(line, strlen(line), "UNABLE TO CONNECT") ||
           startsWithCI(line, strlen(line), "CAN ERROR") ||
           startsWithCI(line, strlen(line), "BUFFER FULL") ||
           startsWithCI(line, strlen(line), "BUS ERROR") ||
           startsWithCI(line, strlen(line), "ERROR") ||
           startsWithCI(line, strlen(line), "STOPPED");
}

bool isNoDataLine(const char* line) {
    return line && startsWithCI(line, strlen(line), "NO DATA");
}

bool nextRawLine(const char* trimmed,
                 size_t trimmedLen,
                 size_t& cursor,
                 const char*& lineStart,
                 size_t& lineLen) {
    if (!trimmed || cursor >= trimmedLen) {
        return false;
    }

    size_t start = cursor;
    size_t end = start;
    while (end < trimmedLen && trimmed[end] != '\r' && trimmed[end] != '\n') {
        ++end;
    }

    lineStart = trimmed + start;
    lineLen = end - start;
    cursor = end;
    while (cursor < trimmedLen && (trimmed[cursor] == '\r' || trimmed[cursor] == '\n')) {
        ++cursor;
    }
    return true;
}

void copyNormalizedLine(const char* rawLine, size_t rawLen, char line[kMaxLineLength]) {
    const size_t copyLen = (rawLen < (kMaxLineLength - 1)) ? rawLen : (kMaxLineLength - 1);
    memcpy(line, rawLine, copyLen);
    line[copyLen] = '\0';
    trimLineInPlace(line);
}

bool hasNormalizedLineOverflow(const char* trimmed, size_t trimmedLen) {
    size_t cursor = 0;
    size_t lineCount = 0;
    char line[kMaxLineLength] = {};
    while (cursor < trimmedLen) {
        const char* rawLine = nullptr;
        size_t rawLen = 0;
        if (!nextRawLine(trimmed, trimmedLen, cursor, rawLine, rawLen)) {
            break;
        }

        if (lineCount >= kMaxNormalizedLines) {
            return true;
        }

        copyNormalizedLine(rawLine, rawLen, line);
        if (line[0] != '\0') {
            ++lineCount;
        }
    }

    return false;
}

// Normalize one line at a time so the OBD update path avoids a large stack
// resident line matrix while preserving the existing line filtering semantics.
bool nextNormalizedLine(const char* trimmed,
                        size_t trimmedLen,
                        size_t& cursor,
                        char line[kMaxLineLength]) {
    while (cursor < trimmedLen) {
        const char* rawLine = nullptr;
        size_t rawLen = 0;
        if (!nextRawLine(trimmed, trimmedLen, cursor, rawLine, rawLen)) {
            return false;
        }

        copyNormalizedLine(rawLine, rawLen, line);
        if (line[0] != '\0') {
            return true;
        }
    }

    return false;
}

bool collectHexBytesFromLine(const char* rawLine,
                             uint8_t* out,
                             size_t outCap,
                             size_t& outLen,
                             bool allowColonPrefix) {
    if (!rawLine || !out || outCap == 0) return false;

    const char* line = rawLine;
    if (allowColonPrefix) {
        const char* colon = strchr(rawLine, ':');
        if (colon && colon != rawLine && !containsDigit(colon + 1)) {
            return false;
        }
        if (colon && colon != rawLine) {
            bool digitsOnly = true;
            for (const char* p = rawLine; p < colon; ++p) {
                if (!isdigit(static_cast<unsigned char>(*p)) && *p != ' ') {
                    digitsOnly = false;
                    break;
                }
            }
            if (digitsOnly) {
                line = colon + 1;
            }
        }
    }

    size_t lineLen = strlen(line);
    size_t i = 0;
    while (i < lineLen) {
        while (i < lineLen && line[i] == ' ') {
            ++i;
        }
        if (i >= lineLen) {
            break;
        }
        if (i + 1 >= lineLen || outLen >= outCap) {
            return false;
        }
        const int value = hexByte(line + i);
        if (value < 0) {
            return false;
        }
        out[outLen++] = static_cast<uint8_t>(value);
        i += 2;
        while (i < lineLen && line[i] == ' ') {
            ++i;
        }
    }

    return true;
}

bool collectNormalizedHexPayload(const char* trimmed,
                                 size_t trimmedLen,
                                 uint8_t* out,
                                 size_t outCap,
                                 size_t& outLen,
                                 bool& sawBusInit,
                                 bool& sawNoData,
                                 bool& sawError) {
    outLen = 0;
    sawBusInit = false;
    sawNoData = false;
    sawError = false;

    if (hasNormalizedLineOverflow(trimmed, trimmedLen)) {
        sawError = true;
        return false;
    }

    size_t cursor = 0;
    char line[kMaxLineLength] = {};
    while (nextNormalizedLine(trimmed, trimmedLen, cursor, line)) {
        if (isNoDataLine(line)) {
            sawNoData = true;
            continue;
        }
        if (isErrorLine(line)) {
            sawError = true;
            continue;
        }
        if (startsWithCI(line, strlen(line), "SEARCHING") ||
            startsWithCI(line, strlen(line), "BUS INIT")) {
            sawBusInit = true;
            continue;
        }
        if (isLikelyEchoLine(line)) {
            continue;
        }
        if (isStatusLine(line)) {
            continue;
        }

        size_t before = outLen;
        if (!collectHexBytesFromLine(line, out, outCap, outLen, true)) {
            sawError = true;
            return false;
        }
        if (outLen == before) {
            sawError = true;
            return false;
        }
    }

    return outLen > 0;
}

bool decodePrintableVin(const uint8_t* bytes, size_t len, char vin[18]) {
    if (!bytes || !vin) return false;
    if (len != 17) return false;

    for (size_t i = 0; i < len; ++i) {
        const uint8_t c = bytes[i];
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
        vin[i] = static_cast<char>(c);
    }
    vin[17] = '\0';
    return true;
}

}  // namespace

Elm327ParseResult parseElm327Response(const char* response, size_t len) {
    Elm327ParseResult result;

    if (response == nullptr || len == 0) {
        result.error = true;
        return result;
    }

    size_t trimmedLen = 0;
    const char* trimmed = trimResponse(response, len, trimmedLen);
    if (trimmedLen == 0) {
        result.error = true;
        return result;
    }

    uint8_t bytes[kMaxPayloadBytes] = {};
    size_t byteCount = 0;
    bool sawBusInit = false;
    bool sawNoData = false;
    bool sawError = false;

    const bool hasPayload = collectNormalizedHexPayload(
        trimmed, trimmedLen, bytes, sizeof(bytes), byteCount, sawBusInit, sawNoData, sawError);

    result.busInit = sawBusInit;
    if (sawBusInit && !hasPayload && !sawNoData && !sawError) {
        return result;
    }
    if (sawNoData && !hasPayload) {
        result.noData = true;
        return result;
    }
    if (sawError && !hasPayload) {
        result.error = true;
        return result;
    }
    if (!hasPayload) {
        result.error = true;
        return result;
    }

    if (byteCount < 2) {
        result.error = true;
        return result;
    }

    result.valid = true;
    result.service = bytes[0];
    if (result.service == 0x62) {
        if (byteCount < 3) {
            result.error = true;
            result.valid = false;
            return result;
        }
        result.did = static_cast<uint16_t>((bytes[1] << 8) | bytes[2]);
        result.pid = bytes[2];
        result.dataLen = static_cast<uint8_t>((byteCount > 3) ? (byteCount - 3) : 0);
        const size_t copyLen = (result.dataLen < sizeof(result.dataBytes))
                                   ? result.dataLen
                                   : sizeof(result.dataBytes);
        if (copyLen > 0) {
            memcpy(result.dataBytes, bytes + 3, copyLen);
        }
        return result;
    }

    result.pid = bytes[1];
    result.dataLen = static_cast<uint8_t>((byteCount > 2) ? (byteCount - 2) : 0);
    const size_t copyLen = (result.dataLen < sizeof(result.dataBytes))
                               ? result.dataLen
                               : sizeof(result.dataBytes);
    if (copyLen > 0) {
        memcpy(result.dataBytes, bytes + 2, copyLen);
    }

    return result;
}

Elm327VinParseResult parseVinResponse(const char* response, size_t len) {
    Elm327VinParseResult result;
    if (response == nullptr || len == 0) {
        result.error = true;
        return result;
    }

    size_t trimmedLen = 0;
    const char* trimmed = trimResponse(response, len, trimmedLen);
    if (trimmedLen == 0) {
        result.error = true;
        return result;
    }

    if (hasNormalizedLineOverflow(trimmed, trimmedLen)) {
        result.error = true;
        return result;
    }

    uint8_t vinBytes[24] = {};
    size_t vinLen = 0;
    bool sawHexFrame = false;

    size_t cursor = 0;
    char line[kMaxLineLength] = {};
    while (nextNormalizedLine(trimmed, trimmedLen, cursor, line)) {
        if (isNoDataLine(line)) {
            result.noData = true;
            continue;
        }
        if (isErrorLine(line)) {
            result.error = true;
            continue;
        }
        if (startsWithCI(line, strlen(line), "SEARCHING") ||
            startsWithCI(line, strlen(line), "BUS INIT") ||
            isStatusLine(line)) {
            continue;
        }

        uint8_t bytes[kMaxPayloadBytes] = {};
        size_t byteCount = 0;
        if (!collectHexBytesFromLine(line, bytes, sizeof(bytes), byteCount, true) ||
            byteCount == 0) {
            result.error = true;
            continue;
        }

        sawHexFrame = true;
        size_t payloadStart = 0;
        if (byteCount >= 3 && bytes[0] == 0x49 && bytes[1] == 0x02) {
            payloadStart = 3;
        } else if (vinLen > 0) {
            payloadStart = 0;
        } else {
            continue;
        }

        for (size_t idx = payloadStart; idx < byteCount && vinLen < sizeof(vinBytes); ++idx) {
            vinBytes[vinLen++] = bytes[idx];
        }
    }

    if (result.noData && !sawHexFrame) {
        result.valid = false;
        result.error = false;
        return result;
    }
    if (!sawHexFrame || vinLen != 17 || !decodePrintableVin(vinBytes, vinLen, result.vin)) {
        result.valid = false;
        if (!result.noData) {
            result.error = true;
        }
        return result;
    }

    result.valid = true;
    result.noData = false;
    result.error = false;
    return result;
}

float decodeSpeedKmh(const Elm327ParseResult& result) {
    if (!result.valid || result.service != 0x41 || result.pid != 0x0D || result.dataLen < 1) {
        return -1.0f;
    }
    return static_cast<float>(result.dataBytes[0]);
}

bool decodeTempC_x10(const Elm327ParseResult& result,
                     Elm327TempDecodeFormat format,
                     int16_t& tempC_x10Out) {
    if (!result.valid) {
        return false;
    }

    int32_t value = 0;
    switch (format) {
        case Elm327TempDecodeFormat::U8_OFFSET40:
            if (result.dataLen < 1) return false;
            value = static_cast<int32_t>(result.dataBytes[0]) * 10 - 400;
            break;

        case Elm327TempDecodeFormat::U16_DIV10_OFFSET40:
            if (result.dataLen < 2) return false;
            value = static_cast<int32_t>((result.dataBytes[0] << 8) | result.dataBytes[1]);
            value = value / 10 - 400;
            break;

        case Elm327TempDecodeFormat::U16_RAW_OFFSET40:
            if (result.dataLen < 2) return false;
            value = static_cast<int32_t>((result.dataBytes[0] << 8) | result.dataBytes[1]) - 400;
            break;
    }

    if (value < INT16_MIN || value > INT16_MAX) {
        return false;
    }

    tempC_x10Out = static_cast<int16_t>(value);
    return true;
}
