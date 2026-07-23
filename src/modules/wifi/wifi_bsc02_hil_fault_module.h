#pragma once

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/hil/hil_next_boot_fault.h"

struct WifiBsc02HilRuntime {
    using HeapMetric = uint32_t (*)(void*) noexcept;
    using AllocateInternal = void* (*)(size_t, void*) noexcept;
    using ReleaseInternal = void (*)(void*, void*) noexcept;
    using StartPressureTask = bool (*)(void*) noexcept;
    using WriteEvidence = void (*)(const char*, void*) noexcept;
    using PersistentClockMs = uint64_t (*)(void*) noexcept;

    HeapMetric freeInternal = nullptr;
    HeapMetric largestInternal = nullptr;
    AllocateInternal allocateInternal = nullptr;
    ReleaseInternal releaseInternal = nullptr;
    StartPressureTask startPressureTask = nullptr;
    WriteEvidence writeEvidence = nullptr;
    PersistentClockMs persistentClockMs = nullptr;
    void* context = nullptr;
};

struct WifiBsc02HilPressurePlannerParameters {
    uint32_t triggerFreeBytes = 0;
    uint32_t triggerLargestBlockBytes = 0;
    uint32_t safetyFreeBytes = 0;
    uint32_t safetyLargestBlockBytes = 0;
    uint32_t absoluteMinimumFreeBytes = 0;
    uint32_t absoluteMinimumLargestBlockBytes = 0;
    uint32_t allocationCapBytes = 0;
    uint32_t chunkBytes = 0;
    uint32_t allocationReserveBytes = 0;
};

struct WifiBsc02HilHeapMetrics {
    uint32_t freeBytes = 0;
    uint32_t largestBlockBytes = 0;
};

enum class WifiBsc02HilPressureDecision : uint8_t {
    Allocate,
    TriggerReached,
    SafetyBreach,
    CapReached,
    InvalidParameters,
};

using WifiBsc02HilNextBootRecord = HilNextBootFaultRecord;

struct WifiBsc02HilPressureSnapshot {
    uint32_t freeInternalBefore = 0;
    uint32_t freeInternalAfter = 0;
    uint32_t largestInternalBefore = 0;
    uint32_t largestInternalAfter = 0;
    uint32_t allocatedBytes = 0;
    uint32_t taskOverheadBytes = 0;
    uint32_t triggerFreeBytes = 0;
    uint32_t triggerLargestBlockBytes = 0;
    uint32_t safetyFloorBytes = 0;
    uint32_t largestBlockSafetyFloorBytes = 0;
    uint32_t minimumFreeBytes = 0;
    uint32_t minimumLargestBlockBytes = 0;
    uint32_t readySequence = 0;
    bool taskActive = false;
    bool triggerReached = false;
    bool safetyBreach = false;
    bool heapStopPending = false;
    bool competingOperationObserved = false;
};

class WifiBsc02HilFaultModule {
  public:
    static constexpr uint32_t kNextBootMagic = 0x42303248u;
    static constexpr uint16_t kNextBootSchemaVersion = HilNextBootFaultStore::kSchemaVersion;
    static constexpr uint32_t kApAutomaticReleaseMs = 1000;
    static constexpr uint32_t kPressureAutomaticReleaseMs = 5000;
    static constexpr size_t kPressureChunkBytes = 1024;
    static constexpr size_t kPressureAllocationReserveBytes = 256;
    static constexpr size_t kMaximumPressureBytes = 64 * 1024;
    static constexpr uint32_t kMaximumPressureTaskOverheadBytes = 8 * 1024;
    static constexpr size_t kMaximumPressureChunks = kMaximumPressureBytes / kPressureChunkBytes;
    static constexpr uint32_t kAbsoluteMinimumFreeBytes = 14 * 1024;
    static constexpr uint32_t kAbsoluteMinimumLargestBlockBytes = 6 * 1024;
    static constexpr uint32_t kPressureSafetyFreeBytes = 14 * 1024 + 512;
    static constexpr uint32_t kPressureSafetyLargestBlockBytes = 6 * 1024 + 512;
    static constexpr uint16_t kApAdmissionPhase = 1;
    static constexpr uint16_t kSramPressurePhase = 2;

    WifiBsc02HilFaultModule(HilFaultRuntimeOwner& owner, WifiBsc02HilNextBootRecord& nextBootRecord,
                            uint32_t currentBootSequence, const WifiBsc02HilRuntime& runtime = {}) noexcept;

