#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wifi_maintenance_write_policy.h"

namespace WifiMaintenanceHttpPreflight {

constexpr size_t kMaxHeaderBytes = 2048;
constexpr size_t kMaxBodyBytes = 128u * 1024u;
constexpr size_t kMaxLegacyMultipartBodyBytes = 4u * 1024u;

enum class Decision : uint8_t {
    NeedMoreHeaders,
    AllowFrameworkParsing,
    AllowBodyParsing,
    RejectForbidden,
    RejectRateLimited,
    RejectLengthRequired,
    RejectTooLarge,
    RejectMultipart,
    RejectBadRequest,
    RejectHeadersTooLarge,
};

inline char asciiLower(const char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

inline bool equalsIgnoreCase(const char* begin, const char* end, const char* expected) {
    if (!begin || !end || !expected) {
        return false;
    }
    while (begin < end && *expected != '\0') {
        if (asciiLower(*begin++) != asciiLower(*expected++)) {
            return false;
        }
    }
    return begin == end && *expected == '\0';
}

inline bool equalsExact(const char* begin, const char* end, const char* expected) {
    if (!begin || !end || !expected) {
        return false;
    }
    while (begin < end && *expected != '\0') {
        if (*begin++ != *expected++) {
            return false;
        }
    }
    return begin == end && *expected == '\0';
}

inline bool startsWithIgnoreCase(const char* begin, const char* end, const char* expected) {
    if (!begin || !end || !expected) {
        return false;
    }
    while (*expected != '\0') {
        if (begin == end || asciiLower(*begin++) != asciiLower(*expected++)) {
            return false;
        }
    }
    return true;
}

inline bool isMultipartFormDataWithBoundary(const char* begin, const char* end) {
    constexpr char kMediaType[] = "multipart/form-data";
    constexpr char kBoundary[] = "boundary=";
    if (!startsWithIgnoreCase(begin, end, kMediaType)) {
        return false;
    }
    const char* cursor = begin + sizeof(kMediaType) - 1;
    while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
        ++cursor;
    }
    if (cursor == end || *cursor++ != ';') {
        return false;
    }
    while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
        ++cursor;
    }
    if (!startsWithIgnoreCase(cursor, end, kBoundary)) {
        return false;
    }
    cursor += sizeof(kBoundary) - 1;
    return cursor < end;
}

inline void trim(const char*& begin, const char*& end) {
    while (begin < end && (*begin == ' ' || *begin == '\t')) {
        ++begin;
    }
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) {
        --end;
    }
}

inline const char* findBytes(const char* data, const size_t length, const char* needle, const size_t needleLength) {
    if (!data || !needle || needleLength == 0 || length < needleLength) {
        return nullptr;
    }
    for (size_t offset = 0; offset <= length - needleLength; ++offset) {
        size_t i = 0;
        while (i < needleLength && data[offset + i] == needle[i]) {
            ++i;
        }
        if (i == needleLength) {
            return data + offset;
        }
    }
    return nullptr;
}

inline bool isBodyMethod(const char* begin, const char* end) {
    return equalsIgnoreCase(begin, end, "POST") || equalsIgnoreCase(begin, end, "PUT") ||
           equalsIgnoreCase(begin, end, "PATCH") || equalsIgnoreCase(begin, end, "DELETE");
}

inline bool isLegacyMultipartPath(const char* begin, const char* end) {
    return equalsExact(begin, end, "/api/device/settings") ||
           equalsExact(begin, end, "/api/obd/devices/name") ||
           equalsExact(begin, end, "/api/autopush/activate") ||
           equalsExact(begin, end, "/api/autopush/slot") ||
           equalsExact(begin, end, "/api/v1/devices/name") ||
           equalsExact(begin, end, "/api/v1/devices/profile") ||
           equalsExact(begin, end, "/api/v1/devices/delete");
}

inline Decision applyWriteAdmission(const Decision decision, const bool admitted) {
    return decision == Decision::AllowBodyParsing && !admitted ? Decision::RejectRateLimited : decision;
}

