#pragma once

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include <cstdint>

#include "modules/hil/hil_fault_serial_module.h"

struct WifiBsc10HilRuntime {
    using WriteEvidence = void (*)(const char*, void*) noexcept;

    WriteEvidence writeEvidence = nullptr;
    void* context = nullptr;
};

struct WifiBsc10Admission {
    bool persistedEnabled = false;
    uint8_t lifecycleState = 0;
    int selectedSlot = -1;
};

struct WifiBsc10HilSnapshot {
    bool admissionAttempted = false;
    bool rejectionApplied = false;
    uint32_t armSequence = 0;
    uint32_t readySequence = 0;
    uint32_t generation = 0;
};

class WifiBsc10HilFaultModule {
  public:
    static constexpr uint32_t kAutomaticReleaseMs = 1000;
    static constexpr uint16_t kLifecycleAdmissionPhase = 1;

    explicit WifiBsc10HilFaultModule(HilFaultRuntimeOwner& owner, const WifiBsc10HilRuntime& runtime = {}) noexcept;

    void configure(const WifiBsc10HilRuntime& runtime) noexcept;
    bool admitLifecycleStart(const WifiBsc10Admission& admission, uint32_t nowMs) noexcept;
    void service(uint32_t nowMs) noexcept;

    HilFaultSnapshot controllerSnapshot() const noexcept;
    WifiBsc10HilSnapshot snapshot() const noexcept;

  private:
    uint32_t nextGeneration() noexcept;
    void emitEvent(const char* event, const char* reason, bool admitted) noexcept;

    HilFaultRuntimeOwner& owner_;
    WifiBsc10HilRuntime runtime_{};
    WifiBsc10Admission admission_{};
    HilSessionTokenHash sessionHash_{};
    uint32_t armSequence_ = 0;
    uint32_t readySequence_ = 0;
    uint32_t generation_ = 0;
    uint32_t generationCounter_ = 0;
    bool admissionAttempted_ = false;
    bool rejectionApplied_ = false;
};

WifiBsc10HilFaultModule& wifiBsc10HilFaultModule() noexcept;
void configureWifiBsc10HilDeviceRuntime() noexcept;

#endif // V1SIMPLE_HIL_FAULT_CONTROL
