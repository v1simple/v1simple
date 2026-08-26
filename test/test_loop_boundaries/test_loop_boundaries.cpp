#include <unity.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/runtime_coordinator.h"

namespace {

enum class Call {
    BEGIN,
    CONNECTION_RUNTIME,
    ACCEPT_CONNECTION,
    SHOW_SCAN,
    MARK_SCAN,
    CONNECTION_PRESENTATION,
    POWER,
    TOUCH,
    POWER_PRESENTATION,
    SETTINGS_PREEMPT,
    TAP,
    READY_GATE,
    BLE_RUNTIME,
    BLE_QUEUE,
    CONNECTION_CYCLE,
    OBD,
    PARSING,
    OBSERVE_ALP,
    ALP_STATE,
    GPS,
    SPEED,
    ALERT,
    DISPLAY_EDGE,
    DISPLAY_PRESENTATION,
    CONNECTION_DISPATCH,
    PERSISTENCE,
    FINISH,
};

struct FakeConnectionSnapshot {
    bool connected = true;
    bool backpressured = false;
    bool overloaded = false;
    bool requestShowInitialScanning = true;
};

struct FakeDisplayEdges {
    bool parsed = true;
};

struct MaintenanceCall {
    uint32_t nowMs = 0;
    bool bleConnected = false;
    bool bleBackpressured = false;
    bool overloaded = false;
    bool forceTailBleDrainPending = false;
};

struct FinishCall {
    bool bleBackpressured = false;
    uint32_t loopStartUs = 0;
    bool forceBleDrain = false;
};

struct FakeDriveRuntime {
    bool activeValue = true;
    bool presentationOwned = false;
    bool acquirePresentationDuringPower = false;
    bool inSettings = false;
    bool liveAlertPreemptsSettings = false;
    bool settingsPreempted = false;
    bool queueBackpressured = false;
    FakeConnectionSnapshot connection;
    std::vector<Call> calls;
    std::vector<MaintenanceCall> maintenanceCalls;
    std::vector<FinishCall> finishCalls;

    bool active() const { return activeValue; }

    DriveLoopTiming beginDriveLoop() {
        calls.push_back(Call::BEGIN);
        return {1000, 25};
    }

    FakeConnectionSnapshot processConnectionRuntime(uint32_t) {
        calls.push_back(Call::CONNECTION_RUNTIME);
        return connection;
    }

    void acceptConnectionSnapshot(const FakeConnectionSnapshot&) { calls.push_back(Call::ACCEPT_CONNECTION); }
    void showInitialScanningScreen() { calls.push_back(Call::SHOW_SCAN); }
    void markInitialScanningScreenHandled() { calls.push_back(Call::MARK_SCAN); }
    bool powerOwnsPresentation() const { return presentationOwned; }
    void presentConnectionState(uint32_t, const FakeConnectionSnapshot&) {
        calls.push_back(Call::CONNECTION_PRESENTATION);
    }

    void processPower(uint32_t) {
        calls.push_back(Call::POWER);
        if (acquirePresentationDuringPower) {
            presentationOwned = true;
        }
    }

    bool processTouch(uint32_t) {
        calls.push_back(Call::TOUCH);
        return inSettings;
    }

    void servicePowerDisplayOwnership(uint32_t) { calls.push_back(Call::POWER_PRESENTATION); }
    bool preemptSettingsForLiveAlert() {
        calls.push_back(Call::SETTINGS_PREEMPT);
        settingsPreempted = liveAlertPreemptsSettings;
        return settingsPreempted;
    }
    void processTapGesture(uint32_t) { calls.push_back(Call::TAP); }
    void openBootReadyGate(uint32_t) { calls.push_back(Call::READY_GATE); }
    void processBleRuntime() { calls.push_back(Call::BLE_RUNTIME); }
    void processBleQueue() { calls.push_back(Call::BLE_QUEUE); }
    bool bleQueueBackpressured() const { return queueBackpressured; }
    void processConnectionCycle(uint32_t, bool) { calls.push_back(Call::CONNECTION_CYCLE); }
    void processObd(uint32_t, bool) { calls.push_back(Call::OBD); }
    void processAlp(uint32_t) { calls.push_back(Call::PARSING); }
    void observeAlpProductState(uint32_t) { calls.push_back(Call::OBSERVE_ALP); }
    void processAlpPresentationAndPower(uint32_t) { calls.push_back(Call::ALP_STATE); }
    void processGps(uint32_t) { calls.push_back(Call::GPS); }
    void processSpeed(uint32_t) { calls.push_back(Call::SPEED); }
    void processSpeedAlert(uint32_t) { calls.push_back(Call::ALERT); }

