#include <unity.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <Arduino.h>

struct CountingSerial {
    uint32_t printCount = 0;
    uint32_t printlnCount = 0;
    uint32_t printfCount = 0;

    void reset() {
        printCount = 0;
        printlnCount = 0;
        printfCount = 0;
    }

    uint32_t totalLogs() const {
        return printCount + printlnCount + printfCount;
    }

    void begin(unsigned long) {}
    void print(const char*) { printCount++; }
    void print(int) { printCount++; }
    void print(float, int = 2) { printCount++; }
    void println(const char* = "") { printlnCount++; }
    void println(int) { printlnCount++; }
    void println(float, int = 2) { printlnCount++; }
    void printf(const char*, ...) { printfCount++; }
};

static CountingSerial countedSerial;

#define Serial countedSerial
#include "../../src/modules/wifi/wifi_portal_api_service.h"
#include "../../src/modules/wifi/wifi_portal_api_service.cpp"  // Pull implementation for UNIT_TEST.
#undef Serial

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

static bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

static std::string projectRoot() {
    return std::string(PROJECT_DIR);
}

static std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
}

static std::string extractFunctionBody(const std::string& text, const std::string& signature) {
    const size_t sigPos = text.find(signature);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, sigPos);

    const size_t braceStart = text.find('{', sigPos);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, braceStart);

    int depth = 0;
    for (size_t i = braceStart; i < text.size(); ++i) {
        if (text[i] == '{') {
            depth++;
        } else if (text[i] == '}') {
            depth--;
            if (depth == 0) {
                return text.substr(braceStart, i - braceStart + 1);
            }
        }
    }

    TEST_FAIL_MESSAGE("Failed to locate function body end");
    return {};
}

static void assertCaptivePortalNoStoreHeaders(const WebServer& server) {
    TEST_ASSERT_EQUAL_STRING("no-store, no-cache, must-revalidate",
                             server.sentHeader("Cache-Control").c_str());
    TEST_ASSERT_EQUAL_STRING("no-cache", server.sentHeader("Pragma").c_str());
}

static void incrementCounter(void* ctx) { (*static_cast<int*>(ctx))++; }

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    countedSerial.reset();
}

void tearDown() {}

void test_ping_marks_ui_activity_and_returns_ok() {
    WebServer server(80);
    int uiActivityCalls = 0;

    WifiPortalApiService::handleApiPing(server, incrementCounter, &uiActivityCalls);

    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("text/plain", server.lastContentType.c_str());
    TEST_ASSERT_TRUE(responseContains(server, "OK"));
    TEST_ASSERT_EQUAL_STRING("", server.sentHeader("Cache-Control").c_str());
    TEST_ASSERT_EQUAL_STRING("", server.sentHeader("Pragma").c_str());
}

