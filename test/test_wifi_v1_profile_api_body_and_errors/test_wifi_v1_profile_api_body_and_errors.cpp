// Regression coverage for two defects in the V1 profile HTTP API:
//
//   #19  handleApiProfileSave built its 500 body by string-concatenating the
//        store's saveError straight into JSON.  A saveError containing a quote
//        or a backslash produced malformed JSON and the UI's res.json() threw.
//
//   #15  Investigated and found NOT fixable at this layer.  Arduino WebServer
//        buffers the whole body before dispatch, and WebServer::arg() returns
//        String BY VALUE, so any access allocates a further full copy.  Binding
//        it once into a named local is therefore already the minimum cost — the
//        "redundant second copy" the backlog described does not exist.  An
//        attempted fix that replaced the local with repeated inline arg() calls
//        made things strictly worse (restore allocated a 128 KB body twice).
//        What is pinned here is the oversize-payload cap behaviour plus a source
//        contract enforcing one binding per handler, following the pattern in
//        test_stack_hygiene_source_contract.

#include <unity.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../mocks/mock_heap_caps_state.h"
#include "../mocks/esp_heap_caps.h"
#include "../../src/modules/wifi/wifi_json_document.h"
#include "../../src/modules/wifi/wifi_v1_profile_api_service.h"
#include "../../src/modules/wifi/wifi_v1_profile_api_service.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace {

struct FakeRuntime {
    bool parseSettingsOk = true;
    bool saveOk = true;
    String saveError = "";

    int parseSettingsCalls = 0;
    int saveCalls = 0;
    int writeUserBytesCalls = 0;
    int backupCalls = 0;
    bool connected = true;
    bool writeUserBytesResult = true;
};

WifiV1ProfileApiService::Runtime makeRuntime(FakeRuntime& rt) {
    WifiV1ProfileApiService::Runtime runtime{};
    runtime.parseSettingsJson = [](const JsonObject& /*settingsObj*/, uint8_t outBytes[6], void* ctx) {
        auto* rtp = static_cast<FakeRuntime*>(ctx);
        rtp->parseSettingsCalls++;
        if (!rtp->parseSettingsOk) {
            return false;
        }
        memset(outBytes, 0xFF, 6);
        return true;
    };
    runtime.parseSettingsJsonCtx = &rt;
    runtime.saveProfile = [](const String& /*name*/,
                             const String& /*description*/,
                             bool /*displayOn*/,
                             const uint8_t /*inBytes*/[6],
                             String& error,
                             void* ctx) {
        auto* rtp = static_cast<FakeRuntime*>(ctx);
        rtp->saveCalls++;
        if (!rtp->saveOk) {
            error = rtp->saveError;
            return false;
        }
        return true;
    };
    runtime.saveProfileCtx = &rt;
    runtime.backupToSd = [](void* ctx) { static_cast<FakeRuntime*>(ctx)->backupCalls++; };
    runtime.backupToSdCtx = &rt;
    runtime.v1Connected = [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->connected; };
    runtime.v1ConnectedCtx = &rt;
    runtime.writeUserBytes = [](const uint8_t /*inBytes*/[6], void* ctx) {
        auto* rtp = static_cast<FakeRuntime*>(ctx);
        rtp->writeUserBytesCalls++;
        return rtp->writeUserBytesResult;
    };
    runtime.writeUserBytesCtx = &rt;
    return runtime;
}

bool alwaysAllow(void* /*ctx*/) {
    return true;
}

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

// Reparses the response body.  This is the assertion the old concatenated
// builder could not survive.
bool responseParsesAsJson(const WebServer& server, JsonDocument& out) {
    return deserializeJson(out, server.lastBody.c_str()) == DeserializationError::Ok;
}

String oversizeBody(size_t totalBytes) {
    std::string filler(totalBytes, 'x');
    std::string json = "{\"name\":\"";
    json += filler;
    json += "\",\"settings\":{\"byte0\":1}}";
    return String(json.c_str());
}

std::string readProjectFile(const char* relativePath) {
    const std::filesystem::path path = std::filesystem::path(PROJECT_DIR) / relativePath;
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

}  // namespace

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    mock_reset_heap_caps();
}

void tearDown() {}

// ---------------------------------------------------------------------------
// #19 — profile-save error JSON must be escaped
// ---------------------------------------------------------------------------

void test_save_error_with_quotes_and_backslashes_stays_valid_json() {
    WebServer server(80);
    FakeRuntime rt;
    rt.saveOk = false;
    rt.saveError = "open \"/profiles/a\\b.json\" failed";
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"settings\":{\"byte0\":3}}");

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);

    JsonDocument parsed;
    TEST_ASSERT_TRUE_MESSAGE(responseParsesAsJson(server, parsed),
                             "500 body must parse as JSON when saveError contains quotes/backslashes");
    TEST_ASSERT_EQUAL_STRING("open \"/profiles/a\\b.json\" failed", parsed["error"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("open \"/profiles/a\\b.json\" failed", parsed["message"].as<const char*>());
}

void test_save_error_with_control_characters_stays_valid_json() {
    WebServer server(80);
    FakeRuntime rt;
    rt.saveOk = false;
    rt.saveError = "write failed\nretry \"once\"\ttab";
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"settings\":{\"byte0\":3}}");

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);

    JsonDocument parsed;
    TEST_ASSERT_TRUE_MESSAGE(responseParsesAsJson(server, parsed),
                             "500 body must parse as JSON when saveError contains control characters");
    TEST_ASSERT_EQUAL_STRING("write failed\nretry \"once\"\ttab", parsed["error"].as<const char*>());
}

