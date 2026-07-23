#include "obd_transport_operation_barrier.h"

bool ObdTransportOperationBarrier::runtimeValid(const ObdTransportBarrierRuntime& runtime) noexcept {
    return runtime.clockMs != nullptr && runtime.cancellationEpoch != nullptr && runtime.linkDownConfirmed != nullptr &&
           runtime.yieldTransportOwner != nullptr;
}

ObdTransportBarrierOutcome ObdTransportOperationBarrier::wait(const ObdTransportBarrierRequest& request,
                                                              const ObdTransportBarrierRuntime& runtime,
                                                              const ContinuePause continuePause,
                                                              void* const pauseContext) noexcept {
    if (!runtimeValid(runtime) || continuePause == nullptr || request.activeGeneration == 0 ||
        request.maximumWaitMs == 0) {
        return ObdTransportBarrierOutcome::InvalidRuntime;
    }

    while (true) {
        if (runtime.cancellationEpoch(runtime.context) != request.dispatchEpoch) {
            return ObdTransportBarrierOutcome::CancellationEpochAdvanced;
        }
        if (runtime.linkDownConfirmed(request.activeGeneration, runtime.context)) {
            return ObdTransportBarrierOutcome::LinkDownConfirmed;
        }

        const uint32_t nowMs = runtime.clockMs(runtime.context);
        if (static_cast<uint32_t>(nowMs - request.startedAtMs) >= request.maximumWaitMs) {
            return ObdTransportBarrierOutcome::DeadlineReached;
        }
        if (!continuePause(nowMs, pauseContext)) {
            return ObdTransportBarrierOutcome::PauseEnded;
        }
        runtime.yieldTransportOwner(runtime.context);
    }
}
