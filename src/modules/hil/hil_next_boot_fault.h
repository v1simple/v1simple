#pragma once

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/hil/hil_fault_serial_module.h"

struct HilNextBootFaultRecord {
    uint32_t magic = 0;
    uint16_t schemaVersion = 0;
    uint8_t caseId = 0;
    uint8_t faultId = 0;
    uint32_t armSequence = 0;
    uint32_t targetBootSequence = 0;
    uint32_t stagedAtMs = 0;
    uint32_t sessionDeadlineMs = 0;
    uint32_t remainingSessionMs = 0;
    uint64_t persistentStagedAtMs = 0;
    uint64_t persistentDeadlineMs = 0;
    std::array<uint8_t, 32> sessionHash{};
    uint32_t crc32 = 0;
};

class HilNextBootFaultStore {
  public:
    using PersistentClockMs = uint64_t (*)(void*) noexcept;

    static constexpr uint16_t kSchemaVersion = 1;

    HilNextBootFaultStore(HilFaultRuntimeOwner& owner, HilNextBootFaultRecord& record, uint32_t currentBootSequence,
                          HilCaseId caseId, HilFaultId faultId, uint32_t magic,
                          PersistentClockMs persistentClockMs = nullptr, void* clockContext = nullptr) noexcept
        : owner_(owner), record_(record), currentBootSequence_(currentBootSequence), caseId_(caseId), faultId_(faultId),
          magic_(magic), persistentClockMs_(persistentClockMs), clockContext_(clockContext) {}

    void configureClock(PersistentClockMs persistentClockMs, void* context) noexcept {
        persistentClockMs_ = persistentClockMs;
        clockContext_ = context;
    }

    bool stage(const HilArmedFaultIdentity& identity, const uint32_t sessionDeadlineMs,
               const uint32_t stagedAtMs) noexcept {
        HilArmedFaultIdentity armed{};
        const HilFaultSnapshot controllerState = owner_.controller().snapshot(faultId_);
        const int32_t remaining = static_cast<int32_t>(sessionDeadlineMs - stagedAtMs);
        if (identity.caseId != caseId_ || identity.faultId != faultId_ || identity.armSequence == 0 || remaining <= 0 ||
            persistentClockMs_ == nullptr || controllerState.state != HilFaultState::Armed ||
            controllerState.armSequence != identity.armSequence ||
            static_cast<uint32_t>(remaining) > HilFaultController::maximumSessionDurationMs(caseId_) ||
            !owner_.armedIdentity(identity.faultId, armed) || armed.caseId != identity.caseId ||
            armed.faultId != identity.faultId || armed.armSequence != identity.armSequence ||
            !(armed.sessionHash == identity.sessionHash)) {
            return false;
        }

        HilNextBootFaultRecord staged{};
        staged.magic = magic_;
        staged.schemaVersion = kSchemaVersion;
        staged.caseId = static_cast<uint8_t>(identity.caseId);
        staged.faultId = static_cast<uint8_t>(identity.faultId);
        staged.armSequence = identity.armSequence;
        staged.targetBootSequence = currentBootSequence_ + 1u;
        if (staged.targetBootSequence == 0) {
            staged.targetBootSequence = 1;
        }
        staged.sessionHash = identity.sessionHash.bytes;
        staged.stagedAtMs = stagedAtMs;
        staged.sessionDeadlineMs = sessionDeadlineMs;
        staged.remainingSessionMs = static_cast<uint32_t>(remaining);
        staged.persistentStagedAtMs = persistentClockMs_(clockContext_);
        staged.persistentDeadlineMs = staged.persistentStagedAtMs + staged.remainingSessionMs;
        if (staged.persistentDeadlineMs < staged.persistentStagedAtMs) {
            return false;
        }
        staged.crc32 = recordCrc32(staged);
        record_ = staged;
        return true;
    }

    void clear() noexcept { record_ = HilNextBootFaultRecord{}; }

    HilFaultResult restore(const bool enabled, const uint32_t nowMs) noexcept {
        const HilNextBootFaultRecord candidate = record_;
        record_ = HilNextBootFaultRecord{};
        const uint64_t persistentNow = persistentClockMs_ == nullptr ? UINT64_MAX : persistentClockMs_(clockContext_);
        if (!enabled || candidate.magic != magic_ || candidate.schemaVersion != kSchemaVersion ||
            candidate.caseId != static_cast<uint8_t>(caseId_) || candidate.faultId != static_cast<uint8_t>(faultId_) ||
            candidate.armSequence == 0 || candidate.targetBootSequence == 0 ||
            candidate.targetBootSequence != currentBootSequence_ || candidate.remainingSessionMs == 0 ||
            candidate.remainingSessionMs > HilFaultController::maximumSessionDurationMs(caseId_) ||
            static_cast<uint32_t>(candidate.sessionDeadlineMs - candidate.stagedAtMs) != candidate.remainingSessionMs ||
            candidate.persistentDeadlineMs < candidate.persistentStagedAtMs ||
            candidate.persistentDeadlineMs - candidate.persistentStagedAtMs != candidate.remainingSessionMs ||
            persistentNow < candidate.persistentStagedAtMs || persistentNow >= candidate.persistentDeadlineMs ||
            candidate.crc32 != recordCrc32(candidate)) {
            return HilFaultResult::WrongState;
        }

        HilArmedFaultIdentity identity{};
        identity.caseId = caseId_;
        identity.faultId = faultId_;
        identity.sessionHash.bytes = candidate.sessionHash;
        identity.armSequence = candidate.armSequence;
        const uint64_t remainingPersistentMs = candidate.persistentDeadlineMs - persistentNow;
        if (remainingPersistentMs == 0 || remainingPersistentMs > candidate.remainingSessionMs ||
            remainingPersistentMs > UINT32_MAX) {
            return HilFaultResult::InvalidDeadline;
        }
        return owner_.restoreOneShot(identity, nowMs, static_cast<uint32_t>(remainingPersistentMs));
    }