    void configure(const WifiBsc02HilRuntime& runtime) noexcept;
    void configurePressurePlanner(const WifiBsc02HilPressurePlannerParameters& parameters) noexcept;
    bool stageNextBoot(const HilArmedFaultIdentity& identity, uint32_t sessionDeadlineMs, uint32_t stagedAtMs) noexcept;
    void clearNextBoot() noexcept;
    HilFaultResult restoreNextBoot(bool maintenanceActive, uint32_t nowMs) noexcept;
    bool shouldSuppressFreshApStart(bool maintenanceActive, bool freshSetupAdmission, bool serviceReachable,
                                    uint32_t nowMs) noexcept;
    bool finalizeSuppressedApStart(bool interfaceActive, bool serviceActive, bool serviceReachable,
                                   uint32_t nowMs) noexcept;
    void serviceSramPressure(bool maintenanceActive, bool serviceReachable, uint32_t nowMs) noexcept;
    bool observeHeapGuardStop(uint32_t nowMs, uint32_t lowHeapSinceMs, uint32_t minimumPersistMs) noexcept;
    bool pressureTaskTick(uint32_t nowMs) noexcept;
    void service(uint32_t nowMs) noexcept;

    HilFaultSnapshot controllerSnapshot(HilFaultId faultId) const noexcept;
    WifiBsc02HilPressureSnapshot pressureSnapshot() const noexcept;
    static WifiBsc02HilPressureDecision planPressureStep(const WifiBsc02HilPressurePlannerParameters& parameters,
                                                         const WifiBsc02HilHeapMetrics& metrics,
                                                         uint32_t allocatedBytes) noexcept;

  private:
    enum class PressureAbortReason : uint32_t {
        None = 0,
        BaselineUnsafe,
        FireFailed,
        SafetyFloorBreach,
        PlannerAbort,
        PostAllocationBreach,
    };

    struct ActiveIdentity {
        HilSessionTokenHash sessionHash{};
        uint32_t armSequence = 0;
        uint32_t readySequence = 0;
        uint32_t generation = 0;
        uint16_t phase = 0;
    };

    uint32_t nextGeneration() noexcept;
    void attemptApLifecycle(uint32_t nowMs) noexcept;
    void attemptPendingHeapStopObservation(uint32_t nowMs) noexcept;
    WifiBsc02HilHeapMetrics samplePressureMetrics() noexcept;
    bool pressureSafetyBreached(const WifiBsc02HilHeapMetrics& metrics) const noexcept;
    void requestPressureAbort(PressureAbortReason reason) noexcept;
    void consumePendingPressureAbort() noexcept;
    static const char* pressureAbortReasonName(PressureAbortReason reason) noexcept;
    void emitEvent(HilFaultId faultId, const char* event, const char* reason, const ActiveIdentity& identity) noexcept;
    void releasePressureAllocations() noexcept;

    HilFaultRuntimeOwner& owner_;
    HilNextBootFaultStore nextBootStore_;
    WifiBsc02HilRuntime runtime_{};
    WifiBsc02HilPressurePlannerParameters pressurePlanner_{};
    ActiveIdentity apIdentity_{};
    ActiveIdentity pressureIdentity_{};
    std::array<void*, kMaximumPressureChunks> pressureAllocations_{};
    HilAtomicUint32 apFirePending_{0};
    HilAtomicUint32 apCleanupPending_{0};
    HilAtomicUint32 apFired_{0};
    HilAtomicUint32 apCompetitionObserved_{0};
    HilAtomicUint32 apTerminalEmitted_{0};
    HilAtomicUint32 apAttemptGate_{0};
    HilAtomicUint32 pressureBaselineCaptured_{0};
    HilAtomicUint32 pressureFired_{0};
    HilAtomicUint32 pressureAllocationFinished_{0};
    HilAtomicUint32 pressureTriggerReached_{0};
    HilAtomicUint32 pressureSafetyBreach_{0};
    HilAtomicUint32 pressureAbortReason_{0};
    HilAtomicUint32 pressureHeapStopPending_{0};
    HilAtomicUint32 pressureHeapStopTimestampMs_{0};
    HilAtomicUint32 pressureCompetingObserved_{0};
    HilAtomicUint32 pressureTaskActive_{0};
    HilAtomicUint32 pressureTerminalEmitted_{0};
    uint32_t pressureAllocationCount_ = 0;
    HilAtomicUint32 pressureAllocatedBytes_{0};
    HilAtomicUint32 pressurePreTaskFree_{0};
    HilAtomicUint32 pressureTaskOverheadBytes_{0};
    HilAtomicUint32 pressureFreeBefore_{0};
    HilAtomicUint32 pressureFreeAfter_{0};
    HilAtomicUint32 pressureLargestBefore_{0};
    HilAtomicUint32 pressureLargestAfter_{0};
    HilAtomicUint32 pressureMinimumFree_{0};
    HilAtomicUint32 pressureMinimumLargest_{0};
    uint32_t generationCounter_ = 0;
};

WifiBsc02HilFaultModule& wifiBsc02HilFaultModule() noexcept;
void configureWifiBsc02HilDeviceRuntime(uint32_t triggerFreeBytes, uint32_t triggerLargestBlockBytes) noexcept;

#endif
