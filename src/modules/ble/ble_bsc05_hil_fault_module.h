#pragma once

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include <cstddef>
#include <cstdint>

#include "ble_notification_delay_gate.h"
#include "modules/hil/hil_fault_serial_module.h"

struct BleBsc05HilRuntime {
    using ForwardNotification = bool (*)(const uint8_t*, size_t, uint16_t, uint32_t, void*) noexcept;
    using WriteEvidence = void (*)(const char*, void*) noexcept;

    ForwardNotification forwardNotification = nullptr;
    void* forwardContext = nullptr;
    WriteEvidence writeEvidence = nullptr;
    void* evidenceContext = nullptr;
};

struct BleBsc05HilSnapshot {
    bool armed = false;
    bool notificationCaptured = false;
    bool oldSessionClosed = false;
    bool newerSessionOpened = false;
    bool releaseAttempted = false;
    bool wrongGenerationRejected = false;
    uint32_t armSequence = 0;
    uint32_t readySequence = 0;
    uint32_t oldGeneration = 0;
    uint32_t newGeneration = 0;
};

class BleBsc05HilFaultModule {
  public:
    static constexpr uint32_t kAutomaticReleaseMs = HilFaultController::kMaximumAutomaticReleaseMs;
    static constexpr uint16_t kNotificationAdmissionPhase = 1;

    explicit BleBsc05HilFaultModule(HilFaultRuntimeOwner& owner, const BleBsc05HilRuntime& runtime = {}) noexcept;

    void configure(const BleBsc05HilRuntime& runtime) noexcept;

    // Returns true when the caller must forward the notification immediately.
    // A false result means one armed notification was copied into fixed storage.
    bool routeNotification(const uint8_t* data, size_t length, uint16_t characteristicUuid, uint32_t sessionGeneration,
                           uint32_t nowMs) noexcept;
    void recordSessionClosed(uint32_t generation, uint32_t nowMs) noexcept;
    void recordSessionOpened(uint32_t generation, uint32_t nowMs) noexcept;
    void service(uint32_t nowMs) noexcept;

    HilFaultSnapshot controllerSnapshot() const noexcept;
    BleBsc05HilSnapshot snapshot() const noexcept;

  private:
    enum class Stage : uint32_t {
        Idle,
        Armed,
        Capturing,
        Captured,
        Completed,
    };

    struct ActiveIdentity {
        HilSessionTokenHash sessionHash{};
        uint32_t armSequence = 0;
        uint32_t readySequence = 0;
        uint32_t oldGeneration = 0;
    };

    static bool identitiesEqual(const HilArmedFaultIdentity& armed, const ActiveIdentity& active) noexcept;
    static const char* characteristicClass(uint16_t characteristicUuid) noexcept;
    void refreshArm() noexcept;
    void emitEvent(const char* event, const char* reason) noexcept;
    void emitCapturedEvents() noexcept;
    void completeExpired(const char* reason) noexcept;

    HilFaultRuntimeOwner& owner_;
    BleBsc05HilRuntime runtime_{};
    BleNotificationDelayGate delayGate_{};
    ActiveIdentity identity_{};
    BleDelayedNotification releasedNotification_{};
    HilAtomicUint32 stage_{static_cast<uint32_t>(Stage::Idle)};
    bool readyEventPending_ = false;
    bool firedEventPending_ = false;
    bool releaseAttempted_ = false;
    bool wrongGenerationRejected_ = false;
};

BleBsc05HilFaultModule& bleBsc05HilFaultModule() noexcept;
void configureBleBsc05HilDeviceRuntime(BleBsc05HilRuntime::ForwardNotification forwardNotification,
                                       void* forwardContext) noexcept;

#endif // V1SIMPLE_HIL_FAULT_CONTROL
