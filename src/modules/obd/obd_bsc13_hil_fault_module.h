#pragma once

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include <cstdint>

#include "modules/hil/hil_fault_serial_module.h"
#include "obd_physical_link_preownership_barrier.h"

struct ObdBsc13HilAdmission {
    bool physicalLinkConnected = false;
    uint32_t activeGeneration = 0;
    uint8_t runtimeStateCode = 0;
};

struct ObdBsc13HilRuntime {
    using WriteEvidence = void (*)(const char*, void*) noexcept;

    ObdPhysicalLinkPreownershipRuntime barrier{};
    WriteEvidence writeEvidence = nullptr;
    void* evidenceContext = nullptr;
};

struct ObdBsc13HilSnapshot {
    bool armed = false;
    bool barrierActive = false;
    bool completionRecorded = false;
    bool cancellationObserved = false;
    bool linkDownConfirmed = false;
    bool controllerReleaseRecorded = false;
    uint32_t armSequence = 0;
    uint32_t readySequence = 0;
    uint32_t activeGeneration = 0;
    uint32_t dispatchEpoch = 0;
    uint32_t cancellationEpoch = 0;
};

class ObdBsc13HilFaultModule {
  public:
    static constexpr uint32_t kAutomaticReleaseMs = 1000;
    static constexpr uint16_t kPhysicalLinkPreownershipPhase = 1;
    static constexpr uint8_t kConnectingRuntimeStateCode = 4;

    explicit ObdBsc13HilFaultModule(HilFaultRuntimeOwner& owner, const ObdBsc13HilRuntime& runtime = {}) noexcept;

    void configure(const ObdBsc13HilRuntime& runtime) noexcept;

    // Called by the main-loop state-machine owner after physical connection is
    // observed and immediately before CONNECTING adopts the link. False holds
    // only session adoption; callbacks and the transport owner never wait.
    bool admitSessionOwnership(const ObdBsc13HilAdmission& admission, uint32_t nowMs) noexcept;
    void service(uint32_t nowMs) noexcept;

    HilFaultSnapshot controllerSnapshot() const noexcept;
    ObdBsc13HilSnapshot snapshot() const noexcept;

  private:
    enum class Stage : uint8_t {
        Idle,
        Armed,
        Active,
        Completed,
    };

    struct ActiveIdentity {
        HilSessionTokenHash sessionHash{};
        uint32_t armSequence = 0;
        uint32_t readySequence = 0;
        uint32_t generation = 0;
    };

    static bool identitiesEqual(const HilArmedFaultIdentity& armed, const ActiveIdentity& active) noexcept;
    static const char* runtimeStateName(uint8_t stateCode) noexcept;
    void refreshArm() noexcept;
    void evaluateBarrier(uint32_t nowMs) noexcept;
    void completeExpired(const char* reason, uint32_t nowMs) noexcept;
    void emitEvent(const char* event, const char* reason) noexcept;

    HilFaultRuntimeOwner& owner_;
    ObdBsc13HilRuntime runtime_{};
    ActiveIdentity identity_{};
    ObdBsc13HilAdmission admission_{};
    ObdPhysicalLinkPreownershipRequest request_{};
    Stage stage_ = Stage::Idle;
    uint32_t readyTimestampMs_ = 0;
    uint32_t completionTimestampMs_ = 0;
    uint32_t cancellationEpoch_ = 0;
    bool cancellationObserved_ = false;
    bool linkDownConfirmed_ = false;
    bool controllerReleaseRecorded_ = false;
    bool sessionOwnershipPermitted_ = false;
};

ObdBsc13HilFaultModule& obdBsc13HilFaultModule() noexcept;
void configureObdBsc13HilDeviceRuntime(ObdPhysicalLinkPreownershipRuntime::ReadCancellationEpoch cancellationEpoch,
                                       ObdPhysicalLinkPreownershipRuntime::LinkDownConfirmed linkDownConfirmed,
                                       void* probeContext) noexcept;

#endif // V1SIMPLE_HIL_FAULT_CONTROL
