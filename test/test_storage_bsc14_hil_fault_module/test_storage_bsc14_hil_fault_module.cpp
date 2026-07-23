#include <unity.h>

#include <cstdio>
#include <cstring>

#define V1SIMPLE_HIL_FAULT_CONTROL 1
#include "../../src/modules/hil/hil_fault_controller.cpp"
#include "../../src/modules/hil/hil_fault_serial_module.cpp"
#include "../../src/modules/storage/sd_mutex_hold_lifecycle.cpp"
#include "../../src/modules/storage/storage_bsc14_hil_fault_module.cpp"

namespace {
constexpr char kSessionHashText[] = "9192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeafb0";

struct Fixture {
    bool available = true;
    bool owned = false;
    uint32_t acquireCalls = 0;
    uint32_t releaseCalls = 0;
    char output[8192]{};
    size_t outputLength = 0;
};

bool tryAcquire(void* context) noexcept {
    auto& fixture = *static_cast<Fixture*>(context);
    ++fixture.acquireCalls;
    if (!fixture.available || fixture.owned) {
        return false;
    }
    fixture.owned = true;
    return true;
}

void release(void* context) noexcept {
    auto& fixture = *static_cast<Fixture*>(context);
    TEST_ASSERT_TRUE(fixture.owned);
    fixture.owned = false;
    ++fixture.releaseCalls;
}

void writeEvidence(const char* text, void* context) noexcept {
    auto& fixture = *static_cast<Fixture*>(context);
    const size_t length = text == nullptr ? 0 : std::strlen(text);
    const size_t available = sizeof(fixture.output) - fixture.outputLength - 1;
    const size_t copied = length < available ? length : available;
    std::memcpy(fixture.output + fixture.outputLength, text, copied);
    fixture.outputLength += copied;
    fixture.output[fixture.outputLength] = '\0';
}

StorageBsc14HilRuntime runtimeFor(Fixture& fixture) {
    return {{tryAcquire, release, &fixture}, writeEvidence, &fixture};
}

void feed(HilFaultRuntimeOwner& owner, const char* command, const uint32_t nowMs) {
    for (const char* cursor = command; *cursor != '\0'; ++cursor) {
        owner.acceptSerialByte(*cursor, nowMs);
    }
    owner.acceptSerialByte('\n', nowMs);
}

void beginAndArm(HilFaultRuntimeOwner& owner, const uint32_t nowMs) {
    char command[224]{};
    std::snprintf(command, sizeof(command), "V1HIL BEGIN BSC-14 %s 60000", kSessionHashText);
    feed(owner, command, nowMs);
    std::snprintf(command, sizeof(command), "V1HIL ARM BSC-14 sd-mutex-hold %s 14", kSessionHashText);
    feed(owner, command, nowMs + 1);
}
} // namespace

void setUp() {}
void tearDown() {}

void test_armed_holder_owns_mutex_records_all_gestures_and_releases_bounded() {
    Fixture fixture;
    HilFaultRuntimeOwner owner;
    StorageBsc14HilFaultModule module(owner, runtimeFor(fixture));
    beginAndArm(owner, 100);
    module.service(102);

    TEST_ASSERT_TRUE(module.holderTaskTick(103));
    TEST_ASSERT_TRUE(fixture.owned);
    TEST_ASSERT_TRUE(module.snapshot().holding);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Fired),
                            static_cast<uint8_t>(module.controllerSnapshot().state));

    module.recordGesturePersisted(StorageBsc14Gesture::SliderExit, 1, 2, 104);
    module.recordGesturePersisted(StorageBsc14Gesture::StealthDoublePress, 2, 3, 105);
    module.recordGesturePersisted(StorageBsc14Gesture::ProfileTripleTap, 3, 4, 106);
    TEST_ASSERT_EQUAL_UINT32(7u, module.snapshot().gestureMask);
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"gesture_kind\":\"slider_exit\""));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"gesture_kind\":\"stealth_double_press\""));
    TEST_ASSERT_NOT_NULL(std::strstr(fixture.output, "\"gesture_kind\":\"profile_triple_tap\""));

    TEST_ASSERT_TRUE(module.holderTaskTick(5103));
    TEST_ASSERT_FALSE(fixture.owned);
    TEST_ASSERT_EQUAL_UINT32(1u, fixture.releaseCalls);
    TEST_ASSERT_TRUE(module.snapshot().completionRecorded);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(HilFaultState::Released),
                            static_cast<uint8_t>(module.controllerSnapshot().state));
}

void test_busy_mutex_acquisition_expires_without_false_ready_or_release() {
    Fixture fixture;
    fixture.available = false;
    HilFaultRuntimeOwner owner;
    StorageBsc14HilFaultModule module(owner, runtimeFor(fixture));
    beginAndArm(owner, 100);
    module.service(102);

    TEST_ASSERT_TRUE(module.holderTaskTick(103));
    TEST_ASSERT_TRUE(module.snapshot().acquiring);
    TEST_ASSERT_TRUE(module.holderTaskTick(1103));
    TEST_ASSERT_FALSE(module.snapshot().holding);
    TEST_ASSERT_FALSE(fixture.owned);
    TEST_ASSERT_EQUAL_UINT32(0u, fixture.releaseCalls);
    TEST_ASSERT_NULL(std::strstr(fixture.output, "\"hil_event\":\"ready\""));
}

void test_session_end_releases_mutex_from_holder_path() {
    Fixture fixture;
    HilFaultRuntimeOwner owner;
    StorageBsc14HilFaultModule module(owner, runtimeFor(fixture));
    beginAndArm(owner, 100);
    module.service(102);
    module.holderTaskTick(103);
    TEST_ASSERT_TRUE(fixture.owned);

    char command[192]{};
    std::snprintf(command, sizeof(command), "V1HIL END BSC-14 %s", kSessionHashText);
    feed(owner, command, 104);
    module.holderTaskTick(105);
    TEST_ASSERT_FALSE(fixture.owned);
    TEST_ASSERT_EQUAL_UINT32(1u, fixture.releaseCalls);
}

void test_unarmed_and_failed_persist_observations_are_neutral() {
    Fixture fixture;
    HilFaultRuntimeOwner owner;
    StorageBsc14HilFaultModule module(owner, runtimeFor(fixture));
    module.service(100);
    module.holderTaskTick(101);
    module.recordGesturePersisted(StorageBsc14Gesture::SliderExit, 1, 1, 102);
    TEST_ASSERT_EQUAL_UINT32(0u, fixture.acquireCalls);
    TEST_ASSERT_EQUAL_UINT32(0u, module.snapshot().gestureMask);
    TEST_ASSERT_EQUAL_STRING("", fixture.output);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_armed_holder_owns_mutex_records_all_gestures_and_releases_bounded);
    RUN_TEST(test_busy_mutex_acquisition_expires_without_false_ready_or_release);
    RUN_TEST(test_session_end_releases_mutex_from_holder_path);
    RUN_TEST(test_unarmed_and_failed_persist_observations_are_neutral);
    return UNITY_END();
}
