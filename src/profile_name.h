#pragma once

#include <Arduino.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ff.h>

inline constexpr size_t MAX_PROFILE_NAME_LEN = 64;

enum class ProfileNameStatus : uint8_t {
    Valid = 0,
    Empty,
    TooLong,
    Hidden,
    PathLike,
    InvalidCharacter,
};

inline ProfileNameStatus canonicalizeProfileName(const String& raw, String& canonical) {
    canonical = raw;
    canonical.trim();
    if (canonical.length() == 0) {
        return ProfileNameStatus::Empty;
    }
    if (canonical.length() > MAX_PROFILE_NAME_LEN) {
        return ProfileNameStatus::TooLong;
    }
    if (canonical[0] == '.' || canonical[0] == '_') {
        return ProfileNameStatus::Hidden;
    }
    if (canonical.indexOf('/') >= 0 || canonical.indexOf('\\') >= 0 || canonical.indexOf("..") >= 0) {
        return ProfileNameStatus::PathLike;
    }
    for (size_t i = 0; i < canonical.length(); ++i) {
        const unsigned char c = static_cast<unsigned char>(canonical[i]);
        if (c < 0x20 || c == 0x7f || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
            c == '|') {
            return ProfileNameStatus::InvalidCharacter;
        }
    }
    return ProfileNameStatus::Valid;
}

inline const char* profileNameStatusMessage(ProfileNameStatus status) {
    switch (status) {
    case ProfileNameStatus::Valid:
        return "";
    case ProfileNameStatus::Empty:
        return "Profile name is empty";
    case ProfileNameStatus::TooLong:
        return "Profile name exceeds 64 UTF-8 bytes";
    case ProfileNameStatus::Hidden:
        return "Profile name cannot begin with dot or underscore";
    case ProfileNameStatus::PathLike:
        return "Profile name cannot contain path characters";
    case ProfileNameStatus::InvalidCharacter:
        return "Profile name contains an invalid character";
    }
    return "Invalid profile name";
}

// Profile names must coexist on every supported store, including SD/FatFS
// after an offline LittleFS edit. Use the filesystem's actual Unicode mapping,
// not locale case conversion or byte-length equality (case can change width).
// Names are validated UTF-8 before admission; decoding remains bounded even
// when this helper receives malformed input from a recovery candidate.
inline uint32_t nextProfileNameCodePoint(const String& text, size_t& offset) {
    const uint8_t first = static_cast<uint8_t>(text[offset++]);
    if (first < 0x80u) return first;
    const unsigned count = first < 0xe0u ? 1u : first < 0xf0u ? 2u : 3u;
    uint32_t point = first & (0x7fu >> count);
    for (unsigned index = 0; index < count && offset < text.length(); ++index) {
        point = (point << 6u) | (static_cast<uint8_t>(text[offset++]) & 0x3fu);
    }
    return point;
}

inline bool profileCanonicalNamesCollide(const String& lhs, const String& rhs) {
    size_t left = 0;
    size_t right = 0;
    while (left < lhs.length() && right < rhs.length()) {
        if (ff_wtoupper(nextProfileNameCodePoint(lhs, left)) !=
            ff_wtoupper(nextProfileNameCodePoint(rhs, right))) return false;
    }
    return left == lhs.length() && right == rhs.length();
}

// Copy the longest whole-code-point UTF-8 prefix that fits in maxBytes.
// Callers use this only after the source has passed the shared UTF-8 validator.
inline bool copyProfileUtf8Prefix(const String& source, size_t maxBytes, String& output) {
    size_t retained = source.length();
    if (retained > maxBytes) {
        retained = maxBytes;
        while (retained > 0 &&
               (static_cast<unsigned char>(source[retained]) & 0xC0u) == 0x80u) {
            --retained;
        }
    }
    output = source.substring(0, retained);
    return output.length() == retained &&
           std::memcmp(output.c_str(), source.c_str(), retained) == 0;
}

// Deterministic legacy-migration naming shared in shape with usb_profiles.py.
// fixedSuffix (for example " - Slot 2") and the collision suffix are always
// retained; only the UTF-8 stem is shortened, and never inside a code point.
inline bool buildMigratedProfileNameCandidate(const String& rawStem, const char* fixedSuffix,
                                              unsigned collisionOrdinal, String& output) {
    if (!fixedSuffix || collisionOrdinal == 0 || collisionOrdinal >= 1000) return false;
    String stem;
    stem = rawStem;
    if (stem.length() != rawStem.length() || stem != rawStem) return false;
    stem.trim();
    if (stem.length() == 0) {
        stem = "Auto-Push profile";
        if (stem.length() != std::strlen("Auto-Push profile")) return false;
    }

    char collisionSuffix[8] = {};
    size_t collisionLength = 0;
    if (collisionOrdinal > 1) {
        const int written = std::snprintf(collisionSuffix, sizeof(collisionSuffix), " #%u", collisionOrdinal);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(collisionSuffix)) return false;
        collisionLength = static_cast<size_t>(written);
    }
    const size_t fixedLength = std::strlen(fixedSuffix);
    if (fixedLength + collisionLength >= MAX_PROFILE_NAME_LEN) return false;

    if (!copyProfileUtf8Prefix(stem, MAX_PROFILE_NAME_LEN - fixedLength - collisionLength, output)) {
        return false;
    }
    const size_t expectedLength = output.length() + fixedLength + collisionLength;
    output += fixedSuffix;
    if (collisionLength > 0) output += collisionSuffix;
    return output.length() == expectedLength;
}