  private:
    static void crc32Byte(uint32_t& crc, const uint8_t value) noexcept {
        crc ^= value;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }

    static uint32_t recordCrc32(const HilNextBootFaultRecord& record) noexcept {
        uint32_t crc = 0xFFFFFFFFu;
        for (uint8_t shift = 0; shift < 32; shift += 8) {
            crc32Byte(crc, static_cast<uint8_t>(record.magic >> shift));
        }
        for (uint8_t shift = 0; shift < 64; shift += 8) {
            crc32Byte(crc, static_cast<uint8_t>(record.persistentStagedAtMs >> shift));
            crc32Byte(crc, static_cast<uint8_t>(record.persistentDeadlineMs >> shift));
        }
        for (uint8_t shift = 0; shift < 16; shift += 8) {
            crc32Byte(crc, static_cast<uint8_t>(record.schemaVersion >> shift));
        }
        crc32Byte(crc, record.caseId);
        crc32Byte(crc, record.faultId);
        for (uint8_t shift = 0; shift < 32; shift += 8) {
            crc32Byte(crc, static_cast<uint8_t>(record.armSequence >> shift));
        }
        for (uint8_t shift = 0; shift < 32; shift += 8) {
            crc32Byte(crc, static_cast<uint8_t>(record.targetBootSequence >> shift));
        }
        for (uint8_t shift = 0; shift < 32; shift += 8) {
            crc32Byte(crc, static_cast<uint8_t>(record.stagedAtMs >> shift));
            crc32Byte(crc, static_cast<uint8_t>(record.sessionDeadlineMs >> shift));
            crc32Byte(crc, static_cast<uint8_t>(record.remainingSessionMs >> shift));
        }
        for (const uint8_t value : record.sessionHash) {
            crc32Byte(crc, value);
        }
        return crc ^ 0xFFFFFFFFu;
    }

    HilFaultRuntimeOwner& owner_;
    HilNextBootFaultRecord& record_;
    uint32_t currentBootSequence_ = 0;
    HilCaseId caseId_ = HilCaseId::Invalid;
    HilFaultId faultId_ = HilFaultId::Invalid;
    uint32_t magic_ = 0;
    PersistentClockMs persistentClockMs_ = nullptr;
    void* clockContext_ = nullptr;
};

struct HilNextBootFaultRoute {
    using Stage = bool (*)(const HilArmedFaultIdentity&, uint32_t, uint32_t, void*) noexcept;
    using Clear = void (*)(void*) noexcept;

    HilCaseId caseId = HilCaseId::Invalid;
    HilFaultId faultId = HilFaultId::Invalid;
    Stage stage = nullptr;
    Clear clear = nullptr;
    void* context = nullptr;
};

class HilNextBootFaultRouter {
  public:
    static constexpr size_t kMaximumRoutes = 2;

    bool configure(const size_t index, const HilNextBootFaultRoute& route) noexcept {
        if (index >= routes_.size() || route.caseId == HilCaseId::Invalid || route.faultId == HilFaultId::Invalid ||
            route.stage == nullptr || route.clear == nullptr) {
            return false;
        }
        for (size_t candidate = 0; candidate < routes_.size(); ++candidate) {
            if (candidate != index && routes_[candidate].caseId == route.caseId &&
                routes_[candidate].faultId == route.faultId) {
                return false;
            }
        }
        routes_[index] = route;
        return true;
    }

    bool stage(const HilArmedFaultIdentity& identity, const uint32_t sessionDeadlineMs,
               const uint32_t stagedAtMs) noexcept {
        for (const HilNextBootFaultRoute& route : routes_) {
            if (route.caseId == identity.caseId && route.faultId == identity.faultId && route.stage != nullptr) {
                return route.stage(identity, sessionDeadlineMs, stagedAtMs, route.context);
            }
        }
        return false;
    }

    void clear() noexcept {
        for (const HilNextBootFaultRoute& route : routes_) {
            if (route.clear != nullptr) {
                route.clear(route.context);
            }
        }
    }

    static bool stageCallback(const HilArmedFaultIdentity& identity, const uint32_t sessionDeadlineMs,
                              const uint32_t stagedAtMs, void* context) noexcept {
        return context != nullptr &&
               static_cast<HilNextBootFaultRouter*>(context)->stage(identity, sessionDeadlineMs, stagedAtMs);
    }

    static void clearCallback(void* context) noexcept {
        if (context != nullptr) {
            static_cast<HilNextBootFaultRouter*>(context)->clear();
        }
    }

  private:
    std::array<HilNextBootFaultRoute, kMaximumRoutes> routes_{};
};

#endif // V1SIMPLE_HIL_FAULT_CONTROL
