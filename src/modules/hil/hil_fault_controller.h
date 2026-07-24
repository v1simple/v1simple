#pragma once

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(ARDUINO_ARCH_ESP32)
#if !defined(__XTENSA__)
#error "HIL fault controls require the validated ESP32-S3 Xtensa atomic primitive"
#endif
#include <xt_utils.h>
#else
#include <atomic>
#endif

class HilAtomicUint32 {
  public:
    HilAtomicUint32() noexcept = default;
    explicit HilAtomicUint32(uint32_t value) noexcept { store(value); }

    uint32_t load() const noexcept {
#if defined(ARDUINO_ARCH_ESP32)
        memoryBarrier();
        const uint32_t result = value_;
        memoryBarrier();
        return result;
#else
        return value_.load(std::memory_order_seq_cst);
#endif
    }

    void store(uint32_t value) noexcept {
#if defined(ARDUINO_ARCH_ESP32)
        memoryBarrier();
        value_ = value;
        memoryBarrier();
#else
        value_.store(value, std::memory_order_seq_cst);
#endif
    }

    uint32_t fetchAdd(uint32_t increment) noexcept {
        uint32_t expected = load();
        while (!compareExchange(expected, expected + increment)) {
        }
        return expected;
    }

    bool compareExchange(uint32_t& expected, uint32_t desired) noexcept {
#if defined(ARDUINO_ARCH_ESP32)
        memoryBarrier();
        if (xt_utils_compare_and_set(&value_, expected, desired)) {
            memoryBarrier();
            return true;
        }
        expected = load();
        return false;
#else
        return value_.compare_exchange_strong(expected, desired, std::memory_order_seq_cst);
#endif
    }

  private:
#if defined(ARDUINO_ARCH_ESP32)
    static void memoryBarrier() noexcept { __asm__ __volatile__("memw" ::: "memory"); }

    alignas(uint32_t) volatile uint32_t value_ = 0;
#else
    std::atomic<uint32_t> value_{0};
#endif
};

class HilAtomicLease {
  public:
    explicit HilAtomicLease(HilAtomicUint32& gate) noexcept : gate_(gate) {
        uint32_t expected = 0;
        acquired_ = gate_.compareExchange(expected, 1);
    }

    ~HilAtomicLease() {
        if (acquired_) {
            gate_.store(0);
        }
    }

    HilAtomicLease(const HilAtomicLease&) = delete;
    HilAtomicLease& operator=(const HilAtomicLease&) = delete;

    bool acquired() const noexcept { return acquired_; }

  private:
    HilAtomicUint32& gate_;
    bool acquired_ = false;
};

enum class HilCaseId : uint8_t {
    Bsc02,
    Bsc04,
    Bsc05,
    Bsc06,
    Bsc10,
    Bsc13,
    Bsc14,
    Bsc16,
    Invalid,
};

enum class HilFaultId : uint8_t {
    WifiApStartFailOnce,
    WifiInternalSramHold,
    V1VerifyPushSuppressOnce,
    V1NotificationDelayOnce,
    ObdTransportOperationBarrierOnce,
    WifiEnableAdmissionFailOnce,
    ObdPhysicalLinkPreownershipBarrierOnce,
    SdMutexHold,
    BatteryAdcInitFailOnce,
    Invalid,
};

enum class HilFaultState : uint8_t {
    Disarmed,
    Armed,
    Ready,
    Fired,
    Released,
    Expired,
};

enum class HilFaultResult : uint8_t {
    Ok,
    NoSession,
    SessionActive,
    WrongCase,
    WrongFault,
    WrongSession,
    InvalidSessionHash,
    WrongState,
    WrongArmSequence,
    WrongReadySequence,
    WrongGeneration,
    WrongPhase,
    CompetingOperationMissing,
    InvalidDeadline,
    DuplicateFire,
    Expired,
    Busy,
    SessionHashRegistryFull,
};

struct HilSessionTokenHash {
    std::array<uint8_t, 32> bytes{};

