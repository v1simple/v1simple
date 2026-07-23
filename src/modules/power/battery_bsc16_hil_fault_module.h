#pragma once

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include <cstdint>

#include "battery_source_policy.h"
#include "modules/hil/hil_next_boot_fault.h"

struct BatteryBsc16HilRuntime {
    using WriteEvidence = void (*)(const char*, void*) noexcept;
    using PersistentClockMs = uint64_t (*)(void*) noexcept;

    WriteEvidence writeEvidence = nullptr;
    PersistentClockMs persistentClockMs = nullptr;
    void* context = nullptr;
};

struct BatteryBsc16HilAdcAdmission {
    bool latchInitialized = false;
    battery_source_policy::Source sourceClassification = battery_source_policy::Source::Unknown;
    bool powerButtonWillBeEnabled = false;
};

struct BatteryBsc16HilSnapshot {
    bool admissionAttempted = false;
    bool suppressionPending = false;
    bool admissionSuppressed = false;
    bool latchInitialized = false;
    bool powerButtonWillBeEnabled = false;
    battery_source_policy::Source sourceClassification = battery_source_policy::Source::Unknown;
    uint32_t armSequence = 0;
    uint32_t readySequence = 0;
    uint32_t generation = 0;
};

class BatteryBsc16HilFaultModule {
  public:
    static constexpr uint32_t kNextBootMagic = 0x42313648u;
    static constexpr uint16_t kNextBootSchemaVersion = HilNextBootFaultStore::kSchemaVersion;
    static constexpr uint32_t kAutomaticReleaseMs = 1000;
    static constexpr uint16_t kAdcAdmissionPhase = 1;

    BatteryBsc16HilFaultModule(HilFaultRuntimeOwner& owner, HilNextBootFaultRecord& nextBootRecord,
                               uint32_t currentBootSequence, const BatteryBsc16HilRuntime& runtime = {}) noexcept;

    void configure(const BatteryBsc16HilRuntime& runtime) noexcept;
    bool stageNextBoot(const HilArmedFaultIdentity& identity, uint32_t sessionDeadlineMs, uint32_t stagedAtMs) noexcept;
    void clearNextBoot() noexcept;
    HilFaultResult restoreNextBoot(uint32_t nowMs) noexcept;

    bool beginAdcAdmission(const BatteryBsc16HilAdcAdmission& admission, uint32_t nowMs) noexcept;
    void completeAdcAdmissionSuppression(uint32_t nowMs) noexcept;
    void service(uint32_t nowMs) noexcept;

    HilFaultSnapshot controllerSnapshot() const noexcept;
    BatteryBsc16HilSnapshot snapshot() const noexcept;

  private:
    struct ActiveIdentity {
        HilSessionTokenHash sessionHash{};
        uint32_t armSequence = 0;
        uint32_t readySequence = 0;
        uint32_t generation = 0;
    };

    uint32_t nextGeneration() noexcept;
    void emitEvent(const char* event, const char* reason) noexcept;

    HilFaultRuntimeOwner& owner_;
    HilNextBootFaultStore nextBootStore_;
    BatteryBsc16HilRuntime runtime_{};
    BatteryBsc16HilAdcAdmission admission_{};
    ActiveIdentity identity_{};
    uint32_t generationCounter_ = 0;
    bool admissionAttempted_ = false;
    bool suppressionPending_ = false;
    bool admissionSuppressed_ = false;
};

BatteryBsc16HilFaultModule& batteryBsc16HilFaultModule() noexcept;
void configureBatteryBsc16HilDeviceRuntime() noexcept;

#endif // V1SIMPLE_HIL_FAULT_CONTROL
