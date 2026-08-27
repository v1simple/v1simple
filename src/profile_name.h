#pragma once

#include <Arduino.h>
#include <cctype>

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
        return "Profile name exceeds 64 characters";
    case ProfileNameStatus::Hidden:
        return "Profile name cannot begin with dot or underscore";
    case ProfileNameStatus::PathLike:
        return "Profile name cannot contain path characters";
    case ProfileNameStatus::InvalidCharacter:
        return "Profile name contains an invalid character";
    }
    return "Invalid profile name";
}

inline String profileCanonicalCollisionKey(const String& canonical) {
    String key = canonical;
    key.toLowerCase();
    return key;
}
