#include <unity.h>
#include <cstring>

#include "../../include/json_stream_response.h"
#include "../../src/modules/wifi/wifi_api_response.h"

#ifndef ARDUINO
SerialClass Serial;
#endif

namespace {

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

}  // namespace

void setUp() {}

void tearDown() {}

void test_send_json_stream_serializes_const_document() {
    WebServer server(80);
    JsonDocument doc;
    doc["ok"] = true;
    doc["count"] = 3;

    const JsonDocument& constDoc = doc;
    sendJsonStream(server, constDoc);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("application/json", server.lastContentType.c_str());
    TEST_ASSERT_TRUE(responseContains(server, "\"ok\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"count\":3"));
}

void test_send_json_stream_honors_status_code() {
    WebServer server(80);
    JsonDocument doc;
    doc["error"] = "bad";

    sendJsonStream(server, doc, 400);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"bad\""));
}

void test_wifi_api_response_uses_shared_stream_helper() {
    WebServer server(80);
    JsonDocument doc;
    WifiApiResponse::setErrorAndMessage(doc, "Nope");

    WifiApiResponse::sendJsonDocument(server, 503, doc);

    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("application/json", server.lastContentType.c_str());
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"Nope\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"message\":\"Nope\""));
}

void test_send_serialized_json_sends_cached_bytes() {
    WebServer server(80);
    const char payload[] = "{\"cached\":true}";

    sendSerializedJson(server, payload, sizeof(payload) - 1u, 202);

    TEST_ASSERT_EQUAL_INT(202, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("application/json", server.lastContentType.c_str());
    TEST_ASSERT_TRUE(responseContains(server, "\"cached\":true"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_send_json_stream_serializes_const_document);
    RUN_TEST(test_send_json_stream_honors_status_code);
    RUN_TEST(test_wifi_api_response_uses_shared_stream_helper);
    RUN_TEST(test_send_serialized_json_sends_cached_bytes);
    return UNITY_END();
}