    bool operator==(const HilSessionTokenHash& other) const noexcept {
        uint8_t difference = 0;
        for (size_t index = 0; index < bytes.size(); ++index) {
            difference |= static_cast<uint8_t>(bytes[index] ^ other.bytes[index]);
        }
        return difference == 0;
    }
};

struct HilFaultSnapshot {
    HilFaultState state = HilFaultState::Disarmed;
    uint32_t armSequence = 0;
    uint32_t readySequence = 0;
    uint32_t activeGeneration = 0;
    uint16_t exactPhase = 0;
    uint32_t readyTimestampMs = 0;
    uint32_t automaticReleaseDeadlineMs = 0;
};

struct HilReadyResult {
    HilFaultResult result = HilFaultResult::WrongState;
    uint32_t readySequence = 0;
};

class HilFaultController {
  public:
    using MonotonicClock = uint32_t (*)(void*) noexcept;

    static constexpr uint32_t kDefaultMaximumSessionDurationMs = 60000;
    static constexpr uint32_t kBsc16MaximumSessionDurationMs = 180000;
    static constexpr uint32_t kMaximumAutomaticReleaseMs = 5000;
    static constexpr size_t kMaximumSessionHashesPerBoot = 32;

    static constexpr uint32_t maximumSessionDurationMs(const HilCaseId caseId) noexcept {
        return caseId == HilCaseId::Bsc16 ? kBsc16MaximumSessionDurationMs : kDefaultMaximumSessionDurationMs;
    }

    explicit HilFaultController(MonotonicClock clock = nullptr, void* clockContext = nullptr) noexcept;

    HilFaultResult beginSession(HilCaseId caseId, const HilSessionTokenHash& sessionHash, uint32_t deadlineMs,
                                uint32_t nowMs) noexcept;
    HilFaultResult endSession(HilCaseId caseId, const HilSessionTokenHash& sessionHash) noexcept;
    HilFaultResult arm(HilCaseId caseId, HilFaultId faultId, const HilSessionTokenHash& sessionHash,
                       uint32_t armSequence, uint32_t nowMs) noexcept;
    HilReadyResult publishReady(HilCaseId caseId, HilFaultId faultId, const HilSessionTokenHash& sessionHash,
                                uint32_t armSequence, uint32_t activeGeneration, uint16_t exactPhase, uint32_t nowMs,
                                uint32_t automaticReleaseAfterMs) noexcept;
    HilFaultResult fire(HilCaseId caseId, HilFaultId faultId, const HilSessionTokenHash& sessionHash,
                        uint32_t armSequence, uint32_t readySequence, uint32_t activeGeneration, uint16_t exactPhase,
                        uint32_t nowMs) noexcept;
    HilFaultResult observeCompetingOperation(HilCaseId caseId, HilFaultId faultId,
                                             const HilSessionTokenHash& sessionHash, uint32_t armSequence,
                                             uint32_t readySequence, uint32_t activeGeneration, uint16_t exactPhase,
                                             uint32_t nowMs) noexcept;
    HilFaultResult release(HilCaseId caseId, HilFaultId faultId, const HilSessionTokenHash& sessionHash,
                           uint32_t armSequence, uint32_t readySequence, uint32_t activeGeneration, uint16_t exactPhase,
                           uint32_t nowMs) noexcept;

    void service(uint32_t nowMs) noexcept;
    bool sessionActive() const noexcept;
    HilCaseId activeCase() const noexcept;
    HilFaultSnapshot snapshot(HilFaultId faultId) const noexcept;
    bool shouldPause(HilCaseId caseId, HilFaultId faultId, const HilSessionTokenHash& sessionHash, uint32_t armSequence,
                     uint32_t readySequence, uint32_t activeGeneration, uint16_t exactPhase,
                     uint32_t nowMs) const noexcept;

