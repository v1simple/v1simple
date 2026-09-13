#pragma once

#include <WebServer.h>
#include <array>
#include <cstring>
#include <functional>
#include <esp_heap_caps.h>
#include <sys/socket.h>
#include <utility>

#include "wifi_maintenance_http_preflight.h"
#include "wifi_maintenance_interface_policy.h"
#include "wifi_exact_body_length_policy.h"

// Project-owned ingress seam in front of Arduino WebServer. Its fixed peek
// buffer validates every body-bearing request before WebServer::_parseRequest
// can allocate a body-sized buffer or enter multipart form parsing.
class WifiMaintenanceWebServer final : public WebServer {
  public:
    using WebServer::WebServer;

    ~WifiMaintenanceWebServer() override { clearRequestIngress(); }

    enum class ExactBodyStatus : uint8_t {
        None = 0,
        Capturing,
        Ready,
        Invalid,
        TooLarge,
        MemoryUnavailable,
    };

    void setMaintenanceBootMode(const bool enabled) { maintenanceBootMode_ = enabled; }
    void setWriteAdmission(std::function<bool()> admission) { writeAdmission_ = std::move(admission); }
    void setMaintenanceApIp(const IPAddress& apIp) { maintenanceApIp_ = static_cast<uint32_t>(apIp); }
    void setLiveStaIp(std::function<uint32_t()> provider) { liveStaIp_ = std::move(provider); }

