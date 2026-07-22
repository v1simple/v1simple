#include "sd_mutex_hold_lifecycle.h"

#include <climits>

SdMutexHoldLifecycle::SdMutexHoldLifecycle(const SdMutexHoldRuntime& runtime) noexcept : runtime_(runtime) {}

void SdMutexHoldLifecycle::configure(const SdMutexHoldRuntime& runtime) noexcept {
    if (ownsMutex_) {
        return;
    }
    runtime_ = runtime;
    state_ = SdMutexHoldState::Idle;
    acquisitionDeadlineMs_ = 0;
}

bool SdMutexHoldLifecycle::deadlineReached(const uint32_t nowMs, const uint32_t deadlineMs) noexcept {
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

bool SdMutexHoldLifecycle::runtimeValid() const noexcept {
    return runtime_.tryAcquire != nullptr && runtime_.release != nullptr;
}

bool SdMutexHoldLifecycle::begin(const uint32_t nowMs, const uint32_t acquisitionWindowMs) noexcept {
    if (ownsMutex_ || (state_ != SdMutexHoldState::Idle && state_ != SdMutexHoldState::Finished &&
                       state_ != SdMutexHoldState::InvalidRuntime)) {
        return false;
    }
    if (!runtimeValid() || acquisitionWindowMs == 0u || acquisitionWindowMs > static_cast<uint32_t>(INT32_MAX)) {
        state_ = SdMutexHoldState::InvalidRuntime;
        return false;
    }
    acquisitionDeadlineMs_ = nowMs + acquisitionWindowMs;
    state_ = SdMutexHoldState::Waiting;
    return true;
}

SdMutexHoldStep SdMutexHoldLifecycle::releaseOwned() noexcept {
    if (ownsMutex_) {
        runtime_.release(runtime_.context);
        ownsMutex_ = false;
    }
    state_ = SdMutexHoldState::Finished;
    return SdMutexHoldStep::Released;
}

SdMutexHoldStep SdMutexHoldLifecycle::step(const uint32_t nowMs, const bool continueHolding) noexcept {
    if (!runtimeValid()) {
        state_ = SdMutexHoldState::InvalidRuntime;
        return SdMutexHoldStep::InvalidRuntime;
    }
    if (state_ == SdMutexHoldState::Idle || state_ == SdMutexHoldState::Finished) {
        return SdMutexHoldStep::Idle;
    }
    if (state_ == SdMutexHoldState::InvalidRuntime) {
        return SdMutexHoldStep::InvalidRuntime;
    }
    if (state_ == SdMutexHoldState::Waiting) {
        if (!continueHolding || deadlineReached(nowMs, acquisitionDeadlineMs_)) {
            state_ = SdMutexHoldState::Finished;
            return SdMutexHoldStep::AcquisitionExpired;
        }
        if (!runtime_.tryAcquire(runtime_.context)) {
            return SdMutexHoldStep::Waiting;
        }
        ownsMutex_ = true;
        state_ = SdMutexHoldState::Holding;
        return SdMutexHoldStep::Acquired;
    }
    if (!continueHolding) {
        return releaseOwned();
    }
    return SdMutexHoldStep::Holding;
}
