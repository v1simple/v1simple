#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>
#include "modules/wifi/wifi_maintenance_web_server.h"
#include "wifi_rate_limiter.h"
#include "route_cases.h"

SerialClass Serial;
unsigned long mockMillis = 100;
unsigned long mockMicros = 0;

struct Storage {
    bool ready = true;
    bool resolveStorageTransactionsForMutation() {
        ingressEvents.push_back("recovery");
        return ready;
    }
};
class WiFiManager {
  public:
    WifiMaintenanceWebServer server_{80};
    Storage settings_;
    bool maintenanceBootMode_ = true;
    bool maintenanceWritePreAdmitted_ = false;
    SlidingWindowRateLimiter rateLimiter_;
    int serviceCode = 200;
    std::string seenBody;
    std::string seenBoundary;
    bool checkRateLimit();
    bool admitMaintenanceWriteBeforeBody();
    bool hasMaintenanceWriteRequestShape() const;
    bool requireMaintenanceWriteRequestShape();
    static const char* maintenanceApiWriteHeader();
    static const char* maintenanceApiWriteHeaderValue();
    void markUiActivity() {}
#include "registration_declarations.inc"
    void install() {
#include "registration_calls.inc"
    }
    void service(const uint8_t* body = nullptr, size_t size = 0, const char* boundary = nullptr,
                 size_t boundarySize = 0) {
        // The actual shape and rate methods run after the actual recovery
        // wrapper, as they do in production domain handlers.
        if (!requireMaintenanceWriteRequestShape() || !checkRateLimit()) return;
        ingressEvents.push_back("service");
        if (size) seenBody.assign(reinterpret_cast<const char*>(body), size);
        if (boundarySize) seenBoundary.assign(boundary, boundarySize);
        server_.send(serviceCode, "application/json", serviceCode == 200 ? "ok" : "invalid payload");
    }
};
#include "registration_implementations.inc"

