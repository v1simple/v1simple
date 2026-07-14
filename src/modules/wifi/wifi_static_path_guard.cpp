#include "wifi_static_path_guard.h"

#include <cstddef>
#include <cstring>

namespace WifiStaticPathGuard {

namespace {

bool startsWith(const char* path, const char* prefix) {
    return path != nullptr && std::strncmp(path, prefix, std::strlen(prefix)) == 0;
}

bool endsWith(const char* path, const char* suffix) {
    if (path == nullptr || suffix == nullptr) {
        return false;
    }

    const size_t pathLen = std::strlen(path);
    const size_t suffixLen = std::strlen(suffix);
    return pathLen >= suffixLen && std::strncmp(path + pathLen - suffixLen, suffix, suffixLen) == 0;
}

bool hasNestedPathAfterPrefix(const char* path, const char* prefix) {
    const char* rest = path + std::strlen(prefix);
    return std::strchr(rest, '/') != nullptr;
}

bool equalsAny(const char* path, const char* const* allowed, size_t count) {
    if (path == nullptr) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(path, allowed[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool isAllowedSingleFileAsset(const char* path, const char* prefix, const char* suffix) {
    return startsWith(path, prefix) && !hasNestedPathAfterPrefix(path, prefix) && endsWith(path, suffix);
}

} // namespace

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

bool isHtmlPagePath(const char* path) {
    static constexpr const char* kPagePaths[] = {
        "/",           "/index.html",   "/alp",           "/alp.html", "/audio",
        "/audio.html", "/autopush",     "/autopush.html", "/colors",   "/colors.html",
        "/devices",    "/devices.html", "/gps",           "/gps.html", "/obd",
        "/obd.html",   "/profiles",     "/profiles.html", "/settings", "/settings.html",
    };

    return isSafe(path) && equalsAny(path, kPagePaths, sizeof(kPagePaths) / sizeof(kPagePaths[0]));
}

bool isAllowedServedPath(const char* path) {
    if (!isSafe(path)) {
        return false;
    }

    if (isHtmlPagePath(path)) {
        return true;
    }

    if (std::strcmp(path, "/_app/env.js") == 0 || std::strcmp(path, "/_app/version.json") == 0 ||
        startsWith(path, "/_app/immutable/")) {
        return true;
    }

    if (isAllowedSingleFileAsset(path, "/audio/", ".mul") || isAllowedSingleFileAsset(path, "/branding/", ".png")) {
        return true;
    }

    return false;
}

} // namespace WifiStaticPathGuard
