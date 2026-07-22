#include <unity.h>

#include <array>
#include <atomic>
#include <thread>

#ifndef V1_LINKED_TEST_BLE_NOTIFICATION_DELAY_GATE
#define V1_LINKED_TEST_BLE_NOTIFICATION_DELAY_GATE
#define V1_INLINE_TEST_BLE_NOTIFICATION_DELAY_GATE
#endif
#include "../../src/modules/ble/ble_notification_delay_gate.h"
#if defined(V1_INLINE_TEST_BLE_NOTIFICATION_DELAY_GATE)
#include "../../src/modules/ble/ble_notification_delay_gate.cpp"
#endif

void setUp() {}
void tearDown() {}

void test_release_requires_matching_close_and_newer_open() {
    BleNotificationDelayGate gate;
    const std::array<uint8_t, 4> bytes{1, 2, 3, 4};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(BleNotificationDelayCaptureResult::Captured),
                            static_cast<uint8_t>(gate.capture(bytes.data(), bytes.size(), 0xB2CE, 7)));

    BleDelayedNotification delayed{};
    TEST_ASSERT_FALSE(gate.claimEligible(delayed));
    gate.recordSessionOpened(8, 120);
    TEST_ASSERT_FALSE(gate.claimEligible(delayed));
    gate.recordSessionClosed(6, 110);
    TEST_ASSERT_FALSE(gate.claimEligible(delayed));
    gate.recordSessionClosed(7, 111);
    TEST_ASSERT_FALSE(gate.claimEligible(delayed));
    gate.recordSessionOpened(8, 121);
    TEST_ASSERT_TRUE(gate.claimEligible(delayed));
    TEST_ASSERT_EQUAL_UINT32(7, delayed.oldGeneration);
    TEST_ASSERT_EQUAL_UINT32(8, delayed.newGeneration);
    TEST_ASSERT_EQUAL_UINT32(111, delayed.oldSessionClosedAtMs);
    TEST_ASSERT_EQUAL_UINT32(121, delayed.newSessionOpenedAtMs);
    TEST_ASSERT_EQUAL_UINT16(0xB2CE, delayed.characteristicUuid);
    TEST_ASSERT_EQUAL_UINT32(bytes.size(), delayed.length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(bytes.data(), delayed.data.data(), bytes.size());
    TEST_ASSERT_FALSE(gate.claimEligible(delayed));
}

void test_same_generation_open_never_releases_old_copy() {
    BleNotificationDelayGate gate;
    const uint8_t byte = 9;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(BleNotificationDelayCaptureResult::Captured),
                            static_cast<uint8_t>(gate.capture(&byte, 1, 0xB4E0, 12)));
    gate.recordSessionClosed(12, 200);
    gate.recordSessionOpened(12, 201);
    BleDelayedNotification delayed{};
    TEST_ASSERT_FALSE(gate.claimEligible(delayed));
    TEST_ASSERT_TRUE(gate.hasCaptured());
}

void test_fixed_storage_rejects_invalid_and_second_capture() {
    BleNotificationDelayGate gate;
    std::array<uint8_t, BleNotificationDelayGate::kMaximumNotificationBytes + 1> oversized{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(BleNotificationDelayCaptureResult::Invalid),
                            static_cast<uint8_t>(gate.capture(nullptr, 1, 0xB2CE, 1)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(BleNotificationDelayCaptureResult::Invalid),
                            static_cast<uint8_t>(gate.capture(oversized.data(), oversized.size(), 0xB2CE, 1)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(BleNotificationDelayCaptureResult::Captured),
                            static_cast<uint8_t>(gate.capture(oversized.data(), 1, 0xB2CE, 1)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(BleNotificationDelayCaptureResult::Occupied),
                            static_cast<uint8_t>(gate.capture(oversized.data(), 1, 0xB2CE, 1)));
    TEST_ASSERT_TRUE(gate.discard());
    TEST_ASSERT_FALSE(gate.hasCaptured());
}

namespace {
struct CaptureRace {
    std::atomic<bool> leaseEntered{false};
    std::atomic<bool> releaseLease{false};
};

void holdCaptureLease(void* context) noexcept {
    auto& race = *static_cast<CaptureRace*>(context);
    race.leaseEntered.store(true, std::memory_order_release);
    while (!race.releaseLease.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}
} // namespace

void test_lifecycle_signals_survive_concurrent_callback_copy() {
    BleNotificationDelayGate gate;
    CaptureRace race;
    gate.setCaptureLeaseHook(holdCaptureLease, &race);
    const std::array<uint8_t, 3> bytes{4, 5, 6};
    BleNotificationDelayCaptureResult captureResult = BleNotificationDelayCaptureResult::Invalid;

    std::thread callback([&]() { captureResult = gate.capture(bytes.data(), bytes.size(), 0xB2CE, 20); });
    while (!race.leaseEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Lifecycle publication never takes the copy lease, so these callback/main
    // edges cannot be lost or forced to wait behind the byte copy.
    gate.recordSessionClosed(20, 300);
    gate.recordSessionOpened(21, 301);
    race.releaseLease.store(true, std::memory_order_release);
    callback.join();

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(BleNotificationDelayCaptureResult::Captured),
                            static_cast<uint8_t>(captureResult));
    BleDelayedNotification delayed{};
    TEST_ASSERT_TRUE(gate.claimEligible(delayed));
    TEST_ASSERT_EQUAL_UINT32(20, delayed.oldGeneration);
    TEST_ASSERT_EQUAL_UINT32(21, delayed.newGeneration);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_release_requires_matching_close_and_newer_open);
    RUN_TEST(test_same_generation_open_never_releases_old_copy);
    RUN_TEST(test_fixed_storage_rejects_invalid_and_second_capture);
    RUN_TEST(test_lifecycle_signals_survive_concurrent_callback_copy);
    return UNITY_END();
}
