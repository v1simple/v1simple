#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include "hil_fault_serial_module.h"

#include <cstdio>
#include <cstring>

namespace {
constexpr char kProtocolPrefix[] = "V1HIL";
constexpr size_t kMaximumTokens = 9;

size_t tokenize(char* line, char* tokens[], const size_t capacity) noexcept {
    size_t count = 0;
    char* cursor = line;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        if (count == capacity) {
            return capacity + 1;
        }
        tokens[count++] = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && *cursor != '\r' &&
               *cursor != '\n') {
            ++cursor;
        }
        if (*cursor != '\0') {
            *cursor++ = '\0';
        }
    }
    return count;
}

const char* resultName(const HilFaultResult result) noexcept {
    switch (result) {
    case HilFaultResult::Ok: return "ok";
    case HilFaultResult::NoSession: return "no_session";
    case HilFaultResult::SessionActive: return "session_active";
    case HilFaultResult::WrongCase: return "wrong_case";
    case HilFaultResult::WrongFault: return "wrong_fault";
    case HilFaultResult::WrongSession: return "wrong_session";
    case HilFaultResult::InvalidSessionHash: return "invalid_session_hash";
    case HilFaultResult::WrongState: return "wrong_state";
    case HilFaultResult::WrongArmSequence: return "wrong_arm_sequence";
    case HilFaultResult::WrongReadySequence: return "wrong_ready_sequence";
    case HilFaultResult::WrongGeneration: return "wrong_generation";
    case HilFaultResult::WrongPhase: return "wrong_phase";
    case HilFaultResult::CompetingOperationMissing: return "competing_operation_missing";
    case HilFaultResult::InvalidDeadline: return "invalid_deadline";
    case HilFaultResult::DuplicateFire: return "duplicate_fire";
    case HilFaultResult::Expired: return "expired";
    case HilFaultResult::Busy: return "busy";
    case HilFaultResult::SessionHashRegistryFull: return "session_hash_registry_full";
    }
    return "invalid";
}

const char* parseResultName(const HilSerialParseResult result) noexcept {
    switch (result) {
    case HilSerialParseResult::Ok: return "ok";
    case HilSerialParseResult::Empty: return "empty";
    case HilSerialParseResult::TooLong: return "too_long";
    case HilSerialParseResult::InvalidPrefix: return "invalid_prefix";
    case HilSerialParseResult::UnknownCommand: return "unknown_command";
    case HilSerialParseResult::InvalidArguments: return "invalid_arguments";
    }
    return "invalid";
}
}  // namespace

HilSerialParseResult HilFaultSerialModule::parse(const char* line,
                                                 HilSerialCommand& command) noexcept {
    command = HilSerialCommand{};
    if (line == nullptr || line[0] == '\0') {
        return HilSerialParseResult::Empty;
    }
    const size_t length = strnlen(line, kMaximumCommandLength + 1);
    if (length == 0) {
        return HilSerialParseResult::Empty;
    }
    if (length > kMaximumCommandLength) {
        return HilSerialParseResult::TooLong;
    }

    char storage[kMaximumCommandLength + 1]{};
    std::memcpy(storage, line, length);
    storage[length] = '\0';
    char* tokens[kMaximumTokens]{};
    const size_t tokenCount = tokenize(storage, tokens, kMaximumTokens);
    if (tokenCount == 0) {
        return HilSerialParseResult::Empty;
    }
    if (tokenCount > kMaximumTokens || std::strcmp(tokens[0], kProtocolPrefix) != 0) {
        return tokenCount > kMaximumTokens ? HilSerialParseResult::InvalidArguments
                                           : HilSerialParseResult::InvalidPrefix;
    }
    if (tokenCount < 2) {
        return HilSerialParseResult::UnknownCommand;
    }

    if (std::strcmp(tokens[1], "STATUS") == 0) {
        if (tokenCount != 2) {
            return HilSerialParseResult::InvalidArguments;
        }
        command.kind = HilSerialCommandKind::Status;
        return HilSerialParseResult::Ok;
    }
    if (std::strcmp(tokens[1], "BEGIN") == 0) {
        if (tokenCount != 5 || (command.caseId = parseCase(tokens[2])) == HilCaseId::Invalid ||
            !parseHash(tokens[3], command.sessionHash) ||
            !parseUint32(tokens[4], command.durationMs) || command.durationMs == 0 ||
            command.durationMs > HilFaultController::kMaximumSessionDurationMs) {
            return HilSerialParseResult::InvalidArguments;
        }
        command.kind = HilSerialCommandKind::Begin;
        return HilSerialParseResult::Ok;
    }
    if (std::strcmp(tokens[1], "ARM") == 0) {
        if (tokenCount != 6 || (command.caseId = parseCase(tokens[2])) == HilCaseId::Invalid ||
            (command.faultId = parseFault(tokens[3])) == HilFaultId::Invalid ||
            !parseHash(tokens[4], command.sessionHash) ||
            !parseUint32(tokens[5], command.armSequence) || command.armSequence == 0) {
            return HilSerialParseResult::InvalidArguments;
        }
        command.kind = HilSerialCommandKind::Arm;
        return HilSerialParseResult::Ok;
    }
    if (std::strcmp(tokens[1], "RELEASE") == 0) {
        uint32_t exactPhase = 0;
        if (tokenCount != 9 || (command.caseId = parseCase(tokens[2])) == HilCaseId::Invalid ||
            (command.faultId = parseFault(tokens[3])) == HilFaultId::Invalid ||
            !parseHash(tokens[4], command.sessionHash) ||
            !parseUint32(tokens[5], command.armSequence) ||
            !parseUint32(tokens[6], command.readySequence) ||
            !parseUint32(tokens[7], command.activeGeneration) ||
            !parseUint32(tokens[8], exactPhase) || command.armSequence == 0 ||
            command.readySequence == 0 || command.activeGeneration == 0 || exactPhase == 0 ||
            exactPhase > UINT16_MAX) {
            return HilSerialParseResult::InvalidArguments;
        }
        command.exactPhase = static_cast<uint16_t>(exactPhase);
        command.kind = HilSerialCommandKind::Release;
        return HilSerialParseResult::Ok;
    }
    if (std::strcmp(tokens[1], "END") == 0) {
        if (tokenCount != 4 || (command.caseId = parseCase(tokens[2])) == HilCaseId::Invalid ||
            !parseHash(tokens[3], command.sessionHash)) {
            return HilSerialParseResult::InvalidArguments;
        }
        command.kind = HilSerialCommandKind::End;
        return HilSerialParseResult::Ok;
    }
    return HilSerialParseResult::UnknownCommand;
}

