#pragma once

#include <cctype>
#include <cstddef>
#include <cstring>

namespace ObdStringUtils {

inline void copyString(char* dest, size_t destLen, const char* src) {
    if (!dest || destLen == 0) return;
    dest[0] = '\0';
    if (!src) return;
    strncpy(dest, src, destLen - 1);
    dest[destLen - 1] = '\0';
}

inline bool stringContainsCI(const char* haystack, const char* needle) {
    if (!haystack || !needle || needle[0] == '\0') return false;
    const size_t needleLen = strlen(needle);
    const size_t haystackLen = strlen(haystack);
    if (needleLen > haystackLen) return false;

    for (size_t offset = 0; offset + needleLen <= haystackLen; ++offset) {
        bool matches = true;
        for (size_t i = 0; i < needleLen; ++i) {
            const char lhs = static_cast<char>(
                toupper(static_cast<unsigned char>(haystack[offset + i])));
            const char rhs = static_cast<char>(
                toupper(static_cast<unsigned char>(needle[i])));
            if (lhs != rhs) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

inline size_t commandDisplayLen(const char* command) {
    if (!command) return 0;
    size_t len = strlen(command);
    while (len > 0 && (command[len - 1] == '\r' || command[len - 1] == '\n')) {
        --len;
    }
    return len;
}

}  // namespace ObdStringUtils
