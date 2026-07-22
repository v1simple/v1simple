#pragma once

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include <cstdint>

#include "modules/hil/hil_fault_serial_module.h"
#include "sd_mutex_hold_lifecycle.h"

enum class StorageBsc14Gesture : uint8_t {
    SliderExit = 0,
    StealthDoublePress = 1,
    ProfileTripleTap = 2,
    Invalid = 3,
};

struct StorageBsc14HilRuntime {
    using WriteEvidence = void (*)(const char*, void*) noexcept;

    SdMutexHoldRuntime mutex{};
    WriteEvidence writeEvidence = nullptr;
    void* evidenceContext = nullptr;
};

struct StorageBsc14HilSnapshot {
    bool armed = false;
    bool acquiring = false;
    bool holding = false;
    bool completionRecorded = false;
    bool mutexOwned = false;
    uint32_t armSequence = 0;
    uint32_t readySequence = 0;
    uint32_t generation = 0;
    uint32_t gestureMask = 0;
};

class StorageBsc14HilFaultModule {
  public:
    static constexpr uint32_t kAcquisitionWindowMs = 1000;
    static constexpr uint32_t kAutomaticReleaseMs = 5000;
    static constexpr uint16_t kSdMutexHoldPhase = 1;

    explicit StorageBsc14HilFaultModule(HilFaultRuntimeOwner& owner,
                                        const StorageBsc14HilRuntime& runtime = {}) noexcept;

    void configure(const StorageBsc14HilRuntime& runtime) noexcept;
    void service(uint32_t nowMs) noexcept;
    bool holderTaskTick(uint32_t nowMs) noexcept;
    void recordGesturePersisted(StorageBsc14Gesture gesture, uint32_t previousRevision, uint32_t persistedRevision,
                                uint32_t nowMs) noexcept;

    HilFaultSnapshot controllerSnapshot() const noexcept;
    StorageBsc14HilSnapshot snapshot() const noexcept;

  private:
    enum class Stage : uint32_t {
        Idle,
        Armed,
        Acquiring,
        Holding,
        Completed,
    };

    struct ActiveIdentity {
        HilSessionTokenHash sessionHash{};
        uint32_t armSequence = 0;
        uint32_t readySequence = 0;
        uint32_t generation = 0;
    };

    static bool identitiesEqual(const HilArmedFaultIdentity& armed, const ActiveIdentity& active) noexcept;
    static const char* gestureName(StorageBsc14Gesture gesture) noexcept;
    uint32_t nextGeneration() noexcept;
    void refreshArm() noexcept;
    void emitEvent(const char* event, const char* reason, StorageBsc14Gesture gesture = StorageBsc14Gesture::Invalid,
                   uint32_t previousRevision = 0, uint32_t persistedRevision = 0) noexcept;
    void completeHold(uint32_t nowMs, const char* reason) noexcept;

    HilFaultRuntimeOwner& owner_;
    StorageBsc14HilRuntime runtime_{};
    SdMutexHoldLifecycle lifecycle_{};
    ActiveIdentity identity_{};
    HilAtomicUint32 stage_{static_cast<uint32_t>(Stage::Idle)};
    HilAtomicUint32 gestureMask_{0};
    uint32_t generationCounter_ = 0;
    HilAtomicUint32 completionRecorded_{0};
};

StorageBsc14HilFaultModule& storageBsc14HilFaultModule() noexcept;
bool configureStorageBsc14HilDeviceRuntime(void* sdMutex) noexcept;

#endif // V1SIMPLE_HIL_FAULT_CONTROL