inline Decision evaluate(const char* data, const size_t length, const bool maintenanceBootMode) {
    const char* const headersEnd = findBytes(data, length, "\r\n\r\n", 4);
    if (!headersEnd) {
        return length >= kMaxHeaderBytes ? Decision::RejectHeadersTooLarge : Decision::NeedMoreHeaders;
    }

    const char* const requestLineEnd = findBytes(data, static_cast<size_t>(headersEnd - data), "\r\n", 2);
    if (!requestLineEnd) {
        return Decision::RejectBadRequest;
    }
    const char* const methodEnd = findBytes(data, static_cast<size_t>(requestLineEnd - data), " ", 1);
    if (!methodEnd || methodEnd == data) {
        return Decision::RejectBadRequest;
    }
    if (!isBodyMethod(data, methodEnd)) {
        return Decision::AllowFrameworkParsing;
    }
    const char* const targetBegin = methodEnd + 1;
    const char* const targetEnd = findBytes(
        targetBegin, static_cast<size_t>(requestLineEnd - targetBegin), " ", 1);
    if (!targetEnd || targetEnd == targetBegin) {
        return Decision::RejectBadRequest;
    }
    if (!maintenanceBootMode) {
        return Decision::RejectForbidden;
    }

    bool foundContentLength = false;
    bool foundRequestShape = false;
    bool foundContentType = false;
    size_t contentLength = 0;
    const char* contentTypeBegin = nullptr;
    const char* contentTypeEnd = nullptr;

    const char* line = requestLineEnd + 2;
    while (line < headersEnd) {
        // Include the first CRLF of the terminating CRLFCRLF so the final
        // header line has an in-range delimiter.
        const char* const lineEnd = findBytes(line, static_cast<size_t>((headersEnd + 2) - line), "\r\n", 2);
        if (!lineEnd) {
            return Decision::RejectBadRequest;
        }
        const char* const colon = findBytes(line, static_cast<size_t>(lineEnd - line), ":", 1);
        if (!colon) {
            return Decision::RejectBadRequest;
        }
        const char* nameBegin = line;
        const char* nameEnd = colon;
        const char* valueBegin = colon + 1;
        const char* valueEnd = lineEnd;
        trim(nameBegin, nameEnd);
        trim(valueBegin, valueEnd);

        if (equalsIgnoreCase(nameBegin, nameEnd, "Content-Length")) {
            if (foundContentLength || valueBegin == valueEnd) {
                return Decision::RejectBadRequest;
            }
            foundContentLength = true;
            for (const char* digit = valueBegin; digit < valueEnd; ++digit) {
                if (*digit < '0' || *digit > '9') {
                    return Decision::RejectBadRequest;
                }
                const size_t next = contentLength * 10u + static_cast<size_t>(*digit - '0');
                if (next < contentLength || next > kMaxBodyBytes) {
                    return Decision::RejectTooLarge;
                }
                contentLength = next;
            }
        } else if (equalsIgnoreCase(nameBegin, nameEnd, WifiMaintenanceWritePolicy::kRequestShapeHeader)) {
            if (foundRequestShape ||
                !equalsIgnoreCase(valueBegin, valueEnd, WifiMaintenanceWritePolicy::kRequestShapeValue)) {
                return Decision::RejectForbidden;
            }
            foundRequestShape = true;
        } else if (equalsIgnoreCase(nameBegin, nameEnd, "Content-Type")) {
            if (foundContentType) {
                return Decision::RejectBadRequest;
            }
            foundContentType = true;
            contentTypeBegin = valueBegin;
            contentTypeEnd = valueEnd;
        } else if (equalsIgnoreCase(nameBegin, nameEnd, "Transfer-Encoding")) {
            return Decision::RejectBadRequest;
        }
        line = lineEnd + 2;
    }

    if (!foundRequestShape) {
        return Decision::RejectForbidden;
    }
    if (!foundContentLength) {
        return Decision::RejectLengthRequired;
    }
    if (foundContentType && startsWithIgnoreCase(contentTypeBegin, contentTypeEnd, "multipart/")) {
        // App-only upgrades preserve the v2.0.3 LittleFS UI, whose string-only
        // forms use multipart. Bound that compatibility path tightly because
        // pinned WebServer grows String form fields while parsing multipart.
        if (!equalsIgnoreCase(data, methodEnd, "POST") || !isLegacyMultipartPath(targetBegin, targetEnd) ||
            !isMultipartFormDataWithBoundary(contentTypeBegin, contentTypeEnd)) {
            return Decision::RejectMultipart;
        }
        if (contentLength > kMaxLegacyMultipartBodyBytes) {
            return Decision::RejectTooLarge;
        }
    }
    (void)contentLength;
    return Decision::AllowBodyParsing;
}

} // namespace WifiMaintenanceHttpPreflight