    FakeDisplayEdges consumeDisplayEdges() {
        calls.push_back(Call::DISPLAY_EDGE);
        return {};
    }

    void presentDisplay(const FakeDisplayEdges&, bool) { calls.push_back(Call::DISPLAY_PRESENTATION); }

    DriveLoopDispatch processConnectionDispatch(bool) {
        calls.push_back(Call::CONNECTION_DISPATCH);
        return {30, true};
    }

    void processPeriodicMaintenance(uint32_t nowMs, bool bleConnected, bool bleBackpressured, bool overloaded,
                                    bool forceTailBleDrainPending) {
        calls.push_back(Call::PERSISTENCE);
        maintenanceCalls.push_back(
            {nowMs, bleConnected, bleBackpressured, overloaded, forceTailBleDrainPending});
    }

    void finishDriveLoop(bool bleBackpressured, uint32_t loopStartUs, bool forceBleDrain) {
        calls.push_back(Call::FINISH);
        finishCalls.push_back({bleBackpressured, loopStartUs, forceBleDrain});
    }
};

bool called(const FakeDriveRuntime& runtime, Call call) {
    return std::find(runtime.calls.begin(), runtime.calls.end(), call) != runtime.calls.end();
}

void assertCalls(const FakeDriveRuntime& runtime, const std::vector<Call>& expected) {
    TEST_ASSERT_EQUAL_UINT(expected.size(), runtime.calls.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        TEST_ASSERT_EQUAL_INT(static_cast<int>(expected[i]), static_cast<int>(runtime.calls[i]));
    }
}

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

} // namespace

void setUp() {}
void tearDown() {}

void test_drive_coordinator_executes_production_phase_order() {
    FakeDriveRuntime runtime;

    DriveLoopCoordinator::tick(runtime);

    assertCalls(runtime,
                {Call::BEGIN,
                 Call::CONNECTION_RUNTIME,
                 Call::ACCEPT_CONNECTION,
                 Call::SHOW_SCAN,
                 Call::MARK_SCAN,
                 Call::CONNECTION_PRESENTATION,
                 Call::POWER,
                 Call::TOUCH,
                 Call::POWER_PRESENTATION,
                 Call::TAP,
                 Call::READY_GATE,
                 Call::BLE_RUNTIME,
                 Call::BLE_QUEUE,
                 Call::CONNECTION_CYCLE,
                 Call::OBD,
                 Call::PARSING,
                 Call::OBSERVE_ALP,
                 Call::ALP_STATE,
                 Call::GPS,
                 Call::SPEED,
                 Call::ALERT,
                 Call::DISPLAY_EDGE,
                 Call::DISPLAY_PRESENTATION,
                 Call::CONNECTION_DISPATCH,
                 Call::PERSISTENCE,
                 Call::FINISH});
}

