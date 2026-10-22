/**
 * WiFi static assets: the web interface comes from firmware, audio from LittleFS.
 */

#include "wifi_manager_internals.h"
#include "client_write_retry.h"
#include "modules/wifi/embedded_web_assets.h"
#include "modules/wifi/wifi_static_path_guard.h"
#include <LittleFS.h>
#include <cstdlib>
#include <cstring>

namespace {

bool acceptsGzip(String encodings) {
    encodings.toLowerCase();
    bool wildcard = false;
    const char* token = encodings.c_str();
    while (*token) {
        const char* end = std::strchr(token, ',');
        if (!end) end = token + std::strlen(token);
        const char* parameters = static_cast<const char*>(std::memchr(token, ';', end - token));
        const char* nameEnd = parameters ? parameters : end;
        while (token < nameEnd && (*token == ' ' || *token == '\t')) ++token;
        while (nameEnd > token && (nameEnd[-1] == ' ' || nameEnd[-1] == '\t')) --nameEnd;
        bool allowed = true;
        if (parameters) {
            const char* quality = parameters + 1;
            while (quality < end && (*quality == ' ' || *quality == '\t')) ++quality;
            if (end - quality >= 2 && quality[0] == 'q' && quality[1] == '=') {
                char* qualityEnd = nullptr;
                const double value = std::strtod(quality + 2, &qualityEnd);
                while (qualityEnd < end && (*qualityEnd == ' ' || *qualityEnd == '\t')) ++qualityEnd;
                allowed = qualityEnd == end && value > 0.0 && value <= 1.0;
            }
        }
        if (nameEnd - token == 4 && std::strncmp(token, "gzip", 4) == 0) return allowed;
        if (nameEnd - token == 1 && *token == '*') wildcard = allowed;
        token = *end ? end + 1 : end;
    }
    return wildcard;
}

bool isImmutableWebAsset(const char* path) {
    constexpr const char* kImmutablePrefix = "/_app/immutable/";
    return path != nullptr && std::strncmp(path, kImmutablePrefix, std::strlen(kImmutablePrefix)) == 0;
}

String makeImmutableAssetEtag(const char* path, size_t fileSize, bool gzip) {
    return String("\"") + String(path) + (gzip ? ".gz-" : "-") + String(fileSize) + String("\"");
}

void sendStaticCacheHeaders(WebServer& server_, bool gzip, bool immutableCache, const String& etag) {
    server_.sendHeader("Vary", "Accept-Encoding");
    if (gzip) {
        server_.sendHeader("Content-Encoding", "gzip");
    }

    if (immutableCache) {
        server_.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
        server_.sendHeader("ETag", etag);
        return;
    }

    server_.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    server_.sendHeader("Pragma", "no-cache");
    server_.sendHeader("Expires", "0");
}

bool streamOpenFile(WebServer& server_, File& file, const char* path, const char* contentType, size_t fileSize,
                    bool gzip, bool immutableCache, const String& etag) {
    constexpr size_t kStreamChunkBytes = 256;
    server_.setContentLength(fileSize);
    sendStaticCacheHeaders(server_, gzip, immutableCache, etag);
    server_.send(200, contentType, "");

    auto client = server_.client();
    // WiFi static-file serving is Tier 5. Prefer smaller loopTask chunks over
    // a large transient stack buffer on the WebServer path.
    uint8_t buf[kStreamChunkBytes];
    size_t bytesSent = 0;
    while (file.available()) {
        const size_t len = file.read(buf, sizeof(buf));
        if (len == 0) {
            break;
        }
        if (!client_write_retry::writeAll(client, buf, len)) {
            client.stop(); // Fail-fast on short/partial stream writes.
            Serial.printf("[HTTP] WARN stream failed %s (%u/%u bytes)\n", path, static_cast<unsigned>(bytesSent),
                          static_cast<unsigned>(fileSize));
            return false;
        }
        bytesSent += len;
    }

    if (bytesSent != fileSize) {
        client.stop();
        Serial.printf("[HTTP] WARN stream short %s (%u/%u bytes)\n", path, static_cast<unsigned>(bytesSent),
                      static_cast<unsigned>(fileSize));
        return false;
    }

    return true;
}

bool streamEmbeddedAsset(WebServer& server_, const EmbeddedWebAssets::Asset& asset, const char* path,
                         const char* contentType, bool gzip, bool immutableCache, const String& etag) {
    constexpr size_t kStreamChunkBytes = 256;
    server_.setContentLength(asset.size);
    sendStaticCacheHeaders(server_, gzip, immutableCache, etag);
    server_.send(200, contentType, "");

    auto client = server_.client();
    size_t bytesSent = 0;
    while (bytesSent < asset.size) {
        const size_t remaining = asset.size - bytesSent;
        const size_t length = remaining < kStreamChunkBytes ? remaining : kStreamChunkBytes;
        if (!client_write_retry::writeAll(client, asset.data + bytesSent, length)) {
            client.stop();
            Serial.printf("[HTTP] WARN embedded stream failed %s (%u/%u bytes)\n", path,
                          static_cast<unsigned>(bytesSent), static_cast<unsigned>(asset.size));
            return false;
        }
        bytesSent += length;
    }
    return true;
}

} // namespace