    // WebServer's raw-handler path reads in a fixed framework buffer and does
    // not construct the ordinary `plain` String.  These helpers give the few
    // large JSON routes an explicitly PSRAM-owned, length-preserving body.
    // Allocation failure is retained as state until the terminal handler can
    // return 503 without invoking any service mutation.
    void captureExactBody(HTTPRaw& raw, size_t maxBytes) {
        if (raw.status == RAW_START) {
            clearExactBodyStorage();
            // The pinned WebServer TU recognizes this signal and uses the
            // callback-provided currentSize as its only raw-read limit.
            raw.data = reinterpret_cast<void*>(kRawLengthContractSignal);
            raw.currentSize = 0;
            if (!preflightBodyInfoValid_) {
                exactBodyStatus_ = ExactBodyStatus::Invalid;
                return;
            }
            const WifiExactBodyLengthPolicy::StartDecision length = WifiExactBodyLengthPolicy::begin(
                preflightBodyInfoValid_, preflightBodyInfo_.contentLength,
                clientContentLength(), maxBytes);
            if (length.status == WifiExactBodyLengthPolicy::StartStatus::Invalid) {
                exactBodyStatus_ = ExactBodyStatus::Invalid;
                return;
            }
            if (length.status == WifiExactBodyLengthPolicy::StartStatus::TooLarge) {
                exactBodyStatus_ = ExactBodyStatus::TooLarge;
                return;
            }
            exactBodyExpected_ = length.expected;
            if (exactBodyExpected_ != 0) {
                exactBody_ = static_cast<uint8_t*>(
                    heap_caps_malloc(exactBodyExpected_, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
                if (!exactBody_) {
                    exactBodyStatus_ = ExactBodyStatus::MemoryUnavailable;
                    return;
                }
            }
            raw.currentSize = length.frameworkReadLimit;
            exactBodyStatus_ = ExactBodyStatus::Capturing;
            return;
        }

        if (raw.status == RAW_ABORTED) {
            heap_caps_free(exactBody_);
            exactBody_ = nullptr;
            exactBodyExpected_ = 0;
            exactBodyReceived_ = 0;
            exactBodyStatus_ = ExactBodyStatus::Invalid;
            return;
        }
        if (raw.status == RAW_WRITE) {
            if (exactBodyStatus_ != ExactBodyStatus::Capturing ||
                !WifiExactBodyLengthPolicy::acceptsChunk(exactBodyExpected_, exactBodyReceived_,
                                                         raw.currentSize, raw.totalSize)) {
                if (exactBodyStatus_ == ExactBodyStatus::Capturing)
                    exactBodyStatus_ = ExactBodyStatus::Invalid;
                heap_caps_free(exactBody_);
                exactBody_ = nullptr;
                exactBodyExpected_ = 0;
                exactBodyReceived_ = 0;
                return;
            }
            if (raw.currentSize != 0) {
                memcpy(exactBody_ + exactBodyReceived_, raw.buf, raw.currentSize);
                exactBodyReceived_ += raw.currentSize;
            }
            return;
        }
        if (raw.status == RAW_END && exactBodyStatus_ == ExactBodyStatus::Capturing) {
            exactBodyStatus_ = WifiExactBodyLengthPolicy::acceptsEnd(
                                   exactBodyExpected_, exactBodyReceived_, raw.totalSize)
                                   ? ExactBodyStatus::Ready
                                   : ExactBodyStatus::Invalid;
            if (exactBodyStatus_ == ExactBodyStatus::Invalid) {
                heap_caps_free(exactBody_);
                exactBody_ = nullptr;
                exactBodyExpected_ = 0;
                exactBodyReceived_ = 0;
            }
        }
    }

    ExactBodyStatus exactBodyStatus() const { return exactBodyStatus_; }
    const uint8_t* exactBodyData() const { return exactBody_; }
    size_t exactBodySize() const { return exactBodyReceived_; }
    const char* exactMultipartBoundaryData() const {
        return preflightBodyInfo_.encoding == WifiMaintenanceHttpPreflight::BodyEncoding::MultipartFormData
                   ? preflightBodyInfo_.multipartBoundary
                   : nullptr;
    }
    size_t exactMultipartBoundarySize() const {
        return preflightBodyInfo_.encoding == WifiMaintenanceHttpPreflight::BodyEncoding::MultipartFormData
                   ? preflightBodyInfo_.multipartBoundaryLength
                   : 0u;
    }
    void releaseExactBody() { clearRequestIngress(); }
    const uint8_t* exactQueryData() const {
        for (size_t i = 0; i < requestTargetSize_; ++i) {
            if (requestTarget_[i] == '?') return requestTarget_.data() + i + 1u;
        }
        return nullptr;
    }
    size_t exactQuerySize() const {
        for (size_t i = 0; i < requestTargetSize_; ++i) {
            if (requestTarget_[i] == '?') return requestTargetSize_ - i - 1u;
        }
        return 0;
    }

    void handleClient() override {
        if (_currentStatus == HC_NONE) {
            _currentClient = _server.accept();
            if (!_currentClient) {
                if (_nullDelay) {
                    delay(1);
                }
                return;
            }
            clearRequestIngress();
            // NetworkClient::localIP() is backed by getsockname() in the
            // pinned framework, so admit only the maintenance AP or current
            // saved-network address before parsing the request.
            const uint32_t liveStaIp = liveStaIp_ ? liveStaIp_() : 0;
            if (!WifiMaintenanceInterfacePolicy::allows(static_cast<uint32_t>(_currentClient.localIP()),
                                                         maintenanceApIp_, liveStaIp)) {
                _currentClient.stop();
                _currentClient = NetworkClient();
                return;
            }
            _currentStatus = HC_WAIT_READ;
            _statusChange = millis();
        }

        bool keepCurrentClient = false;
        bool callYield = false;

        if (_currentClient.connected()) {
            switch (_currentStatus) {
            case HC_NONE:
                break;
            case HC_WAIT_READ:
                if (_currentClient.available()) {
                    const WifiMaintenanceHttpPreflight::Decision preflight = inspectCurrentRequest();
                    if (preflight == WifiMaintenanceHttpPreflight::Decision::NeedMoreHeaders) {
                        keepCurrentClient = millis() - _statusChange <= HTTP_MAX_DATA_WAIT;
                        callYield = true;
                    } else if (preflight != WifiMaintenanceHttpPreflight::Decision::AllowFrameworkParsing &&
                               preflight != WifiMaintenanceHttpPreflight::Decision::AllowBodyParsing) {
                        sendPreflightError(preflight);
                    } else {
                        _currentClient.setTimeout(HTTP_MAX_SEND_WAIT);
                        if (_parseRequest(_currentClient)) {
                            _contentLength = CONTENT_LENGTH_NOT_SET;
                            _responseCode = 0;
                            _clearResponseHeaders();

                            if (_chain) {
                                _chain->runChain(*this, [this]() { return _handleRequest(); });
                            } else {
                                _handleRequest();
                            }

                            if (_currentClient.isSSE()) {
                                _currentStatus = HC_WAIT_CLOSE;
                                _statusChange = millis();
                                keepCurrentClient = true;
                            }
                        }
                    }
                } else {
                    if (millis() - _statusChange <= HTTP_MAX_DATA_WAIT) {
                        keepCurrentClient = true;
                    }
                    callYield = true;
                }
                break;
            case HC_WAIT_CLOSE:
                if (_currentClient.isSSE()) {
                    _statusChange = millis();
                }
                if (millis() - _statusChange <= HTTP_MAX_CLOSE_WAIT) {
                    keepCurrentClient = true;
                    callYield = true;
                }
                break;
            }
        }

        if (!keepCurrentClient) {
            _currentClient.stop();
            _currentClient = NetworkClient();
            _currentStatus = HC_NONE;
            _currentUpload.reset();
            _currentRaw.reset();
            clearRequestIngress();
        }

        if (callYield) {
            yield();
        }
    }

  private:
    static constexpr uintptr_t kRawLengthContractSignal = 0x56314232u;

    void clearExactBodyStorage() {
        heap_caps_free(exactBody_);
        exactBody_ = nullptr;
        exactBodyExpected_ = 0;
        exactBodyReceived_ = 0;
        exactBodyStatus_ = ExactBodyStatus::None;
    }

    void clearRequestIngress() {
        clearExactBodyStorage();
        requestTargetSize_ = 0;
        preflightBodyInfo_ = WifiMaintenanceHttpPreflight::BodyInfo{};
        preflightBodyInfoValid_ = false;
    }

    WifiMaintenanceHttpPreflight::Decision inspectCurrentRequest() {
        const int socketFd = _currentClient.fd();
        if (socketFd < 0) {
            return WifiMaintenanceHttpPreflight::Decision::RejectBadRequest;
        }
        const ssize_t peeked =
            ::recv(socketFd, headerPeek_.data(), headerPeek_.size(), MSG_PEEK | MSG_DONTWAIT);
        if (peeked <= 0) {
            return WifiMaintenanceHttpPreflight::Decision::NeedMoreHeaders;
        }
        const char* const lineEnd = WifiMaintenanceHttpPreflight::findBytes(
            headerPeek_.data(), static_cast<size_t>(peeked), "\r\n", 2);
        if (!lineEnd) return WifiMaintenanceHttpPreflight::Decision::NeedMoreHeaders;
        const char* const methodEnd = WifiMaintenanceHttpPreflight::findBytes(
            headerPeek_.data(), static_cast<size_t>(lineEnd - headerPeek_.data()), " ", 1);
        if (!methodEnd) return WifiMaintenanceHttpPreflight::Decision::RejectBadRequest;
        const char* const targetBegin = methodEnd + 1;
        const char* const targetEnd = WifiMaintenanceHttpPreflight::findBytes(
            targetBegin, static_cast<size_t>(lineEnd - targetBegin), " ", 1);
        if (!targetEnd || targetEnd == targetBegin ||
            static_cast<size_t>(targetEnd - targetBegin) >= requestTarget_.size()) {
            return WifiMaintenanceHttpPreflight::Decision::RejectBadRequest;
        }
        requestTargetSize_ = static_cast<size_t>(targetEnd - targetBegin);
        memcpy(requestTarget_.data(), targetBegin, requestTargetSize_);
        WifiMaintenanceHttpPreflight::BodyInfo bodyInfo;
        const WifiMaintenanceHttpPreflight::Decision decision = WifiMaintenanceHttpPreflight::evaluate(
            headerPeek_.data(), static_cast<size_t>(peeked), maintenanceBootMode_, &bodyInfo);
        if (decision == WifiMaintenanceHttpPreflight::Decision::AllowBodyParsing) {
            const WifiMaintenanceHttpPreflight::Decision admitted = WifiMaintenanceHttpPreflight::applyWriteAdmission(
                decision, writeAdmission_ && writeAdmission_());
            if (admitted == WifiMaintenanceHttpPreflight::Decision::AllowBodyParsing) {
                preflightBodyInfo_ = bodyInfo;
                preflightBodyInfoValid_ = true;
            }
            return admitted;
        }
        return decision;
    }

    void sendPreflightError(const WifiMaintenanceHttpPreflight::Decision decision) {
        int status = 400;
        const char* reason = "Bad Request";
        const char* body = "{\"success\":false,\"error\":\"invalid request\"}";
        switch (decision) {
        case WifiMaintenanceHttpPreflight::Decision::RejectForbidden:
            status = 403;
            reason = "Forbidden";
            body = "{\"success\":false,\"error\":\"forbidden\"}";
            break;
        case WifiMaintenanceHttpPreflight::Decision::RejectRateLimited:
            status = 429;
            reason = "Too Many Requests";
            body = "{\"success\":false,\"error\":\"too many requests\"}";
            break;
        case WifiMaintenanceHttpPreflight::Decision::RejectLengthRequired:
            status = 411;
            reason = "Length Required";
            body = "{\"success\":false,\"error\":\"content length required\"}";
            break;
        case WifiMaintenanceHttpPreflight::Decision::RejectTooLarge:
            status = 413;
            reason = "Payload Too Large";
            body = "{\"success\":false,\"error\":\"body too large\"}";
            break;
        case WifiMaintenanceHttpPreflight::Decision::RejectMultipart:
            status = 415;
            reason = "Unsupported Media Type";
            body = "{\"success\":false,\"error\":\"multipart unsupported\"}";
            break;
        case WifiMaintenanceHttpPreflight::Decision::RejectHeadersTooLarge:
            status = 431;
            reason = "Request Header Fields Too Large";
            body = "{\"success\":false,\"error\":\"headers too large\"}";
            break;
        default:
            break;
        }

        char responseHeader[192];
        const int headerLength = snprintf(responseHeader, sizeof(responseHeader),
                                          "HTTP/1.1 %d %s\r\nConnection: close\r\nContent-Type: application/json\r\n"
                                          "Content-Length: %u\r\n\r\n",
                                          status, reason, static_cast<unsigned>(strlen(body)));
        if (headerLength > 0 && static_cast<size_t>(headerLength) < sizeof(responseHeader)) {
            _currentClient.write(reinterpret_cast<const uint8_t*>(responseHeader), static_cast<size_t>(headerLength));
            _currentClient.write(reinterpret_cast<const uint8_t*>(body), strlen(body));
        }
        _currentClient.stop();
    }

    bool maintenanceBootMode_ = false;
    uint32_t maintenanceApIp_ = 0;
    std::function<uint32_t()> liveStaIp_;
    std::function<bool()> writeAdmission_;
    std::array<char, WifiMaintenanceHttpPreflight::kMaxHeaderBytes> headerPeek_{};
    std::array<uint8_t, 512> requestTarget_{};
    size_t requestTargetSize_ = 0;
    WifiMaintenanceHttpPreflight::BodyInfo preflightBodyInfo_{};
    bool preflightBodyInfoValid_ = false;
    uint8_t* exactBody_ = nullptr;
    size_t exactBodyExpected_ = 0;
    size_t exactBodyReceived_ = 0;
    ExactBodyStatus exactBodyStatus_ = ExactBodyStatus::None;
};
