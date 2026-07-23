#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

enum class BleNotificationDelayCaptureResult : uint8_t {
    Captured,
    Busy,
    Occupied,
    Invalid,
};

struct BleDelayedNotification {
    std::array<uint8_t, 256> data{};
    size_t length = 0;
    uint16_t characteristicUuid = 0;
    uint32_t oldGeneration = 0;
    uint32_t newGeneration = 0;
    uint32_t oldSessionClosedAtMs = 0;
    uint32_t newSessionOpenedAtMs = 0;
};

// Fixed-storage cross-task handoff for one generation-stamped notification.
// The BLE callback makes one non-blocking lease attempt; lifecycle callbacks
// publish generation signals independently so a close/open edge cannot be
// lost while the notification bytes are being copied.
class BleNotificationDelayGate {
  public:
    static constexpr size_t kMaximumNotificationBytes = 256;

    BleNotificationDelayCaptureResult capture(const uint8_t* data, size_t length, uint16_t characteristicUuid,
                                              uint32_t oldGeneration) noexcept;
    void recordSessionClosed(uint32_t generation, uint32_t nowMs) noexcept;
    void recordSessionOpened(uint32_t generation, uint32_t nowMs) noexcept;

    bool claimEligible(BleDelayedNotification& notification) noexcept;
    bool discard() noexcept;
    bool hasCaptured() const noexcept;

#if defined(UNIT_TEST)
    using CaptureLeaseHook = void (*)(void*) noexcept;
    void setCaptureLeaseHook(CaptureLeaseHook hook, void* context) noexcept {
        captureLeaseHook_ = hook;
        captureLeaseContext_ = context;
    }
#endif

  private:
    enum class State : uint32_t {
        Empty,
        Captured,
        Claimed,
    };

    std::atomic<uint32_t> mutationGate_{0};
    std::atomic<uint32_t> state_{static_cast<uint32_t>(State::Empty)};
    std::atomic<uint32_t> closedGeneration_{0};
    std::atomic<uint32_t> closedAtMs_{0};
    std::atomic<uint32_t> closedSequence_{0};
    std::atomic<uint32_t> openedGeneration_{0};
    std::atomic<uint32_t> openedAtMs_{0};
    std::atomic<uint32_t> openedSequence_{0};
    std::atomic<uint32_t> lifecycleSequence_{0};
    BleDelayedNotification notification_{};
#if defined(UNIT_TEST)
    CaptureLeaseHook captureLeaseHook_ = nullptr;
    void* captureLeaseContext_ = nullptr;
#endif
};
