#include <unity.h>

#include <vector>

#include "../../src/modules/wifi/wifi_system_api_service.cpp"

namespace {

enum class Event {
    UiActivity,
    Persist,
    MarkClean,
    Delay,
    Restart,
};

std::vector<Event> events;
uint32_t observedDelayMs = 0;

WifiSystemApiService::RebootRuntime makeRuntime(bool maintenance = true) {
    WifiSystemApiService::RebootRuntime runtime;
    runtime.maintenanceBootActive = maintenance;
    runtime.persistSettings = [](void*) { events.push_back(Event::Persist); };
    runtime.markCleanShutdown = [](void*) { events.push_back(Event::MarkClean); };
    runtime.delayBeforeRestart = [](uint32_t delayMs, void*) {
        events.push_back(Event::Delay);
        observedDelayMs = delayMs;
    };
    runtime.restart = [](void*) { events.push_back(Event::Restart); };
    runtime.markUiActivity = [](void*) { events.push_back(Event::UiActivity); };
    return runtime;
}

} // namespace

void setUp() {
    events.clear();
    observedDelayMs = 0;
}

void tearDown() {}

void test_reboot_normal_persists_marks_clean_responds_and_restarts_in_order() {
    WebServer server(80);
    WifiSystemApiService::handleApiRebootNormal(server, makeRuntime());

    TEST_ASSERT_EQUAL_INT(202, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("application/json", server.lastContentType.c_str());
    TEST_ASSERT_EQUAL_STRING("{\"success\":true,\"rebooting\":true,\"target\":\"normal\"}", server.lastBody.c_str());
    TEST_ASSERT_EQUAL_UINT32(100, observedDelayMs);
    TEST_ASSERT_EQUAL_UINT32(5, events.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Event::UiActivity), static_cast<int>(events[0]));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Event::Persist), static_cast<int>(events[1]));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Event::MarkClean), static_cast<int>(events[2]));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Event::Delay), static_cast<int>(events[3]));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Event::Restart), static_cast<int>(events[4]));
}

void test_reboot_normal_rejects_non_maintenance_runtime() {
    WebServer server(80);
    WifiSystemApiService::handleApiRebootNormal(server, makeRuntime(false));

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_NOT_EQUAL(nullptr, std::strstr(server.lastBody.c_str(), "maintenance_mode_required"));
    TEST_ASSERT_EQUAL_UINT32(1, events.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Event::UiActivity), static_cast<int>(events[0]));
}

void test_reboot_normal_fails_closed_when_callbacks_are_missing() {
    WebServer server(80);
    WifiSystemApiService::RebootRuntime runtime = makeRuntime();
    runtime.restart = nullptr;
    WifiSystemApiService::handleApiRebootNormal(server, runtime);

    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_NOT_EQUAL(nullptr, std::strstr(server.lastBody.c_str(), "reboot_runtime_unavailable"));
    TEST_ASSERT_EQUAL_UINT32(1, events.size());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_reboot_normal_persists_marks_clean_responds_and_restarts_in_order);
    RUN_TEST(test_reboot_normal_rejects_non_maintenance_runtime);
    RUN_TEST(test_reboot_normal_fails_closed_when_callbacks_are_missing);
    return UNITY_END();
}
