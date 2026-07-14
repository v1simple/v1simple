/**
 * WiFi manager support helpers split out of wifi_manager.cpp.
 *
 * Keeps filesystem/static-file serving and API-facing enum string helpers
 * separate from the main runtime/orchestration implementation.
 */

#include "wifi_manager_internals.h"
#include "client_write_retry.h"
#include "perf_metrics.h"
#include <LittleFS.h>
#include <cstring>

namespace {

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

} // namespace

bool serveLittleFSFileHelper(WebServer& server_, const char* path, const char* contentType) {
    uint32_t startUs = PERF_TIMESTAMP_US();
    String acceptEncoding = server_.header("Accept-Encoding");
    bool clientAcceptsGzip = acceptEncoding.indexOf("gzip") >= 0;
    const bool immutableCache = isImmutableWebAsset(path);

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
                perfRecordFsServeUs(PERF_TIMESTAMP_US() - startUs);
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
    perfRecordFsServeUs(PERF_TIMESTAMP_US() - startUs);
    return true;
}
