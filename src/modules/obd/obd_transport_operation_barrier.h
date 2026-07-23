#pragma once

#include <cstdint>

enum class ObdTransportBarrierOutcome : uint8_t {
    CancellationEpochAdvanced,
    LinkDownConfirmed,
    PauseEnded,
    DeadlineReached,
    InvalidRuntime,
};

struct ObdTransportBarrierRequest {
    uint32_t dispatchEpoch = 0;
    uint32_t activeGeneration = 0;
    uint32_t startedAtMs = 0;
    uint32_t maximumWaitMs = 0;
};

struct ObdTransportBarrierRuntime {
    using ClockMs = uint32_t (*)(void*) noexcept;
    using ReadCancellationEpoch = uint32_t (*)(void*) noexcept;
    using LinkDownConfirmed = bool (*)(uint32_t, void*) noexcept;
    using YieldTransportOwner = void (*)(void*) noexcept;

    ClockMs clockMs = nullptr;
    ReadCancellationEpoch cancellationEpoch = nullptr;
    LinkDownConfirmed linkDownConfirmed = nullptr;
    YieldTransportOwner yieldTransportOwner = nullptr;
    void* context = nullptr;
};

// Bounded wait used only by the OBD transport owner after it claims a request
// epoch and before it enters the selected low-level operation. Product mutexes
// and callback context never enter this seam.
class ObdTransportOperationBarrier {
  public:
    using ContinuePause = bool (*)(uint32_t, void*) noexcept;

    static bool runtimeValid(const ObdTransportBarrierRuntime& runtime) noexcept;
    static ObdTransportBarrierOutcome wait(const ObdTransportBarrierRequest& request,
                                           const ObdTransportBarrierRuntime& runtime, ContinuePause continuePause,
                                           void* pauseContext) noexcept;
};
