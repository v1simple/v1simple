#pragma once

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include <cstddef>
#include <cstdint>
#include <array>

#include "hil_fault_controller.h"

enum class HilSerialCommandKind : uint8_t {
    Begin,
    Arm,
    Release,
    Status,
    End,
    NextBoot,
    Invalid,
};

enum class HilSerialParseResult : uint8_t {
    Ok,
    Empty,
    TooLong,
    InvalidPrefix,
    UnknownCommand,
    InvalidArguments,
};

struct HilSerialCommand {
    HilSerialCommandKind kind = HilSerialCommandKind::Invalid;
    HilCaseId caseId = HilCaseId::Invalid;
    HilFaultId faultId = HilFaultId::Invalid;
    HilSessionTokenHash sessionHash{};
    uint32_t durationMs = 0;
    uint32_t armSequence = 0;
    uint32_t readySequence = 0;
    uint32_t activeGeneration = 0;
    uint16_t exactPhase = 0;
};

class HilFaultSerialModule {
  public:
    static constexpr size_t kMaximumCommandLength = 224;

    static HilSerialParseResult parse(const char* line, HilSerialCommand& command) noexcept;
    static HilFaultResult execute(const HilSerialCommand& command, HilFaultController& controller,
                                  uint32_t nowMs) noexcept;
    static bool formatResponse(HilSerialParseResult parseResult, HilFaultResult result,
                               const HilFaultController& controller, char* output, size_t outputSize) noexcept;

  private:
    static HilCaseId parseCase(const char* token) noexcept;
    static HilFaultId parseFault(const char* token) noexcept;
    static bool parseHash(const char* token, HilSessionTokenHash& hash) noexcept;
    static bool parseUint32(const char* token, uint32_t& value) noexcept;
};

struct HilArmedFaultIdentity {
    HilCaseId caseId = HilCaseId::Invalid;
    HilFaultId faultId = HilFaultId::Invalid;
    HilSessionTokenHash sessionHash{};
    uint32_t armSequence = 0;
};

class HilFaultRuntimeOwner {
  public:
    using WriteSerial = void (*)(const char*, void*) noexcept;
    using StageNextBoot = bool (*)(const HilArmedFaultIdentity&, uint32_t, uint32_t, void*) noexcept;
    using ClearNextBoot = void (*)(void*) noexcept;

    void configureSerial(WriteSerial writer, void* context) noexcept;
    void configureNextBoot(StageNextBoot stage, ClearNextBoot clear, void* context) noexcept;
    void acceptSerialByte(char value, uint32_t nowMs) noexcept;
    HilFaultResult restoreOneShot(const HilArmedFaultIdentity& identity, uint32_t nowMs, uint32_t durationMs) noexcept;
    HilFaultResult abortSession(const HilArmedFaultIdentity& identity) noexcept;
    void service(uint32_t nowMs) noexcept;
    bool armedIdentity(HilFaultId faultId, HilArmedFaultIdentity& identity) const noexcept;
    HilFaultController& controller() noexcept;
    const HilFaultController& controller() const noexcept;

  private:
    static constexpr size_t kFaultCount = static_cast<size_t>(HilFaultId::Invalid);
    static constexpr size_t kResponseBytes = 224;

    void executeSerialLine(uint32_t nowMs) noexcept;
    HilFaultResult executeCommand(const HilSerialCommand& command, uint32_t nowMs) noexcept;
    void bindSuccessfulCommand(const HilSerialCommand& command, HilFaultResult result, uint32_t nowMs) noexcept;
    void invalidateStagedNextBoot() noexcept;
    void clearTrackedSession() noexcept;
    static bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) noexcept;
    static bool identitiesEqual(const HilArmedFaultIdentity& left, const HilArmedFaultIdentity& right) noexcept;

    HilFaultController controller_{};
    std::array<HilArmedFaultIdentity, kFaultCount> bindings_{};
    std::array<char, HilFaultSerialModule::kMaximumCommandLength + 1> line_{};
    size_t lineLength_ = 0;
    bool lineOverflow_ = false;
    WriteSerial writer_ = nullptr;
    void* writerContext_ = nullptr;
    StageNextBoot stageNextBoot_ = nullptr;
    ClearNextBoot clearNextBoot_ = nullptr;
    void* stageContext_ = nullptr;
    HilArmedFaultIdentity stagedNextBootIdentity_{};
    HilCaseId activeSessionCase_ = HilCaseId::Invalid;
    HilSessionTokenHash activeSessionHash_{};
    uint32_t activeSessionDeadlineMs_ = 0;
    bool activeSessionTracked_ = false;
    bool stagedNextBoot_ = false;
};

HilFaultRuntimeOwner& hilFaultRuntimeOwner() noexcept;

#endif // V1SIMPLE_HIL_FAULT_CONTROL