bool serveLittleFSFileHelper(WebServer& server_, const char* path, const char* contentType) {
    if (!WifiStaticPathGuard::isAllowedServedPath(path)) {
        return false;
    }
    const bool clientAcceptsGzip = acceptsGzip(server_.header("Accept-Encoding"));
    const bool immutableCache = isImmutableWebAsset(path);

    if (EmbeddedWebAssets::isManagedPath(path)) {
        const EmbeddedWebAssets::Asset* asset = nullptr;
        bool gzip = false;
        if (clientAcceptsGzip) {
            const String gzPath = String(path) + ".gz";
            asset = EmbeddedWebAssets::find(gzPath.c_str());
            gzip = asset != nullptr;
        }
        if (!asset) asset = EmbeddedWebAssets::find(path);
        // Never revive stale HTML or old hashed assets from a previous installation.
        if (!asset) return false;
        const String etagPath = String(EmbeddedWebAssets::fingerprint()) + path;
        const String etag = makeImmutableAssetEtag(etagPath.c_str(), asset->size, gzip);
        if (immutableCache && server_.header("If-None-Match") == etag) {
            sendStaticCacheHeaders(server_, false, true, etag);
            server_.send(304, contentType, "");
            return true;
        }
        // A stream failure already closes the connection after the 200 headers.
        // Do not append an error response or try filesystem fallback.
        (void)streamEmbeddedAsset(server_, *asset, path, contentType, gzip, immutableCache, etag);
        return true;
    }

    if (clientAcceptsGzip) {
        String gzPath = String(path) + ".gz";
        if (LittleFS.exists(gzPath.c_str())) {
            File file = LittleFS.open(gzPath.c_str(), "r");
            if (file) {
                size_t fileSize = file.size();
                String etag = makeImmutableAssetEtag(path, fileSize, true);
                if (immutableCache && server_.header("If-None-Match") == etag) {
                    sendStaticCacheHeaders(server_, false, true, etag);
                    server_.send(304, contentType, "");
                    file.close();
                    return true;
                }
                const bool streamOk =
                    streamOpenFile(server_, file, path, contentType, fileSize, true, immutableCache, etag);
                file.close();
                if (!streamOk) {
                    server_.send(500, "text/plain", "Stream error");
                    return true;
                }
                return true;
            }
        }
    }

    File file = LittleFS.open(path, "r");
    if (!file) {
        return false;
    }
    size_t fileSize = file.size();
    String etag = makeImmutableAssetEtag(path, fileSize, false);
    if (immutableCache && server_.header("If-None-Match") == etag) {
        sendStaticCacheHeaders(server_, false, true, etag);
        server_.send(304, contentType, "");
        file.close();
        return true;
    }
    const bool streamOk = streamOpenFile(server_, file, path, contentType, fileSize, false, immutableCache, etag);
    file.close();
    if (!streamOk) {
        server_.send(500, "text/plain", "Stream error");
        return true;
    }
    return true;
}