void test_plain_save_error_still_reports_error_field_verbatim() {
    WebServer server(80);
    FakeRuntime rt;
    rt.saveOk = false;
    rt.saveError = "disk full";
    server.setArg("plain", "{\"name\":\"RoadTrip\",\"settings\":{\"byte0\":3}}");

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(500, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"error\":\"disk full\""));
    TEST_ASSERT_EQUAL_INT(0, rt.backupCalls);

    JsonDocument parsed;
    TEST_ASSERT_TRUE(responseParsesAsJson(server, parsed));
    TEST_ASSERT_EQUAL_STRING("disk full", parsed["error"].as<const char*>());
}

// ---------------------------------------------------------------------------
// #15 — oversize-payload caps must keep working after the copy removal
// ---------------------------------------------------------------------------

void test_profile_save_rejects_oversize_payload_without_saving() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", oversizeBody(4200));

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Payload too large"));
    TEST_ASSERT_EQUAL_INT(0, rt.parseSettingsCalls);
    TEST_ASSERT_EQUAL_INT(0, rt.saveCalls);
}

void test_profile_save_accepts_payload_just_under_the_cap() {
    WebServer server(80);
    FakeRuntime rt;
    // 4000 bytes of filler plus the JSON scaffolding stays under 4096.
    server.setArg("plain", oversizeBody(4000));

    WifiV1ProfileApiService::handleApiProfileSave(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, rt.saveCalls);
}

void test_profile_push_rejects_oversize_payload_without_writing() {
    WebServer server(80);
    FakeRuntime rt;
    server.setArg("plain", oversizeBody(4200));

    WifiV1ProfileApiService::handleApiSettingsPush(server, makeRuntime(rt), alwaysAllow, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "Payload too large"));
    TEST_ASSERT_EQUAL_INT(0, rt.writeUserBytesCalls);
}

// ---------------------------------------------------------------------------
// #15 — structural contract.  The redundant second copy is not observable
// through the HTTP surface, so it is pinned at the source layer, and the
// comment is pinned with it so nobody "closes" the heap risk in a comment
// without closing it in code.
// ---------------------------------------------------------------------------

// WebServer::arg() returns String BY VALUE — every call allocates a fresh full
// copy of the request body. So the minimum achievable cost in a handler is to
// bind it ONCE and reuse the binding. An earlier attempt at #15 replaced the
// named local with repeated inline server.arg("plain") calls; that made the
// restore handler allocate a whole 128 KB body twice (once to measure length,
// once to parse) and the profile save handler three times. This test pins the
// one-binding-per-handler rule so that regression cannot come back.
void test_post_handlers_bind_the_request_body_exactly_once_per_handler() {
    struct Expectation {
        const char* path;
        int maxArgCalls; // one per handler that reads a body
    };
    const Expectation sources[] = {
        // save, delete, push
        {"src/modules/wifi/wifi_v1_profile_api_service.cpp", 3},
        // restore (backup-now takes no body)
        {"src/modules/wifi/backup_api_service.cpp", 1},
    };

    for (const Expectation& expected : sources) {
        const std::string source = readProjectFile(expected.path);
        TEST_ASSERT_FALSE_MESSAGE(source.empty(), expected.path);

        int argCalls = 0;
        const std::string needle = "server.arg(\"plain\")";
        for (size_t at = source.find(needle); at != std::string::npos; at = source.find(needle, at + 1)) {
            ++argCalls;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected.maxArgCalls, argCalls, expected.path);

        // And the binding must actually be a named local, not an inline temporary.
        TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos,
                                      source.find("const String body = server.arg(\"plain\")"),
                                      expected.path);
    }
}

void test_body_cap_comments_do_not_claim_the_transport_risk_is_closed() {
    const char* sources[] = {
        "src/modules/wifi/wifi_v1_profile_api_service.cpp",
        "src/modules/wifi/backup_api_service.cpp",
    };

    for (const char* relativePath : sources) {
        const std::string source = readProjectFile(relativePath);
        TEST_ASSERT_FALSE_MESSAGE(source.empty(), relativePath);
        // The cap is an application-level limit; it does not bound what the
        // transport already allocated.
        TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos,
                                      source.find("does NOT bound the transport allocation"),
                                      relativePath);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_save_error_with_quotes_and_backslashes_stays_valid_json);
    RUN_TEST(test_save_error_with_control_characters_stays_valid_json);
    RUN_TEST(test_plain_save_error_still_reports_error_field_verbatim);
    RUN_TEST(test_profile_save_rejects_oversize_payload_without_saving);
    RUN_TEST(test_profile_save_accepts_payload_just_under_the_cap);
    RUN_TEST(test_profile_push_rejects_oversize_payload_without_writing);
    RUN_TEST(test_post_handlers_bind_the_request_body_exactly_once_per_handler);
    RUN_TEST(test_body_cap_comments_do_not_claim_the_transport_risk_is_closed);
    return UNITY_END();
}