void test_power_owner_suppresses_touch_and_presentations_but_keeps_runtime_live() {
    FakeDriveRuntime runtime;
    runtime.presentationOwned = true;

    DriveLoopCoordinator::tick(runtime);

    TEST_ASSERT_FALSE(called(runtime, Call::SHOW_SCAN));
    TEST_ASSERT_FALSE(called(runtime, Call::CONNECTION_PRESENTATION));
    TEST_ASSERT_FALSE(called(runtime, Call::TOUCH));
    TEST_ASSERT_FALSE(called(runtime, Call::TAP));
    TEST_ASSERT_FALSE(called(runtime, Call::DISPLAY_PRESENTATION));
    TEST_ASSERT_TRUE(called(runtime, Call::CONNECTION_RUNTIME));
    TEST_ASSERT_TRUE(called(runtime, Call::BLE_RUNTIME));
    TEST_ASSERT_TRUE(called(runtime, Call::BLE_QUEUE));
    TEST_ASSERT_TRUE(called(runtime, Call::CONNECTION_CYCLE));
    TEST_ASSERT_TRUE(called(runtime, Call::CONNECTION_DISPATCH));
    TEST_ASSERT_TRUE(called(runtime, Call::DISPLAY_EDGE));
    TEST_ASSERT_TRUE(called(runtime, Call::PERSISTENCE));
}

void test_warning_acquired_during_power_phase_suppresses_same_tick_touch_and_display() {
    FakeDriveRuntime runtime;
    runtime.acquirePresentationDuringPower = true;

    DriveLoopCoordinator::tick(runtime);

    TEST_ASSERT_TRUE(called(runtime, Call::POWER));
    TEST_ASSERT_FALSE(called(runtime, Call::TOUCH));
    TEST_ASSERT_FALSE(called(runtime, Call::TAP));
    TEST_ASSERT_TRUE(called(runtime, Call::DISPLAY_EDGE));
    TEST_ASSERT_FALSE(called(runtime, Call::DISPLAY_PRESENTATION));
    TEST_ASSERT_TRUE(called(runtime, Call::BLE_RUNTIME));
    TEST_ASSERT_TRUE(called(runtime, Call::CONNECTION_DISPATCH));
}

void test_settings_remains_open_after_alp_processing_without_live_alert() {
    FakeDriveRuntime runtime;
    runtime.inSettings = true;

    DriveLoopCoordinator::tick(runtime);

    assertCalls(runtime,
                {Call::BEGIN,
                 Call::CONNECTION_RUNTIME,
                 Call::ACCEPT_CONNECTION,
                 Call::SHOW_SCAN,
                 Call::MARK_SCAN,
                 Call::CONNECTION_PRESENTATION,
                 Call::POWER,
                 Call::TOUCH,
                 Call::POWER_PRESENTATION,
                 Call::PARSING,
                 Call::SETTINGS_PREEMPT,
                 Call::OBSERVE_ALP,
                 Call::PERSISTENCE,
                 Call::FINISH});
    TEST_ASSERT_EQUAL(1, std::count(runtime.calls.begin(), runtime.calls.end(), Call::PARSING));
    TEST_ASSERT_FALSE(runtime.settingsPreempted);
    TEST_ASSERT_EQUAL_UINT(1, runtime.maintenanceCalls.size());
    TEST_ASSERT_EQUAL_UINT32(25, runtime.maintenanceCalls[0].nowMs);
    TEST_ASSERT_TRUE(runtime.maintenanceCalls[0].bleConnected);
    TEST_ASSERT_FALSE(runtime.maintenanceCalls[0].bleBackpressured);
    TEST_ASSERT_FALSE(runtime.maintenanceCalls[0].overloaded);
    TEST_ASSERT_TRUE(runtime.maintenanceCalls[0].forceTailBleDrainPending);
    TEST_ASSERT_EQUAL_UINT(1, runtime.finishCalls.size());
    TEST_ASSERT_FALSE(runtime.finishCalls[0].bleBackpressured);
    TEST_ASSERT_EQUAL_UINT32(1000, runtime.finishCalls[0].loopStartUs);
    TEST_ASSERT_TRUE(runtime.finishCalls[0].forceBleDrain);
}

