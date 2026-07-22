#pragma once

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include <cstdint>

#include "modules/hil/hil_fault_serial_module.h"

struct ConnectionBsc04HilRuntime {
    using WriteEvidence = void (*)(const char*, void*) noexcept;

    WriteEvidence writeEvidence = nullptr;
    void* context = nullptr;
};

struct ConnectionBsc04VerifyPushAdmission {
    bool verifyPushMatchEdge = false;
    bool v1GattConnected = false;
    uint8_t coordinatorStateCode = 0;
};

struct ConnectionBsc04HilSnapshot {
    bool admissionAttempted = false;
    bool suppressionApplied = false;
    bool v1GattConnected = false;
    uint8_t coordinatorStateCode = 0;
    uint32_t armSequence = 0;
    uint32_t readySequence = 0;
    uint32_t generation = 0;
};

class ConnectionBsc04HilFaultModule {
  public:
    static constexpr uint32_t kAutomaticReleaseMs = 1000;
    static constexpr uint16_t kVerifyPushAdmissionPhase = 1;
    static constexpr uint8_t kV1SettlingStateCode = 1;

    explicit ConnectionBsc04HilFaultModule(HilFaultRuntimeOwner& owner,
                                           const ConnectionBsc04HilRuntime& runtime = {}) noexcept;

    void configure(const ConnectionBsc04HilRuntime& runtime) noexcept;
    bool routeVerifyPushMatchEdge(const ConnectionBsc04VerifyPushAdmission& admission, uint32_t nowMs) noexcept;
    void service(uint32_t nowMs) noexcept;

    HilFaultSnapshot controllerSnapshot() const noexcept;
    ConnectionBsc04HilSnapshot snapshot() const noexcept;

  private:
    struct ActiveIdentity {
        HilSessionTokenHash sessionHash{};
        uint32_t armSequence = 0;
        uint32_t readySequence = 0;
        uint32_t generation = 0;
    };

    uint32_t nextGeneration() noexcept;
    void emitEvent(const char* event, const char* reason, bool forwarded) noexcept;

    HilFaultRuntimeOwner& owner_;
    ConnectionBsc04HilRuntime runtime_{};
    ConnectionBsc04VerifyPushAdmission admission_{};
    ActiveIdentity identity_{};
    uint32_t generationCounter_ = 0;
    bool admissionAttempted_ = false;
    bool suppressionApplied_ = false;
};

ConnectionBsc04HilFaultModule& connectionBsc04HilFaultModule() noexcept;
void configureConnectionBsc04HilDeviceRuntime() noexcept;

#endif // V1SIMPLE_HIL_FAULT_CONTROL
