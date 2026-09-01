#pragma once

#include <WebServer.h>
#include <array>
#include <cstring>
#include <functional>
#include <sys/socket.h>
#include <utility>

#include "wifi_maintenance_http_preflight.h"
#include "wifi_maintenance_interface_policy.h"

// Project-owned ingress seam in front of Arduino WebServer. Its fixed peek
// buffer validates every body-bearing request before WebServer::_parseRequest
// can allocate a body-sized buffer or enter multipart form parsing.
class WifiMaintenanceWebServer final : public WebServer {
  public:
    using WebServer::WebServer;

    void setMaintenanceBootMode(const bool enabled) { maintenanceBootMode_ = enabled; }
    void setWriteAdmission(std::function<bool()> admission) { writeAdmission_ = std::move(admission); }
    void setMaintenanceApIp(const IPAddress& apIp) { maintenanceApIp_ = static_cast<uint32_t>(apIp); }
    void setLiveStaIp(std::function<uint32_t()> provider) { liveStaIp_ = std::move(provider); }

    void handleClient() override {
        if (_currentStatus == HC_NONE) {
            _currentClient = _server.accept();
            if (!_currentClient) {
                if (_nullDelay) {
                    delay(1);
                }
                return;
            }
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
        }

        if (callYield) {
            yield();
        }
    }

  private:
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
        const WifiMaintenanceHttpPreflight::Decision decision = WifiMaintenanceHttpPreflight::evaluate(
            headerPeek_.data(), static_cast<size_t>(peeked), maintenanceBootMode_);
        if (decision == WifiMaintenanceHttpPreflight::Decision::AllowBodyParsing) {
            return WifiMaintenanceHttpPreflight::applyWriteAdmission(
                decision, writeAdmission_ && writeAdmission_());
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
};