void test_generate_204_marks_ui_activity_and_returns_empty_204() {
    WebServer server(80);
    int uiActivityCalls = 0;

    WifiPortalApiService::handleApiGenerate204(server, incrementCounter, &uiActivityCalls);

    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(204, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("text/plain", server.lastContentType.c_str());
    TEST_ASSERT_EQUAL_STRING("", server.lastBody.c_str());
    assertCaptivePortalNoStoreHeaders(server);
}

void test_gen_204_marks_ui_activity_and_returns_empty_204() {
    WebServer server(80);
    int uiActivityCalls = 0;

    WifiPortalApiService::handleApiGen204(server, incrementCounter, &uiActivityCalls);

    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_INT(204, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("text/plain", server.lastContentType.c_str());
    TEST_ASSERT_EQUAL_STRING("", server.lastBody.c_str());
    assertCaptivePortalNoStoreHeaders(server);
}

void test_hotspot_detect_marks_ui_activity_and_redirects_to_settings() {
    WebServer server(80);
    int uiActivityCalls = 0;

    WifiPortalApiService::handleApiHotspotDetect(server, incrementCounter, &uiActivityCalls);

    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);
    TEST_ASSERT_EQUAL_STRING("/settings", server.sentHeader("Location").c_str());
    TEST_ASSERT_EQUAL_INT(302, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("text/html", server.lastContentType.c_str());
    TEST_ASSERT_EQUAL_STRING("", server.lastBody.c_str());
    assertCaptivePortalNoStoreHeaders(server);
}

void test_fwlink_redirects_to_settings() {
    WebServer server(80);

    WifiPortalApiService::handleApiFwlink(server);

    TEST_ASSERT_EQUAL_STRING("/settings", server.sentHeader("Location").c_str());
    TEST_ASSERT_EQUAL_INT(302, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("text/html", server.lastContentType.c_str());
    TEST_ASSERT_EQUAL_STRING("", server.lastBody.c_str());
    assertCaptivePortalNoStoreHeaders(server);
}

void test_ncsi_returns_expected_body() {
    WebServer server(80);

    WifiPortalApiService::handleApiNcsiTxt(server);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("text/plain", server.lastContentType.c_str());
    TEST_ASSERT_TRUE(responseContains(server, "Microsoft NCSI"));
    assertCaptivePortalNoStoreHeaders(server);
}

void test_probe_burst_does_not_emit_serial_logs() {
    static constexpr int kProbeBursts = 10;
    int uiActivityCalls = 0;

    countedSerial.reset();
    for (int i = 0; i < kProbeBursts; ++i) {
        WebServer pingServer(80);
        WifiPortalApiService::handleApiPing(pingServer, incrementCounter, &uiActivityCalls);
        TEST_ASSERT_EQUAL_INT(200, pingServer.lastStatusCode);

        WebServer generate204Server(80);
        WifiPortalApiService::handleApiGenerate204(generate204Server, incrementCounter, &uiActivityCalls);
        TEST_ASSERT_EQUAL_INT(204, generate204Server.lastStatusCode);

        WebServer gen204Server(80);
        WifiPortalApiService::handleApiGen204(gen204Server, incrementCounter, &uiActivityCalls);
        TEST_ASSERT_EQUAL_INT(204, gen204Server.lastStatusCode);

        WebServer hotspotServer(80);
        WifiPortalApiService::handleApiHotspotDetect(hotspotServer, incrementCounter, &uiActivityCalls);
        TEST_ASSERT_EQUAL_INT(302, hotspotServer.lastStatusCode);

        WebServer fwlinkServer(80);
        WifiPortalApiService::handleApiFwlink(fwlinkServer);
        TEST_ASSERT_EQUAL_INT(302, fwlinkServer.lastStatusCode);

        WebServer ncsiServer(80);
        WifiPortalApiService::handleApiNcsiTxt(ncsiServer);
        TEST_ASSERT_EQUAL_INT(200, ncsiServer.lastStatusCode);
    }

    TEST_ASSERT_EQUAL_INT(kProbeBursts * 4, uiActivityCalls);
    TEST_ASSERT_EQUAL_UINT32(0, countedSerial.totalLogs());
}

void test_static_http_routine_logs_are_not_emitted() {
    const std::string root = projectRoot();
    const std::string helperText = readFile(root + "/src/wifi_manager_helpers.cpp");
    const std::string routesText = readFile(root + "/src/wifi_routes.cpp");
    const std::string lifecycleText = readFile(root + "/src/wifi_manager_lifecycle.cpp");
    const std::string serveBody = extractFunctionBody(
        helperText, "bool serveLittleFSFileHelper(WebServer& server_, const char* path, const char* contentType)");
    const std::string notFoundBody = extractFunctionBody(
        lifecycleText, "void WiFiManager::handleNotFound()");

    TEST_ASSERT_EQUAL(std::string::npos,
                      routesText.find("Serial.printf(\"[HTTP] 200 / -> /index.html"));
    TEST_ASSERT_EQUAL(std::string::npos,
                      routesText.find("WIFI_LOG(\"[HTTP] 200 / -> /index.html"));

    TEST_ASSERT_EQUAL(std::string::npos, serveBody.find("Serial.printf(\"[HTTP] 200"));
    TEST_ASSERT_EQUAL(std::string::npos, serveBody.find("Serial.printf(\"[HTTP] MISS"));
    TEST_ASSERT_EQUAL(std::string::npos, serveBody.find("WIFI_LOG(\"[HTTP] 200 %s -> %s.gz"));
    TEST_ASSERT_EQUAL(std::string::npos, serveBody.find("WIFI_LOG(\"[HTTP] MISS %s"));
    TEST_ASSERT_EQUAL(std::string::npos, serveBody.find("WIFI_LOG(\"[HTTP] 200 %s (%u bytes)"));

    TEST_ASSERT_EQUAL(std::string::npos, notFoundBody.find("Serial.printf(\"[HTTP] 404"));
    TEST_ASSERT_EQUAL(std::string::npos, notFoundBody.find("WIFI_LOG(\"[HTTP] 404"));
}

void test_obd_routine_success_logs_are_removed() {
    const std::string root = projectRoot();
    const std::string stateText = readFile(root + "/src/modules/obd/obd_runtime_state_machine.cpp");
    const std::string clientText = readFile(root + "/src/modules/obd/obd_ble_client.cpp");
    const std::string moduleText = readFile(root + "/src/modules/obd/obd_runtime_module.cpp");
    const std::string transitionBody = extractFunctionBody(
        stateText, "void ObdRuntimeModule::transitionTo(ObdConnectionState newState, uint32_t nowMs)");
    const std::string connectBody = extractFunctionBody(
        clientText, "bool ObdBleClient::connect(const char* address, uint8_t addrType, uint32_t timeoutMs, bool preferCachedAttributes)");
    const std::string discoverBody = extractFunctionBody(
        clientText, "bool ObdBleClient::discoverServices()");
    const std::string subscribeBody = extractFunctionBody(
        clientText, "bool ObdBleClient::subscribeNotify(void (*callback)(const uint8_t* data, size_t len))");

    TEST_ASSERT_EQUAL(std::string::npos, stateText.find("#include \"obd_log.h\""));
    TEST_ASSERT_EQUAL(std::string::npos, clientText.find("#include \"obd_log.h\""));
    TEST_ASSERT_EQUAL(std::string::npos, moduleText.find("#include \"obd_log.h\""));

    TEST_ASSERT_EQUAL(std::string::npos, transitionBody.find("OBD_LOGF(\"[OBD] %s -> %s"));
    TEST_ASSERT_EQUAL(std::string::npos, transitionBody.find("Serial."));

    TEST_ASSERT_EQUAL(std::string::npos, connectBody.find("OBD_LOGF(\"[OBD] connect addr="));
    TEST_ASSERT_EQUAL(std::string::npos, connectBody.find("Serial.printf(\"[OBD] connect addr="));

    TEST_ASSERT_EQUAL(std::string::npos, discoverBody.find("OBD_LOGLN(\"[OBD] discoverServices: OK\")"));
    TEST_ASSERT_EQUAL(std::string::npos, discoverBody.find("Serial.println(\"[OBD] discoverServices: OK\")"));

    TEST_ASSERT_EQUAL(std::string::npos, subscribeBody.find("OBD_LOGLN(\"[OBD] subscribeNotify: OK\")"));
    TEST_ASSERT_EQUAL(std::string::npos, subscribeBody.find("Serial.println(\"[OBD] subscribeNotify: OK\")"));

    TEST_ASSERT_EQUAL(std::string::npos, moduleText.find("OBD_LOGF(\"[OBD] begin addr="));
    TEST_ASSERT_EQUAL(std::string::npos, moduleText.find("Serial.printf(\"[OBD] begin addr="));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ping_marks_ui_activity_and_returns_ok);
    RUN_TEST(test_generate_204_marks_ui_activity_and_returns_empty_204);
    RUN_TEST(test_gen_204_marks_ui_activity_and_returns_empty_204);
    RUN_TEST(test_hotspot_detect_marks_ui_activity_and_redirects_to_settings);
    RUN_TEST(test_fwlink_redirects_to_settings);
    RUN_TEST(test_ncsi_returns_expected_body);
    RUN_TEST(test_probe_burst_does_not_emit_serial_logs);
    RUN_TEST(test_static_http_routine_logs_are_not_emitted);
    RUN_TEST(test_obd_routine_success_logs_are_removed);
    return UNITY_END();
}
