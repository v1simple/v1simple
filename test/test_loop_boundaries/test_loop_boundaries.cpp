#include <unity.h>

#include <fstream>
#include <sstream>
#include <string>

namespace {
std::string projectRoot() {
#ifdef PROJECT_DIR
    return PROJECT_DIR;
#else
    return ".";
#endif
}

std::string readFile(const std::string& path) {
    std::ifstream input(path);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

std::string extractFunctionBody(const std::string& source, const std::string& signature) {
    const size_t signatureAt = source.find(signature);
    if (signatureAt == std::string::npos) {
        return {};
    }
    const size_t bodyAt = source.find('{', signatureAt);
    int depth = 0;
    for (size_t i = bodyAt; i < source.size(); ++i) {
        if (source[i] == '{') {
            ++depth;
        } else if (source[i] == '}' && --depth == 0) {
            return source.substr(bodyAt, i - bodyAt + 1);
        }
    }
    return {};
}

void assertOrdered(const std::string& source, const char* first, const char* second) {
    const size_t firstAt = source.find(first);
    const size_t secondAt = source.find(second);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, firstAt);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, secondAt);
    TEST_ASSERT_TRUE(firstAt < secondAt);
}
} // namespace

void setUp() {}
void tearDown() {}

void test_drive_tick_preserves_ingest_sensor_display_finalize_order() {
    const std::string source = readFile(projectRoot() + "/src/drive_runtime.cpp");
    const std::string tick = extractFunctionBody(source, "void DriveRuntime::tick()");

    assertOrdered(tick, "connectionRuntime_.process", "power_.process");
    assertOrdered(tick, "power_.process", "ble_.process");
    assertOrdered(tick, "ble_.process", "processConnectionCycle");
    assertOrdered(tick, "processConnectionCycle", "processObd");
    assertOrdered(tick, "processObd(nowMs, bleConnectedNow);", "const AlpStatus alpStatus");
    assertOrdered(tick, "const AlpStatus alpStatus", "gps_.update");
    assertOrdered(tick, "gps_.update", "speed_.update");
    assertOrdered(tick, "speed_.update", "processDisplay");
    assertOrdered(tick, "processDisplay", "connectionDispatch_.process");
    assertOrdered(tick, "connectionDispatch_.process", "processPeriodicMaintenance(dispatchNowMs");
    assertOrdered(tick, "processPeriodicMaintenance(dispatchNowMs",
                  "state_.lastLoopUs = finishLoop(bleBackpressure");
}

void test_drive_runtime_replaces_global_provider_and_loop_wrapper_graph() {
    const std::string header = readFile(projectRoot() + "/src/drive_runtime.h");
    const std::string source = readFile(projectRoot() + "/src/drive_runtime.cpp");
    const std::string main = readFile(projectRoot() + "/src/main.cpp");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, header.find("class DriveRuntime final"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, header.find("void tick();"));
    TEST_ASSERT_EQUAL(std::string::npos, header.find("struct Providers"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("ProviderCallbackBindings"));
    TEST_ASSERT_EQUAL(std::string::npos, main.find("#include \"main_globals.h\""));
    TEST_ASSERT_EQUAL(std::string::npos, main.find("LoopIngestModule"));
    TEST_ASSERT_EQUAL(std::string::npos, main.find("LoopTailModule"));
}

void test_drive_runtime_has_no_maintenance_wifi_dependency_or_start_path() {
    const std::string header = readFile(projectRoot() + "/src/drive_runtime.h");
    const std::string source = readFile(projectRoot() + "/src/drive_runtime.cpp");

    TEST_ASSERT_EQUAL(std::string::npos, header.find("WiFiManager"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("startSetupMode("));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("wifiManager"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_drive_tick_preserves_ingest_sensor_display_finalize_order);
    RUN_TEST(test_drive_runtime_replaces_global_provider_and_loop_wrapper_graph);
    RUN_TEST(test_drive_runtime_has_no_maintenance_wifi_dependency_or_start_path);
    return UNITY_END();
}
