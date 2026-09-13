#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wifi_maintenance_write_policy.h"
#include "v1_profile_limits.h"

namespace WifiMaintenanceHttpPreflight {

constexpr size_t kMaxHeaderBytes = 2048;
constexpr size_t kMaxBodyBytes = 128u * 1024u;
constexpr size_t kMaxLegacyMultipartBodyBytes = 4u * 1024u;
constexpr size_t kMaxDetectorOperationBodyBytes = 256u;
constexpr size_t kMaxMultipartBoundaryBytes = 70u;

enum class BodyEncoding : uint8_t {
    Other = 0,
    UrlEncoded,
    MultipartFormData,
};

// Fixed-size metadata copied from the admission pass. The WebServer parser
// must not be trusted to reconstruct either value through heap-backed String
// operations before the project-owned raw-body handler runs.
struct BodyInfo {
    size_t contentLength = 0;
    BodyEncoding encoding = BodyEncoding::Other;
    char multipartBoundary[kMaxMultipartBoundaryBytes + 1u]{};
    size_t multipartBoundaryLength = 0;
};

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

inline bool isBoundaryTokenByte(const char value) {
    return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') || value == '\'' || value == '(' || value == ')' ||
           value == '+' || value == '_' || value == ',' || value == '-' || value == '.' ||
           value == '/' || value == ':' || value == '=' || value == '?';
}

inline bool parseMultipartFormDataBoundary(const char* begin, const char* end,
                                           const char*& boundaryBegin, const char*& boundaryEnd) {
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
    if (cursor == end) return false;
    if (*cursor == '"') {
        boundaryBegin = ++cursor;
        while (cursor < end && *cursor != '"') {
            if (!isBoundaryTokenByte(*cursor)) return false;
            ++cursor;
        }
        if (cursor == end || cursor == boundaryBegin || ++cursor != end) return false;
        boundaryEnd = cursor - 1;
    } else {
        boundaryBegin = cursor;
        while (cursor < end && isBoundaryTokenByte(*cursor)) ++cursor;
        boundaryEnd = cursor;
        if (boundaryBegin == boundaryEnd || cursor != end) return false;
    }
    return static_cast<size_t>(boundaryEnd - boundaryBegin) <= kMaxMultipartBoundaryBytes;
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

inline bool isExactFormPath(const char* begin, const char* end) {
    return equalsExact(begin, end, "/api/device/settings") ||
           equalsExact(begin, end, "/api/autopush/activate") ||
           equalsExact(begin, end, "/api/autopush/slot") ||
           equalsExact(begin, end, "/api/autopush/push") ||
           equalsExact(begin, end, "/api/v1/apply") ||
           equalsExact(begin, end, "/api/v1/factory-reset") ||
           equalsExact(begin, end, "/api/v1/devices/name") ||
           equalsExact(begin, end, "/api/v1/devices/profile") ||
           equalsExact(begin, end, "/api/v1/devices/delete");
}

inline size_t bodyLimitForPath(const char* begin, const char* end) {
    const char* pathEnd = end;
    for (const char* cursor = begin; cursor < end; ++cursor) {
        if (*cursor == '?') {
            pathEnd = cursor;
            break;
        }
    }
    if (equalsExact(begin, pathEnd, "/api/v1/profile")) return V1_PROFILE_HTTP_SAVE_MAX_BYTES;
    if (equalsExact(begin, pathEnd, "/api/v1/profile/delete")) return V1_PROFILE_HTTP_DELETE_MAX_BYTES;
    if (equalsExact(begin, pathEnd, "/api/autopush/push") ||
        equalsExact(begin, pathEnd, "/api/v1/apply") ||
        equalsExact(begin, pathEnd, "/api/v1/factory-reset")) {
        return kMaxDetectorOperationBodyBytes;
    }
    if (equalsExact(begin, pathEnd, "/api/settings/restore")) return kMaxBodyBytes;
    // Every other write route uses WebServer's form parser.  Bound that path
    // independently of Content-Type: urlencoded and plain bodies allocate the
    // same framework Strings as the legacy multipart compatibility path.
    return kMaxLegacyMultipartBodyBytes;
}

inline Decision applyWriteAdmission(const Decision decision, const bool admitted) {
    return decision == Decision::AllowBodyParsing && !admitted ? Decision::RejectRateLimited : decision;
}

inline Decision evaluate(const char* data, const size_t length, const bool maintenanceBootMode,
                         BodyInfo* const bodyInfo = nullptr) {
    if (bodyInfo) *bodyInfo = BodyInfo{};
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
    const char* targetPathEnd = targetEnd;
    for (const char* cursor = targetBegin; cursor < targetEnd; ++cursor) {
        if (*cursor == '?') {
            targetPathEnd = cursor;
            break;
        }
    }
    // Exact form routes take all mutation parameters from the byte-preserved
    // body. Reject a query component so framework argument allocation cannot
    // silently drop a query-only optional field while the body still commits.
    if (targetPathEnd != targetEnd && isExactFormPath(targetBegin, targetPathEnd)) {
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
                if (next < contentLength || next > bodyLimitForPath(targetBegin, targetEnd)) {
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
    const bool exactFormPath = isExactFormPath(targetBegin, targetPathEnd);
    if (foundContentType && startsWithIgnoreCase(contentTypeBegin, contentTypeEnd, "multipart/")) {
        // App-only upgrades preserve the v2.0.3 LittleFS UI, whose string-only
        // forms use multipart. Bound that compatibility path tightly because
        // pinned WebServer grows String form fields while parsing multipart.
        const char* boundaryBegin = nullptr;
        const char* boundaryEnd = nullptr;
        if (!equalsIgnoreCase(data, methodEnd, "POST") || !isLegacyMultipartPath(targetBegin, targetEnd) ||
            !parseMultipartFormDataBoundary(contentTypeBegin, contentTypeEnd, boundaryBegin, boundaryEnd)) {
            return Decision::RejectMultipart;
        }
        if (contentLength > kMaxLegacyMultipartBodyBytes) {
            return Decision::RejectTooLarge;
        }
        if (bodyInfo && exactFormPath) {
            bodyInfo->encoding = BodyEncoding::MultipartFormData;
            bodyInfo->multipartBoundaryLength = static_cast<size_t>(boundaryEnd - boundaryBegin);
            for (size_t i = 0; i < bodyInfo->multipartBoundaryLength; ++i)
                bodyInfo->multipartBoundary[i] = boundaryBegin[i];
            bodyInfo->multipartBoundary[bodyInfo->multipartBoundaryLength] = '\0';
        }
    } else if (bodyInfo && exactFormPath) {
        bodyInfo->encoding = BodyEncoding::UrlEncoded;
    }
    if (bodyInfo) bodyInfo->contentLength = contentLength;
    return Decision::AllowBodyParsing;
}

} // namespace WifiMaintenanceHttpPreflight