HilFaultResult HilFaultSerialModule::execute(const HilSerialCommand& command,
                                             HilFaultController& controller,
                                             const uint32_t nowMs) noexcept {
    switch (command.kind) {
    case HilSerialCommandKind::Begin:
        return controller.beginSession(command.caseId, command.sessionHash,
                                       nowMs + command.durationMs, nowMs);
    case HilSerialCommandKind::Arm:
        return controller.arm(command.caseId, command.faultId, command.sessionHash,
                              command.armSequence, nowMs);
    case HilSerialCommandKind::Release:
        return controller.release(command.caseId, command.faultId, command.sessionHash,
                                  command.armSequence, command.readySequence,
                                  command.activeGeneration, command.exactPhase, nowMs);
    case HilSerialCommandKind::Status:
        controller.service(nowMs);
        return controller.sessionActive() ? HilFaultResult::Ok : HilFaultResult::NoSession;
    case HilSerialCommandKind::End:
        return controller.endSession(command.caseId, command.sessionHash);
    case HilSerialCommandKind::Invalid:
        return HilFaultResult::WrongState;
    }
    return HilFaultResult::WrongState;
}

bool HilFaultSerialModule::formatResponse(const HilSerialParseResult parseResult,
                                          const HilFaultResult result,
                                          const HilFaultController& controller,
                                          char* output,
                                          const size_t outputSize) noexcept {
    if (output == nullptr || outputSize == 0) {
        return false;
    }
    const int written = std::snprintf(
        output, outputSize,
        "{\"ok\":%s,\"parse\":\"%s\",\"result\":\"%s\",\"case_id\":\"%s\"}\n",
        parseResult == HilSerialParseResult::Ok && result == HilFaultResult::Ok ? "true" : "false",
        parseResultName(parseResult), resultName(result),
        HilFaultController::caseName(controller.activeCase()));
    return written > 0 && static_cast<size_t>(written) < outputSize;
}

HilCaseId HilFaultSerialModule::parseCase(const char* token) noexcept {
    constexpr HilCaseId cases[] = {HilCaseId::Bsc02, HilCaseId::Bsc04, HilCaseId::Bsc05,
                                   HilCaseId::Bsc06, HilCaseId::Bsc10, HilCaseId::Bsc13,
                                   HilCaseId::Bsc14, HilCaseId::Bsc16};
    for (const HilCaseId caseId : cases) {
        if (std::strcmp(token, HilFaultController::caseName(caseId)) == 0) {
            return caseId;
        }
    }
    return HilCaseId::Invalid;
}

HilFaultId HilFaultSerialModule::parseFault(const char* token) noexcept {
    for (uint8_t raw = 0; raw < static_cast<uint8_t>(HilFaultId::Invalid); ++raw) {
        const auto faultId = static_cast<HilFaultId>(raw);
        if (std::strcmp(token, HilFaultController::faultName(faultId)) == 0) {
            return faultId;
        }
    }
    return HilFaultId::Invalid;
}

bool HilFaultSerialModule::parseHash(const char* token, HilSessionTokenHash& hash) noexcept {
    if (token == nullptr || std::strlen(token) != 64) {
        return false;
    }
    uint8_t combined = 0;
    for (size_t index = 0; index < hash.bytes.size(); ++index) {
        uint8_t value = 0;
        for (size_t nibble = 0; nibble < 2; ++nibble) {
            const char c = token[(index * 2) + nibble];
            uint8_t decoded = 0;
            if (c >= '0' && c <= '9') {
                decoded = static_cast<uint8_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                decoded = static_cast<uint8_t>(10 + c - 'a');
            } else {
                return false;
            }
            value = static_cast<uint8_t>((value << 4u) | decoded);
        }
        hash.bytes[index] = value;
        combined |= value;
    }
    return combined != 0;
}

bool HilFaultSerialModule::parseUint32(const char* token, uint32_t& value) noexcept {
    if (token == nullptr || token[0] == '\0') {
        return false;
    }
    uint32_t parsed = 0;
    for (const char* cursor = token; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
        if (parsed > (UINT32_MAX - digit) / 10u) {
            return false;
        }
        parsed = (parsed * 10u) + digit;
    }
    value = parsed;
    return true;
}

#endif  // V1SIMPLE_HIL_FAULT_CONTROL
