#pragma once

#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "Arduino.h"

#ifndef CONTENT_LENGTH_NOT_SET
#define CONTENT_LENGTH_NOT_SET ((size_t)-2)
#endif

class WebServer {
public:
    static constexpr size_t kUnlimitedClientBytes = std::numeric_limits<size_t>::max();

    class MockClient {
    public:
        explicit MockClient(WebServer* server = nullptr) : server_(server) {}

        bool connected() const { return connected_; }

        void setConnected(bool connected) { connected_ = connected; }

        void stop() { connected_ = false; }

        size_t write(const uint8_t* data, size_t length) {
            if (!server_ || !data || length == 0 || !connected_) {
                return 0;
            }

            size_t bytesToWrite = length;
            if (server_->clientMaxWriteSize_ != kUnlimitedClientBytes) {
                bytesToWrite = std::min(bytesToWrite, server_->clientMaxWriteSize_);
            }

            if (server_->clientDisconnectAfterBytes_ != kUnlimitedClientBytes) {
                if (server_->clientBytesWritten_ >= server_->clientDisconnectAfterBytes_) {
                    connected_ = false;
                    return 0;
                }

                const size_t bytesBeforeDisconnect =
                    server_->clientDisconnectAfterBytes_ - server_->clientBytesWritten_;
                bytesToWrite = std::min(bytesToWrite, bytesBeforeDisconnect);
            }

            if (bytesToWrite == 0) {
                return 0;
            }

            server_->lastBody.write(data, bytesToWrite);
            server_->clientBytesWritten_ += bytesToWrite;
            server_->clientWriteCalls_++;

            if (server_->clientDisconnectAfterBytes_ != kUnlimitedClientBytes &&
                server_->clientBytesWritten_ >= server_->clientDisconnectAfterBytes_) {
                connected_ = false;
            }

            return bytesToWrite;
        }

    private:
        WebServer* server_ = nullptr;
        bool connected_ = true;
    };

    explicit WebServer(int /*port*/ = 80) : client_(this) {}

    bool hasArg(const String& name) const {
        return args_.find(name.c_str()) != args_.end();
    }

    String arg(const String& name) const {
        auto it = args_.find(name.c_str());
        if (it == args_.end()) {
            return "";
        }
        return it->second.c_str();
    }

    String arg(const char* name) const {
        return arg(String(name));
    }

    void setArg(const String& name, const String& value) {
        args_[name.c_str()] = value.c_str();
    }

    void clearArgs() {
        args_.clear();
    }

    bool hasHeader(const String& name) const {
        return requestHeaders_.find(name.c_str()) != requestHeaders_.end();
    }

    String header(const String& name) const {
        auto it = requestHeaders_.find(name.c_str());
        if (it == requestHeaders_.end()) {
            return "";
        }
        return it->second.c_str();
    }

    void setHeader(const String& name, const String& value) {
        requestHeaders_[name.c_str()] = value.c_str();
    }

    void collectHeaders(const char* const* /*headerKeys*/, size_t /*count*/) {}

    void sendHeader(const String& name, const String& value, bool /*first*/ = false) {
        responseHeaders_[name.c_str()] = value.c_str();
    }

    void setContentLength(size_t length) {
        lastContentLength = length;
        contentLengthHistory.push_back(length);
    }

    void setClientMaxWriteSize(size_t bytes) {
        clientMaxWriteSize_ = bytes;
    }

    void clearClientMaxWriteSize() {
        clientMaxWriteSize_ = kUnlimitedClientBytes;
    }

    void setClientDisconnectAfterBytes(size_t bytes) {
        clientDisconnectAfterBytes_ = bytes;
    }

    void clearClientDisconnectAfterBytes() {
        clientDisconnectAfterBytes_ = kUnlimitedClientBytes;
    }

    size_t clientBytesWritten() const {
        return clientBytesWritten_;
    }

    size_t clientWriteCalls() const {
        return clientWriteCalls_;
    }

    MockClient& client() { return client_; }
    const MockClient& client() const { return client_; }

    void handleClient() {}

    String sentHeader(const String& name) const {
        auto it = responseHeaders_.find(name.c_str());
        if (it == responseHeaders_.end()) {
            return "";
        }
        return it->second.c_str();
    }

    void send(int code, const char* contentType, const String& body) {
        lastStatusCode = code;
        lastContentType = contentType ? contentType : "";
        lastBody = body;
        sendCount++;
    }

    void send(int code, const char* contentType, const char* body) {
        send(code, contentType, String(body ? body : ""));
    }

    int lastStatusCode = 0;
    String lastContentType;
    String lastBody;
    size_t lastContentLength = 0;
    std::vector<size_t> contentLengthHistory;
    int sendCount = 0;

private:
    std::unordered_map<std::string, std::string> args_;
    std::unordered_map<std::string, std::string> requestHeaders_;
    std::unordered_map<std::string, std::string> responseHeaders_;
    size_t clientMaxWriteSize_ = kUnlimitedClientBytes;
    size_t clientDisconnectAfterBytes_ = kUnlimitedClientBytes;
    size_t clientBytesWritten_ = 0;
    size_t clientWriteCalls_ = 0;
    MockClient client_;
};
