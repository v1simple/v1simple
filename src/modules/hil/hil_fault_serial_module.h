#pragma once

#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include <cstddef>
#include <cstdint>

#include "hil_fault_controller.h"

enum class HilSerialCommandKind : uint8_t {
    Begin,
    Arm,
    Release,
    Status,
    End,
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
    static HilFaultResult execute(const HilSerialCommand& command,
                                  HilFaultController& controller,
                                  uint32_t nowMs) noexcept;
    static bool formatResponse(HilSerialParseResult parseResult,
                               HilFaultResult result,
                               const HilFaultController& controller,
                               char* output,
                               size_t outputSize) noexcept;

  private:
    static HilCaseId parseCase(const char* token) noexcept;
    static HilFaultId parseFault(const char* token) noexcept;
    static bool parseHash(const char* token, HilSessionTokenHash& hash) noexcept;
    static bool parseUint32(const char* token, uint32_t& value) noexcept;
};

#endif  // V1SIMPLE_HIL_FAULT_CONTROL