static std::string request(const std::string& path, size_t length, const std::string& type = "application/json",
                           const std::string& method = "POST", const std::string& shape = "maintenance-ui",
                           const std::string& extra = "") {
    return method + " " + path + " HTTP/1.1\r\nX-V1Simple-Request: " + shape +
           "\r\nContent-Length: " + std::to_string(length) + "\r\nContent-Type: " + type + "\r\n" + extra + "\r\n";
}
static void expectPreflight(const std::string& value, WifiMaintenanceHttpPreflight::Decision expected,
                            bool maintenance = true) {
    assert(WifiMaintenanceHttpPreflight::evaluate(value.data(), value.size(), maintenance) == expected);
}
static void literalAdmissionCases() {
    using D = WifiMaintenanceHttpPreflight::Decision;
    for (const auto& row : routeCases) {
        expectPreflight(request(row.path, row.maxBytes), D::AllowBodyParsing);
        expectPreflight(request(row.path, row.maxBytes + 1), D::RejectTooLarge);
        expectPreflight(request(std::string(row.path) + "?x=1", row.maxBytes + 1),
                        row.bodyOnlyForm ? D::RejectBadRequest : D::RejectTooLarge);
        WifiMaintenanceHttpPreflight::BodyInfo info;
        const std::string plain = request(row.path, 3);
        assert(WifiMaintenanceHttpPreflight::evaluate(plain.data(), plain.size(), true, &info) == D::AllowBodyParsing);
        assert(info.contentLength == 3);
        assert(info.encoding == (row.bodyOnlyForm ? WifiMaintenanceHttpPreflight::BodyEncoding::UrlEncoded
                                                 : WifiMaintenanceHttpPreflight::BodyEncoding::Other));
        assert(info.multipartBoundaryLength == 0);
        expectPreflight(request(std::string(row.path) + "?x=1", 0),
                        row.bodyOnlyForm ? D::RejectBadRequest : D::AllowBodyParsing);
        expectPreflight(request(std::string(row.path) + "?x=1", 0),
                        row.bodyOnlyForm ? D::RejectBadRequest : D::RejectForbidden, false);
        expectPreflight(request(row.path, 3, "multipart/form-data; boundary=legacy"),
                        row.multipart ? D::AllowBodyParsing : D::RejectMultipart);
        expectPreflight(request(std::string(row.path) + "?x=1", 3, "multipart/form-data; boundary=legacy"),
                        row.bodyOnlyForm ? D::RejectBadRequest : D::RejectMultipart);
        for (const char* method : {"PUT", "PATCH", "DELETE"}) {
            expectPreflight(request(row.path, 0, "application/json", method), D::AllowBodyParsing);
            expectPreflight(request(row.path, 0, "multipart/form-data; boundary=legacy", method), D::RejectMultipart);
        }
        expectPreflight(request(row.path, 3, "application/json", "POST", "wrong"), D::RejectForbidden);
        expectPreflight(request(row.path, 3), D::RejectForbidden, false);
        expectPreflight(request(row.path, 3, "application/json", "POST", "maintenance-ui", "Content-Length: 3\r\n"), D::RejectBadRequest);
        expectPreflight(request(row.path, 3, "application/json", "POST", "maintenance-ui", "Transfer-Encoding: chunked\r\n"), D::RejectBadRequest);
    }
    expectPreflight(request("/api/unknown", 4096), D::AllowBodyParsing);
    expectPreflight(request("/api/unknown", 4097), D::RejectTooLarge);
    expectPreflight(request("/api/v1/profile/", 4097), D::RejectTooLarge);
    expectPreflight(request("/API/v1/profile", 4097), D::RejectTooLarge);
    expectPreflight(request("/api/v1/profile#x", 4097), D::RejectTooLarge);
    expectPreflight("GET /api/v1/apply?x=1 HTTP/1.1\r\nHost: unit\r\n\r\n", D::AllowFrameworkParsing, false);
    expectPreflight("HEAD /unknown HTTP/1.1\r\nHost: unit\r\n\r\n", D::AllowFrameworkParsing, false);
    expectPreflight("POST /api/device/settings HTTP/1.1\r\n", D::NeedMoreHeaders);
    expectPreflight(std::string(2048, 'x'), D::RejectHeadersTooLarge);
    expectPreflight("POST /api/device/settings HTTP/1.1\r\nContent-Length: 0\r\n\r\n", D::RejectForbidden);
    expectPreflight("POST /api/device/settings HTTP/1.1\r\nX-V1Simple-Request: maintenance-ui\r\n\r\n", D::RejectLengthRequired);
    expectPreflight(request("/api/device/settings", 0, "text/plain", "POST", "MAINTENANCE-UI"), D::AllowBodyParsing);
    expectPreflight(request("/api/device/settings", 0, "text/plain", "POST", "maintenance-ui", "X-V1Simple-Request: maintenance-ui\r\n"), D::RejectForbidden);
    for (const char* boundary : {"", "bad boundary", "\"unterminated", "ok; extra=true"}) {
        expectPreflight(request("/api/device/settings", 0, std::string("multipart/form-data; boundary=") + boundary), D::RejectMultipart);
    }
    expectPreflight(request("/api/device/settings", 0, "multipart/form-data; boundary=" + std::string(70, 'a')), D::AllowBodyParsing);
    expectPreflight(request("/api/device/settings", 0, "multipart/form-data; boundary=" + std::string(71, 'a')), D::RejectMultipart);
    for (const char* length : {"-1", "+1", "1x", "1, 1", ""}) {
        expectPreflight(std::string("POST /api/v1/profile HTTP/1.1\r\nX-V1Simple-Request: maintenance-ui\r\nContent-Length: ") + length + "\r\n\r\n", D::RejectBadRequest);
    }
    expectPreflight("POST /api/v1/profile HTTP/1.1\r\nX-V1Simple-Request: maintenance-ui\r\nContent-Length: 99999999999999999999999999\r\n\r\n", D::RejectTooLarge);
}