void test_live_alert_preempts_settings_after_single_alp_processing_pass() {
    FakeDriveRuntime runtime;
    runtime.inSettings = true;
    runtime.liveAlertPreemptsSettings = true;

    DriveLoopCoordinator::tick(runtime);

    assertCalls(runtime,
                {Call::BEGIN,
                 Call::CONNECTION_RUNTIME,
                 Call::ACCEPT_CONNECTION,
                 Call::SHOW_SCAN,
                 Call::MARK_SCAN,
                 Call::CONNECTION_PRESENTATION,
                 Call::POWER,
                 Call::TOUCH,
                 Call::POWER_PRESENTATION,
                 Call::PARSING,
                 Call::SETTINGS_PREEMPT,
                 Call::TAP,
                 Call::READY_GATE,
                 Call::BLE_RUNTIME,
                 Call::BLE_QUEUE,
                 Call::CONNECTION_CYCLE,
                 Call::OBD,
                 Call::OBSERVE_ALP,
                 Call::ALP_STATE,
                 Call::GPS,
                 Call::SPEED,
                 Call::ALERT,
                 Call::DISPLAY_EDGE,
                 Call::DISPLAY_PRESENTATION,
                 Call::CONNECTION_DISPATCH,
                 Call::PERSISTENCE,
                 Call::FINISH});
    TEST_ASSERT_EQUAL(1, std::count(runtime.calls.begin(), runtime.calls.end(), Call::PARSING));
    TEST_ASSERT_TRUE(runtime.settingsPreempted);
    TEST_ASSERT_EQUAL_UINT(1, runtime.maintenanceCalls.size());
    TEST_ASSERT_EQUAL_UINT32(30, runtime.maintenanceCalls[0].nowMs);
    TEST_ASSERT_TRUE(runtime.maintenanceCalls[0].bleConnected);
    TEST_ASSERT_FALSE(runtime.maintenanceCalls[0].bleBackpressured);
    TEST_ASSERT_FALSE(runtime.maintenanceCalls[0].overloaded);
    TEST_ASSERT_FALSE(runtime.maintenanceCalls[0].forceTailBleDrainPending);
    TEST_ASSERT_EQUAL_UINT(1, runtime.finishCalls.size());
    TEST_ASSERT_FALSE(runtime.finishCalls[0].bleBackpressured);
    TEST_ASSERT_EQUAL_UINT32(1000, runtime.finishCalls[0].loopStartUs);
    TEST_ASSERT_FALSE(runtime.finishCalls[0].forceBleDrain);
}

void test_inactive_drive_runtime_executes_no_phase() {
    FakeDriveRuntime runtime;
    runtime.activeValue = false;

    DriveLoopCoordinator::tick(runtime);

    TEST_ASSERT_TRUE(runtime.calls.empty());
}

void test_drive_runtime_replaces_global_provider_and_loop_wrapper_graph() {
    const std::string header = readFile(projectRoot() + "/src/drive_runtime.h");
    const std::string main = readFile(projectRoot() + "/src/main.cpp");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, header.find("class DriveRuntime final"));
    TEST_ASSERT_EQUAL(std::string::npos, header.find("struct Providers"));
    TEST_ASSERT_EQUAL(std::string::npos, main.find("#include \"main_globals.h\""));
    TEST_ASSERT_EQUAL(std::string::npos, main.find("LoopIngestModule"));
    TEST_ASSERT_EQUAL(std::string::npos, main.find("LoopTailModule"));
}

void test_drive_runtime_composition_has_no_maintenance_wifi_dependency() {
    const std::string header = readFile(projectRoot() + "/src/drive_runtime.h");

    TEST_ASSERT_EQUAL(std::string::npos, header.find("WiFiManager"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_drive_coordinator_executes_production_phase_order);
    RUN_TEST(test_power_owner_suppresses_touch_and_presentations_but_keeps_runtime_live);
    RUN_TEST(test_warning_acquired_during_power_phase_suppresses_same_tick_touch_and_display);
    RUN_TEST(test_settings_remains_open_after_alp_processing_without_live_alert);
    RUN_TEST(test_live_alert_preempts_settings_after_single_alp_processing_pass);
    RUN_TEST(test_inactive_drive_runtime_executes_no_phase);
    RUN_TEST(test_drive_runtime_replaces_global_provider_and_loop_wrapper_graph);
    RUN_TEST(test_drive_runtime_composition_has_no_maintenance_wifi_dependency);
    return UNITY_END();
}
