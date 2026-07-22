#pragma once

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include <cstdint>

#include "modules/hil/hil_fault_serial_module.h"
#include "obd_transport_operation_barrier.h"

enum class ObdBsc06Operation : uint8_t {
    None,
    Write,
};

struct ObdBsc06HilAdmission {
    ObdBsc06Operation operation = ObdBsc06Operation::None;
    uint32_t activeGeneration = 0;
    uint32_t requestId = 0;
    uint32_t dispatchEpoch = 0;
    uint8_t runtimeStateCode = 0;
};

struct ObdBsc06HilRuntime {
    using WriteEvidence = void (*)(const char*, void*) noexcept;

    ObdTransportBarrierRuntime barrier{};
    WriteEvidence writeEvidence = nullptr;
    void* evidenceContext = nullptr;
};

struct ObdBsc06HilSnapshot {
    bool armed = false;
    bool barrierActive = false;
    bool completionRecorded = false;
    bool operationSuppressed = false;
    bool controllerReleaseRecorded = false;
    uint32_t armSequence = 0;
    uint32_t readySequence = 0;
    uint32_t activeGeneration = 0;
    uint32_t requestId = 0;
    uint32_t dispatchEpoch = 0;
    uint32_t cancellationEpoch = 0;
};

class ObdBsc06HilFaultModule {
  public:
    static constexpr uint32_t kAutomaticReleaseMs = 1000;
    static constexpr uint16_t kPollingWritePhase = 1;
    static constexpr uint8_t kPollingRuntimeStateCode = 8;

    explicit ObdBsc06HilFaultModule(HilFaultRuntimeOwner& owner, const ObdBsc06HilRuntime& runtime = {}) noexcept;

    void configure(const ObdBsc06HilRuntime& runtime) noexcept;

    // Called by the transport owner after request-epoch claim and immediately
    // before the selected low-level operation. False suppresses that operation.
    bool routeOperation(const ObdBsc06HilAdmission& admission, uint32_t nowMs) noexcept;
    void service(uint32_t nowMs) noexcept;

    HilFaultSnapshot controllerSnapshot() const noexcept;
    ObdBsc06HilSnapshot snapshot() const noexcept;

  private:
    enum class Stage : uint32_t {
        Idle,
        Armed,
        Pausing,
        Paused,
        Completed,
    };

    struct ActiveIdentity {
        HilSessionTokenHash sessionHash{};
        uint32_t armSequence = 0;
        uint32_t readySequence = 0;
        uint32_t generation = 0;
    };

    static bool identitiesEqual(const HilArmedFaultIdentity& armed, const ActiveIdentity& active) noexcept;
    static bool continuePause(uint32_t nowMs, void* context) noexcept;
    static const char* operationName(ObdBsc06Operation operation) noexcept;
    static const char* runtimeStateName(uint8_t stateCode) noexcept;

    void refreshArm() noexcept;
    void emitEvent(const char* event, const char* reason) noexcept;
    void emitPendingEvents() noexcept;

    HilFaultRuntimeOwner& owner_;
    ObdBsc06HilRuntime runtime_{};
    ActiveIdentity identity_{};
    ObdBsc06HilAdmission admission_{};
    HilAtomicUint32 stage_{static_cast<uint32_t>(Stage::Idle)};
    ObdTransportBarrierOutcome outcome_ = ObdTransportBarrierOutcome::InvalidRuntime;
    HilFaultState controllerStateAtCompletion_ = HilFaultState::Disarmed;
    uint32_t readyTimestampMs_ = 0;
    uint32_t completionTimestampMs_ = 0;
    uint32_t cancellationEpochAtCompletion_ = 0;
    uint32_t linkDownGeneration_ = 0;
    bool readyEventPending_ = false;
    bool firedEventPending_ = false;
    bool completionEventPending_ = false;
    bool operationSuppressed_ = false;
    bool controllerReleaseRecorded_ = false;
};

ObdBsc06HilFaultModule& obdBsc06HilFaultModule() noexcept;
void configureObdBsc06HilDeviceRuntime(ObdTransportBarrierRuntime::ReadCancellationEpoch cancellationEpoch,
                                       ObdTransportBarrierRuntime::LinkDownConfirmed linkDownConfirmed,
                                       void* probeContext) noexcept;

#endif // V1SIMPLE_HIL_FAULT_CONTROL