    static bool isAllowed(HilCaseId caseId, HilFaultId faultId) noexcept;
    static const char* caseName(HilCaseId caseId) noexcept;
    static const char* faultName(HilFaultId faultId) noexcept;
    static const char* stateName(HilFaultState state) noexcept;

#if defined(UNIT_TEST)
    using ReadyReservationHook = void (*)(HilFaultController&, uint32_t, void*);
    using SnapshotReadHook = void (*)(void*);
    void setReadyReservationHook(ReadyReservationHook hook, void* context) noexcept {
        readyReservationHook_ = hook;
        readyReservationContext_ = context;
    }
    void setSnapshotReadHook(SnapshotReadHook hook, void* context) noexcept {
        snapshotReadHook_ = hook;
        snapshotReadContext_ = context;
    }
#endif

  private:
    static constexpr size_t kFaultCount = static_cast<size_t>(HilFaultId::Invalid);
    static constexpr uint32_t kStateBits = 3;
    static constexpr uint32_t kStateMask = (1u << kStateBits) - 1u;
    static constexpr uint32_t kEpochMask = (1u << (32u - kStateBits)) - 1u;
    static constexpr uint32_t kUpdatingStateRaw = 6u;
    static constexpr uint32_t kCompetingOperationObservedStateRaw = 7u;

    struct Slot {
        HilAtomicUint32 epochAndState{0};
        HilAtomicUint32 armSequence{0};
        HilAtomicUint32 readySequence{0};
        HilAtomicUint32 activeGeneration{0};
        HilAtomicUint32 exactPhase{0};
        HilAtomicUint32 readyTimestampMs{0};
        HilAtomicUint32 automaticReleaseDeadlineMs{0};
    };

    std::array<Slot, kFaultCount> slots_{};
    std::array<HilAtomicUint32, 8> sessionHashWords_{};
    HilAtomicUint32 activeEpoch_{0};
    HilAtomicUint32 epochCounter_{0};
    HilAtomicUint32 nextReadySequence_{1};
    HilAtomicUint32 sessionDeadlineMs_{0};
    HilAtomicUint32 activeCase_{static_cast<uint32_t>(HilCaseId::Invalid)};
    HilAtomicUint32 mutationGate_{0};
    std::array<HilSessionTokenHash, kMaximumSessionHashesPerBoot> usedSessionHashes_{};
    size_t usedSessionHashCount_ = 0;
    MonotonicClock clock_ = nullptr;
    void* clockContext_ = nullptr;
#if defined(UNIT_TEST)
    ReadyReservationHook readyReservationHook_ = nullptr;
    void* readyReservationContext_ = nullptr;
    mutable SnapshotReadHook snapshotReadHook_ = nullptr;
    mutable void* snapshotReadContext_ = nullptr;
#endif

    static uint32_t pack(uint32_t epoch, HilFaultState state) noexcept;
    static uint32_t packRaw(uint32_t epoch, uint32_t state) noexcept;
    static uint32_t unpackEpoch(uint32_t packed) noexcept;
    static HilFaultState unpackState(uint32_t packed) noexcept;
    static bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) noexcept;
    static bool durationIsBounded(uint32_t nowMs, uint32_t deadlineMs, uint32_t maximumDurationMs) noexcept;
    static size_t faultIndex(HilFaultId faultId) noexcept;

    void storeSessionHash(const HilSessionTokenHash& hash) noexcept;
    static bool sessionHashIsZero(const HilSessionTokenHash& hash) noexcept;
    bool sessionHashWasUsed(const HilSessionTokenHash& hash) const noexcept;
    uint32_t completionTime(uint32_t fallbackNowMs) const noexcept;
    bool sessionHashMatches(const HilSessionTokenHash& hash) const noexcept;
    bool sessionMatches(HilCaseId caseId, const HilSessionTokenHash& hash, uint32_t& epoch) const noexcept;
    void resetSlot(Slot& slot, uint32_t epoch) noexcept;
    HilFaultResult validateReadyIdentity(const Slot& slot, uint32_t epoch, uint32_t armSequence, uint32_t readySequence,
                                         uint32_t activeGeneration, uint16_t exactPhase) const noexcept;
};

#endif // V1SIMPLE_HIL_FAULT_CONTROL