struct Exchange {
    WiFiManager manager;
    std::shared_ptr<SocketState> socket = std::make_shared<SocketState>();
    int peer = -1;
    Exchange() {
        mock_reset_heap_caps();
        ingressEvents.clear();
        g_mock_heap_caps_allocation_observer = [](void*, bool allocated) {
            ingressEvents.push_back(allocated ? "allocate" : "release");
        };
        manager.install();
        manager.server_.setMaintenanceApIp(IPAddress(1));
        manager.server_.setLiveStaIp([] { return 2u; });
        manager.server_.setWriteAdmission([this] {
            ingressEvents.push_back("admission");
            return manager.admitMaintenanceWriteBeforeBody();
        });
        int pair[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        socket->fd = pair[0];
        peer = pair[1];
    }
    ~Exchange() {
        assert(g_mock_heap_caps_outstanding_allocations == 0);
        close(socket->fd);
        close(peer);
        g_mock_heap_caps_allocation_observer = nullptr;
    }
    void send(const std::string& value) {
        assert(::send(peer, value.data(), value.size(), 0) == static_cast<ssize_t>(value.size()));
        manager.server_.setMaintenanceBootMode(manager.maintenanceBootMode_);
        manager.server_.enqueue(socket);
        manager.server_.handleClient();
        assert(g_mock_heap_caps_outstanding_allocations == 0);
    }
    int count(const char* event) const {
        return static_cast<int>(std::count(ingressEvents.begin(), ingressEvents.end(), event));
    }
    void order(const char* before, const char* after) const {
        auto first = std::find(ingressEvents.begin(), ingressEvents.end(), before);
        auto second = std::find(ingressEvents.begin(), ingressEvents.end(), after);
        assert(first != ingressEvents.end() && second != ingressEvents.end() && first < second);
    }
    void tokenCount(size_t count) { assert(manager.rateLimiter_.evaluate(mockMillis).requestCount == count + 1); }
    void preflightRejected(int status) {
        assert(socket->response.find("HTTP/1.1 " + std::to_string(status) + " ") == 0);
        assert(count("admission") == 0 && count("parse") == 0);
        assert(count("recovery") == 0 && count("service") == 0);
        assert(g_mock_heap_caps_malloc_calls == 0);
        tokenCount(0);
    }
};

static void actualRegistrationsAndWrappers() {
    for (const auto& row : routeCases) {
        Exchange test;
        const auto& registrations = test.manager.server_.registrations;
        assert(registrations.size() == sizeof(routeCases) / sizeof(routeCases[0]));
        assert(std::count_if(registrations.begin(), registrations.end(), [&](const auto& registered) {
            return registered.path == row.path && bool(registered.raw) == row.exact;
        }) == 1);
        const std::string body("a\0b", 3);
        test.send(request(row.path, body.size()) + body);
        assert(test.manager.server_.statusCode == 200);
        assert(test.count("admission") == 1 && test.count("recovery") == 1 && test.count("service") == 1);
        test.order("interface", "admission");
        test.order("admission", "parse");
        test.order("parse", "recovery");
        test.order("recovery", "service");
        test.tokenCount(1);
        assert(test.count("allocate") == (row.exact ? 1 : 0));
        assert(test.count("release") == (row.exact ? 1 : 0));
        if (row.exact) {
            assert(test.manager.seenBody == body);
            assert(g_mock_heap_caps_last_malloc_caps == (MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
            test.order("admission", "allocate");
            test.order("allocate", "recovery");
            test.order("response:200", "release");
            assert(test.manager.server_.exactBodyStatus() == WifiMaintenanceWebServer::ExactBodyStatus::None);
        }
    }
    // Each actual exact registration must permit its independently pinned cap,
    // even though this intentionally short transfer cannot become a service call.
    for (const auto& row : routeCases) {
        if (!row.exact) continue;
        Exchange test;
        test.send(request(row.path, row.maxBytes) + "abc");
        assert(test.manager.server_.statusCode == 400);
        assert(g_mock_heap_caps_last_malloc_size == row.maxBytes);
        assert(test.count("allocate") == 1 && test.count("release") == 1);
        assert(test.count("recovery") == 0 && test.count("service") == 0);
        test.tokenCount(1);
    }
    for (bool exact : {false, true}) {
        const char* path = exact ? "/api/v1/profile" : "/api/gps/config";
        for (bool recovery : {false, true}) {
            Exchange test;
            test.manager.settings_.ready = recovery;
            test.manager.serviceCode = 400;
            test.send(request(path, 3) + "bad");
            assert(test.manager.server_.statusCode == (recovery ? 400 : 503));
            assert(test.count("service") == (recovery ? 1 : 0));
            assert(test.count("recovery") == 1);
            assert(test.count("release") == (exact ? 1 : 0));
            assert(!test.manager.maintenanceWritePreAdmitted_);
            test.tokenCount(1);
        }
    }
    for (int failure = 0; failure < 4; ++failure) {
        Exchange test;
        if (failure == 0) g_mock_heap_caps_fail_malloc = true;
        if (failure == 1) test.manager.server_.abortBody = true;
        if (failure == 2) test.manager.server_.parsedLengthAdjustment = 1;
        if (failure == 3) test.manager.server_.skipRaw = true;
        test.send(request("/api/v1/profile", 3) + "abc");
        assert(test.manager.server_.statusCode == (failure == 0 ? 503 : 400));
        assert(test.count("recovery") == 0 && test.count("service") == 0);
        assert(test.count("release") == (failure == 1 ? 1 : 0));
        assert(!test.manager.maintenanceWritePreAdmitted_);
        test.tokenCount(1);
    }
    {
        Exchange test;
        test.send(request("/api/device/settings", 3, "multipart/form-data; boundary=\"legacy\"") + "abc");
        assert(test.manager.seenBoundary == "legacy");
        assert(test.manager.server_.statusCode == 200);
    }
    {
        Exchange test;
        test.send(request("/api/device/settings", 0));
        assert(test.manager.server_.statusCode == 200);
        assert(g_mock_heap_caps_malloc_calls == 0);
        test.tokenCount(1);
    }
}

static void actualSocketAdmission() {
    for (uint32_t address : {0u, 3u}) {
        Exchange test;
        test.socket->localIp = address;
        test.send(request("/api/v1/profile", 3) + "abc");
        assert(test.socket->stopped && test.socket->response.empty());
        assert(test.count("parse") == 0 && test.count("admission") == 0 && g_mock_heap_caps_malloc_calls == 0);
        test.tokenCount(0);
    }
    {
        Exchange test;
        test.socket->localIp = 2;
        test.send(request("/api/v1/profile", 3) + "abc");
        assert(test.manager.server_.statusCode == 200);
    }
    {
        Exchange test;
        test.manager.server_.setLiveStaIp([] { return 1u; });
        test.send(request("/api/v1/profile", 3) + "abc");
        assert(test.count("admission") == 0 && test.count("parse") == 0);
        test.tokenCount(0);
    }
    for (int rejected = 0; rejected < 6; ++rejected) {
        Exchange test;
        std::string value = request("/api/v1/profile", 3);
        int status = 403;
        if (rejected == 0) test.manager.maintenanceBootMode_ = false;
        if (rejected == 1) value = request("/api/v1/profile", 3, "application/json", "POST", "wrong");
        if (rejected == 2) { value = request("/api/v1/profile", 16385); status = 413; }
        if (rejected == 3) { value = request("/api/v1/apply?x=1", 3); status = 400; }
        if (rejected == 4) { value = request("/api/v1/profile", 3, "multipart/form-data; boundary=legacy"); status = 415; }
        if (rejected == 5) {
            value = "POST /api/v1/profile HTTP/1.1\r\nX-V1Simple-Request: maintenance-ui\r\n\r\n";
            status = 411;
        }
        test.send(value + "abc");
        test.preflightRejected(status);
    }
    {
        Exchange test;
        for (size_t i = 0; i < SlidingWindowRateLimiter::MAX_REQUESTS; ++i)
            assert(test.manager.rateLimiter_.evaluate(mockMillis).allowed);
        test.send(request("/api/v1/profile", 3) + "abc");
        assert(test.socket->response.find("HTTP/1.1 429 ") == 0);
        assert(test.count("admission") == 1 && test.count("parse") == 0 && g_mock_heap_caps_malloc_calls == 0);
    }
    {
        Exchange test;
        test.send("POST /api/v1/profile HTTP/1.1\r\nX-Long: " + std::string(2048, 'x'));
        test.preflightRejected(431);
    }
    {
        Exchange test;
        test.send("POST /api/v1/profile HTTP/1.1\r\n");
        assert(test.count("admission") == 0 && test.count("parse") == 0);
        assert(test.socket->response.empty() && !test.socket->stopped);
        test.tokenCount(0);
    }
    for (const char* method : {"PUT", "PATCH", "DELETE", "GET"}) {
        Exchange test;
        test.send(request("/api/v1/profile", 0, "application/json", method));
        assert(test.manager.server_.statusCode == 404);
        assert(test.count("admission") == (std::string(method) == "GET" ? 0 : 1));
        assert(test.count("service") == 0 && g_mock_heap_caps_malloc_calls == 0);
        test.tokenCount(std::string(method) == "GET" ? 0 : 1);
    }
    {
        Exchange test;
        test.send(request("/api/unknown", 0));
        assert(test.manager.server_.statusCode == 404);
        test.tokenCount(1);
    }
    {
        // Preflight accepts the case-insensitive shape value; the existing
        // domain shape check remains case-sensitive after recovery.
        Exchange test;
        test.send(request("/api/v1/profile", 3, "application/json", "POST", "MAINTENANCE-UI") + "abc");
        assert(test.manager.server_.statusCode == 403);
        assert(test.count("recovery") == 1 && test.count("service") == 0);
        assert(test.count("allocate") == 1 && test.count("release") == 1);
        test.tokenCount(1);
    }
}

int main() {
    literalAdmissionCases();
    actualRegistrationsAndWrappers();
    actualSocketAdmission();
    std::puts("maintenance ingress: 35 literal route contracts, actual registrations/wrappers and socket admission passed");
}
