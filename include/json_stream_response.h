#pragma once
/**
 * Streaming JSON helper — sends JSON directly to the WiFi client
 * without allocating an intermediate Arduino String.
 *
 * Saves ~3-15 KB of internal SRAM per API response depending on
 * document size. Avoids heap fragmentation from short-lived String
 * allocations during WiFi request handling.
 *
 * Usage:
 *   JsonDocument doc;
 *   doc["key"] = "value";
 *   sendJsonStream(server, doc);          // 200 OK
 *   sendJsonStream(server, doc, 400);     // custom status code
 */

#include <ArduinoJson.h>
#include <WebServer.h>
#include <stdint.h>
#include <string.h>
#if defined(UNIT_TEST)
#include <string>
#else
#include "client_write_retry.h"
#endif

#if !defined(UNIT_TEST)
namespace json_stream_detail {

// Buffered Print adapter to avoid byte-at-a-time TCP writes from serializeJson().
template <typename ClientT, size_t kBufferSize = 512>
class BufferedClientPrint final : public Print {
public:
    explicit BufferedClientPrint(ClientT& client) : client_(client) {}

    size_t write(uint8_t c) override { return write(&c, 1); }

    size_t write(const uint8_t* data, size_t size) override {
        if (!data || size == 0 || failed_) {
            return 0;
        }

        size_t accepted = 0;
        while (size > 0) {
            if (used_ == kBufferSize) {
                if (!flushBuffer()) {
                    break;
                }
            }

            const size_t freeBytes = kBufferSize - used_;
            const size_t toCopy = (size < freeBytes) ? size : freeBytes;
            memcpy(buffer_ + used_, data, toCopy);
            used_ += toCopy;
            data += toCopy;
            size -= toCopy;
            accepted += toCopy;
        }
        return accepted;
    }

    bool flushBuffer() {
        if (failed_ || used_ == 0) {
            return !failed_;
        }

        if (!client_write_retry::writeAll(client_, buffer_, used_)) {
            failed_ = true;
            client_.stop();
        }

        used_ = 0;
        return !failed_;
    }

private:
    ClientT& client_;
    uint8_t buffer_[kBufferSize] = {};
    size_t used_ = 0;
    bool failed_ = false;
};

}  // namespace json_stream_detail
#endif

inline void sendJsonStream(WebServer& server, const JsonDocument& doc, int code = 200) {
#if defined(UNIT_TEST)
    std::string response;
    serializeJson(doc, response);
    server.send(code, "application/json", response.c_str());
#else
    const size_t len = measureJson(doc);
    server.setContentLength(len);
    server.send(code, "application/json", "");

    auto client = server.client();
    json_stream_detail::BufferedClientPrint<decltype(client)> buffered(client);
    serializeJson(doc, buffered);
    buffered.flushBuffer();
#endif
}

inline void sendSerializedJson(WebServer& server, const char* data, size_t len, int code = 200) {
#if defined(UNIT_TEST)
    const std::string body(data ? data : "", len);
    server.send(code, "application/json", body.c_str());
#else
    server.setContentLength(len);
    server.send(code, "application/json", "");
    if (data != nullptr && len > 0) {
        server.sendContent(data, len);
    }
#endif
}
