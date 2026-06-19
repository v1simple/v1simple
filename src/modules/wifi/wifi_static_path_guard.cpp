#include "wifi_static_path_guard.h"

#include <cstddef>

namespace WifiStaticPathGuard {

bool isSafe(const char* path) {
    if (path == nullptr || path[0] != '/') {
        return false;
    }

    const char* segmentStart = path + 1;
    for (const char* cursor = path + 1;; ++cursor) {
        const char ch = *cursor;
        if (ch == '\\') {
            return false;
        }

        if (ch == '/' || ch == '\0') {
            const size_t segmentLen = static_cast<size_t>(cursor - segmentStart);
            if (segmentLen == 2 && segmentStart[0] == '.' && segmentStart[1] == '.') {
                return false;
            }
            if (ch == '\0') {
                break;
            }
            segmentStart = cursor + 1;
        }
    }

    return true;
}

}  // namespace WifiStaticPathGuard
