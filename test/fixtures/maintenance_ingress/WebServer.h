#pragma once

// Socket-backed framework adapter for the project-owned ingress fixture.
// Parsing here only drives raw callbacks; the pinned framework's actual raw,
// plain and multipart loops are exercised by test_http_transfer_lifecycle.py.
#include <Arduino.h>
#include <cassert>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

inline std::vector<std::string> ingressEvents;
class IPAddress {
  public:
    explicit IPAddress(uint32_t value = 0) : value_(value) {}
    operator uint32_t() const { return value_; }
  private:
    uint32_t value_;
};
struct SocketState {
    int fd = -1;
    uint32_t localIp = 1;
    bool stopped = false;
    std::string response;
};
class NetworkClient {
  public:
    NetworkClient() = default;
    explicit NetworkClient(std::shared_ptr<SocketState> state) : state_(std::move(state)) {}
    explicit operator bool() const { return state_ && !state_->stopped; }
    bool connected() const { return bool(*this); }
    int fd() const { return state_ ? state_->fd : -1; }
    IPAddress localIP() const { ingressEvents.push_back("interface"); return IPAddress(state_->localIp); }
    int available() const { char value; return connected() && recv(fd(), &value, 1, MSG_PEEK | MSG_DONTWAIT) > 0; }
    void stop() { if (state_) state_->stopped = true; }
    bool isSSE() const { return false; }
    void setTimeout(unsigned long) {}
    size_t write(const uint8_t* data, size_t length) {
        state_->response.append(reinterpret_cast<const char*>(data), length);
        return length;
    }
  private:
    std::shared_ptr<SocketState> state_;
};
class NetworkServer {
  public:
    NetworkClient queued;
    NetworkClient accept() { NetworkClient result = queued; queued = NetworkClient(); return result; }
};
enum HTTPMethod { HTTP_GET, HTTP_POST };
enum HTTPRawStatus { RAW_START, RAW_WRITE, RAW_END, RAW_ABORTED };
struct HTTPRaw {
    HTTPRawStatus status = RAW_START;
    void* data = nullptr;
    uint8_t* buf = nullptr;
    size_t currentSize = 0;
    size_t totalSize = 0;
};
enum { HC_NONE, HC_WAIT_READ, HC_WAIT_CLOSE };
constexpr unsigned long HTTP_MAX_DATA_WAIT = 5000;
constexpr unsigned long HTTP_MAX_CLOSE_WAIT = 2000;
constexpr unsigned long HTTP_MAX_SEND_WAIT = 5000;
constexpr size_t CONTENT_LENGTH_NOT_SET = size_t(-1);
class WebServer {
  public:
    using THandlerFunction = std::function<void()>;
    struct Registration { std::string path; THandlerFunction handler; THandlerFunction raw; };
    struct Chain { void runChain(WebServer&, THandlerFunction handler) { handler(); } };
    explicit WebServer(int = 80) {}
    virtual ~WebServer() = default;
    virtual void handleClient() {}
    void on(const char* path, HTTPMethod method, THandlerFunction handler, THandlerFunction raw = {}) {
        assert(method == HTTP_POST);
        registrations.push_back({path, std::move(handler), std::move(raw)});
    }
    void send(int status, const char*, const char* body) {
        statusCode = status;
        responseBody = body;
        ingressEvents.push_back("response:" + std::to_string(status));
    }
    void sendHeader(const char*, const String&) {}
    String uri() const { return String(path_); }
    bool hasHeader(const char* name) const { return headers_.count(name); }
    String header(const char* name) const {
        auto found = headers_.find(name);
        return String(found == headers_.end() ? "" : found->second);
    }
    HTTPRaw& raw() { return *_currentRaw; }
    int clientContentLength() const { return parsedLength_; }
    void enqueue(const std::shared_ptr<SocketState>& socket) { _server.queued = NetworkClient(socket); }
    std::vector<Registration> registrations;
    int statusCode = 0;
    std::string responseBody;
    bool abortBody = false;
    bool skipRaw = false;
    int parsedLengthAdjustment = 0;
    size_t rawCapObserved = 0;

  protected:
    bool _parseRequest(NetworkClient& client) {
        ingressEvents.push_back("parse");
        char input[8192];
        const ssize_t count = recv(client.fd(), input, sizeof(input), MSG_DONTWAIT);
        assert(count > 0);
        const std::string request(input, static_cast<size_t>(count));
        const size_t firstSpace = request.find(' ');
        const std::string method = request.substr(0, firstSpace);
        const size_t secondSpace = request.find(' ', firstSpace + 1);
        path_ = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        path_ = path_.substr(0, path_.find('?'));
        headers_.clear();
        size_t line = request.find("\r\n") + 2;
        const size_t end = request.find("\r\n\r\n");
        while (line < end) {
            const size_t colon = request.find(':', line);
            const size_t next = request.find("\r\n", line);
            size_t value = colon + 1;
            while (request[value] == ' ') ++value;
            headers_[request.substr(line, colon - line)] = request.substr(value, next - value);
            line = next + 2;
        }
        parsedLength_ = headers_.count("Content-Length") ? std::stoi(headers_["Content-Length"]) : 0;
        parsedLength_ += parsedLengthAdjustment;
        selected_ = nullptr;
        for (auto& registration : registrations) {
            if (method == "POST" && registration.path == path_) selected_ = &registration;
        }
        if (!selected_) return true;
        if (!selected_->raw) {
            ingressEvents.push_back("framework-body");
            return true;
        }
        if (skipRaw) return true;
        _currentRaw = std::make_unique<HTTPRaw>();
        selected_->raw();
        assert(reinterpret_cast<uintptr_t>(_currentRaw->data) == 0x56314232u);
        rawCapObserved = _currentRaw->currentSize;
        if (abortBody) {
            _currentRaw->status = RAW_ABORTED;
            selected_->raw();
            return true;
        }
        const std::string body = request.substr(end + 4);
        if (_currentRaw->currentSize && !body.empty()) {
            _currentRaw->status = RAW_WRITE;
            _currentRaw->buf = reinterpret_cast<uint8_t*>(const_cast<char*>(body.data()));
            _currentRaw->currentSize = body.size();
            _currentRaw->totalSize = body.size();
            selected_->raw();
        }
        _currentRaw->status = RAW_END;
        selected_->raw();
        return true;
    }
    void _handleRequest() {
        if (selected_) selected_->handler();
        else send(404, "text/plain", "not found");
    }
    void _clearResponseHeaders() {}
    NetworkServer _server;
    NetworkClient _currentClient;
    int _currentStatus = HC_NONE;
    bool _nullDelay = false;
    unsigned long _statusChange = 0;
    size_t _contentLength = 0;
    int _responseCode = 0;
    Chain* _chain = nullptr;
    std::unique_ptr<int> _currentUpload;
    std::unique_ptr<HTTPRaw> _currentRaw;
  private:
    int parsedLength_ = 0;
    std::string path_;
    std::map<std::string, std::string> headers_;
    Registration* selected_ = nullptr;
};
