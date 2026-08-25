#include <unity.h>

#include "../mocks/WebServer.h"
#include "../../src/modules/wifi/wifi_system_api_service.cpp"

namespace {

struct Probe {
    int persistCalls = 0;
    int prepareCalls = 0;
    int restartCalls = 0;
    bool prepareResult = true;
};

void persist(void* context) { ++static_cast<Probe*>(context)->persistCalls; }

bool prepare(void* context) {
    auto* probe = static_cast<Probe*>(context);
    ++probe->prepareCalls;
    return probe->prepareResult;
}

void restart(void* context) { ++static_cast<Probe*>(context)->restartCalls; }

WifiSystemApiService::RebootRuntime runtimeFor(Probe& probe) {
    WifiSystemApiService::RebootRuntime runtime;
    runtime.maintenanceBootActive = true;
    runtime.persistSettings = persist;
    runtime.prepareCleanRestart = prepare;
    runtime.restart = restart;
    runtime.ctx = &probe;
    return runtime;
}

} // namespace

void setUp() {}
void tearDown() {}

void test_failed_writer_shutdown_returns_503_without_restart() {
    WebServer server(80);
    Probe probe;
    probe.prepareResult = false;

    WifiSystemApiService::handleApiRebootNormal(server, runtimeFor(probe));

    TEST_ASSERT_EQUAL_INT(503, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("{\"success\":false,\"error\":\"persistence_shutdown_failed\"}",
                             server.lastBody.c_str());
    TEST_ASSERT_EQUAL_INT(1, probe.persistCalls);
    TEST_ASSERT_EQUAL_INT(1, probe.prepareCalls);
    TEST_ASSERT_EQUAL_INT(0, probe.restartCalls);
}

void test_confirmed_writer_shutdown_allows_restart() {
    WebServer server(80);
    Probe probe;

    WifiSystemApiService::handleApiRebootNormal(server, runtimeFor(probe));

    TEST_ASSERT_EQUAL_INT(202, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, probe.persistCalls);
    TEST_ASSERT_EQUAL_INT(1, probe.prepareCalls);
    TEST_ASSERT_EQUAL_INT(1, probe.restartCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_failed_writer_shutdown_returns_503_without_restart);
    RUN_TEST(test_confirmed_writer_shutdown_allows_restart);
    return UNITY_END();
}
