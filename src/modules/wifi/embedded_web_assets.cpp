#include "embedded_web_assets.h"
#include "embedded_web_assets.generated.h"
#include "wifi_static_path_guard.h"

#include <cstring>

namespace EmbeddedWebAssets {

const char* fingerprint() {
    return kBundleFingerprint;
}

bool isManagedPath(const char* path) {
    if (!WifiStaticPathGuard::isSafe(path)) {
        return false;
    }
    return WifiStaticPathGuard::isHtmlPagePath(path) || std::strncmp(path, "/_app/", 6) == 0 ||
           (std::strncmp(path, "/branding/", 10) == 0 && WifiStaticPathGuard::isAllowedServedPath(path));
}

const Asset* find(const char* path) {
    if (!path) {
        return nullptr;
    }
    for (size_t i = 0; i < sizeof(kAssets) / sizeof(kAssets[0]); ++i) {
        if (std::strcmp(path, kAssets[i].path) == 0) {
            return &kAssets[i];
        }
    }
    return nullptr;
}

} // namespace EmbeddedWebAssets
