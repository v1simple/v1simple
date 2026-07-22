#pragma once

#include <cstdint>

enum class SdMutexHoldState : uint8_t {
    Idle,
    Waiting,
    Holding,
    Finished,
    InvalidRuntime,
};

enum class SdMutexHoldStep : uint8_t {
    Idle,
    Waiting,
    Acquired,
    Holding,
    Released,
    AcquisitionExpired,
    InvalidRuntime,
};

struct SdMutexHoldRuntime {
    using TryAcquire = bool (*)(void*) noexcept;
    using Release = void (*)(void*) noexcept;

    TryAcquire tryAcquire = nullptr;
    Release release = nullptr;
    void* context = nullptr;
};

class SdMutexHoldLifecycle {
  public:
    explicit SdMutexHoldLifecycle(const SdMutexHoldRuntime& runtime = {}) noexcept;

    void configure(const SdMutexHoldRuntime& runtime) noexcept;
    bool begin(uint32_t nowMs, uint32_t acquisitionWindowMs) noexcept;
    SdMutexHoldStep step(uint32_t nowMs, bool continueHolding) noexcept;

    SdMutexHoldState state() const noexcept { return state_; }
    bool ownsMutex() const noexcept { return ownsMutex_; }

  private:
    static bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) noexcept;
    bool runtimeValid() const noexcept;
    SdMutexHoldStep releaseOwned() noexcept;

    SdMutexHoldRuntime runtime_{};
    SdMutexHoldState state_ = SdMutexHoldState::Idle;
    uint32_t acquisitionDeadlineMs_ = 0;
    bool ownsMutex_ = false;
};
