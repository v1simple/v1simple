#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include "hil_fault_serial_module.h"

#include <cstdio>
#include <cstring>

namespace {
constexpr char kProtocolPrefix[] = "V1HIL";
constexpr size_t kMaximumTokens = 9;
HilFaultRuntimeOwner gHilFaultRuntimeOwner;

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
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && *cursor != '\r' && *cursor != '\n') {
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
    case HilFaultResult::Ok:
        return "ok";
    case HilFaultResult::NoSession:
        return "no_session";
    case HilFaultResult::SessionActive:
        return "session_active";
    case HilFaultResult::WrongCase:
        return "wrong_case";
    case HilFaultResult::WrongFault:
        return "wrong_fault";
    case HilFaultResult::WrongSession:
        return "wrong_session";
    case HilFaultResult::InvalidSessionHash:
        return "invalid_session_hash";
    case HilFaultResult::WrongState:
        return "wrong_state";
    case HilFaultResult::WrongArmSequence:
        return "wrong_arm_sequence";
    case HilFaultResult::WrongReadySequence:
        return "wrong_ready_sequence";
    case HilFaultResult::WrongGeneration:
        return "wrong_generation";
    case HilFaultResult::WrongPhase:
        return "wrong_phase";
    case HilFaultResult::CompetingOperationMissing:
        return "competing_operation_missing";
    case HilFaultResult::InvalidDeadline:
        return "invalid_deadline";
    case HilFaultResult::DuplicateFire:
        return "duplicate_fire";
    case HilFaultResult::Expired:
        return "expired";
    case HilFaultResult::Busy:
        return "busy";
    case HilFaultResult::SessionHashRegistryFull:
        return "session_hash_registry_full";
    }
    return "invalid";
}

const char* parseResultName(const HilSerialParseResult result) noexcept {
    switch (result) {
    case HilSerialParseResult::Ok:
        return "ok";
    case HilSerialParseResult::Empty:
        return "empty";
    case HilSerialParseResult::TooLong:
        return "too_long";
    case HilSerialParseResult::InvalidPrefix:
        return "invalid_prefix";
    case HilSerialParseResult::UnknownCommand:
        return "unknown_command";
    case HilSerialParseResult::InvalidArguments:
        return "invalid_arguments";
    }
    return "invalid";
}
} // namespace

HilSerialParseResult HilFaultSerialModule::parse(const char* line, HilSerialCommand& command) noexcept {
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
            !parseHash(tokens[3], command.sessionHash) || !parseUint32(tokens[4], command.durationMs) ||
            command.durationMs == 0 || command.durationMs > HilFaultController::kMaximumSessionDurationMs) {
            return HilSerialParseResult::InvalidArguments;
        }
        command.kind = HilSerialCommandKind::Begin;
        return HilSerialParseResult::Ok;
    }
    if (std::strcmp(tokens[1], "ARM") == 0) {
        if (tokenCount != 6 || (command.caseId = parseCase(tokens[2])) == HilCaseId::Invalid ||
            (command.faultId = parseFault(tokens[3])) == HilFaultId::Invalid ||
            !parseHash(tokens[4], command.sessionHash) || !parseUint32(tokens[5], command.armSequence) ||
            command.armSequence == 0) {
            return HilSerialParseResult::InvalidArguments;
        }
        command.kind = HilSerialCommandKind::Arm;
        return HilSerialParseResult::Ok;
    }
    if (std::strcmp(tokens[1], "NEXT_BOOT") == 0) {
        if (tokenCount != 6 || (command.caseId = parseCase(tokens[2])) == HilCaseId::Invalid ||
            (command.faultId = parseFault(tokens[3])) == HilFaultId::Invalid ||
            !parseHash(tokens[4], command.sessionHash) || !parseUint32(tokens[5], command.armSequence) ||
            command.armSequence == 0) {
            return HilSerialParseResult::InvalidArguments;
        }
        command.kind = HilSerialCommandKind::NextBoot;
        return HilSerialParseResult::Ok;
    }
    if (std::strcmp(tokens[1], "RELEASE") == 0) {
        uint32_t exactPhase = 0;
        if (tokenCount != 9 || (command.caseId = parseCase(tokens[2])) == HilCaseId::Invalid ||
            (command.faultId = parseFault(tokens[3])) == HilFaultId::Invalid ||
            !parseHash(tokens[4], command.sessionHash) || !parseUint32(tokens[5], command.armSequence) ||
            !parseUint32(tokens[6], command.readySequence) || !parseUint32(tokens[7], command.activeGeneration) ||
            !parseUint32(tokens[8], exactPhase) || command.armSequence == 0 || command.readySequence == 0 ||
            command.activeGeneration == 0 || exactPhase == 0 || exactPhase > UINT16_MAX) {
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

HilFaultResult HilFaultSerialModule::execute(const HilSerialCommand& command, HilFaultController& controller,
                                             const uint32_t nowMs) noexcept {
    switch (command.kind) {
    case HilSerialCommandKind::Begin:
        return controller.beginSession(command.caseId, command.sessionHash, nowMs + command.durationMs, nowMs);
    case HilSerialCommandKind::Arm:
        return controller.arm(command.caseId, command.faultId, command.sessionHash, command.armSequence, nowMs);
    case HilSerialCommandKind::Release:
        return controller.release(command.caseId, command.faultId, command.sessionHash, command.armSequence,
                                  command.readySequence, command.activeGeneration, command.exactPhase, nowMs);
    case HilSerialCommandKind::Status:
        controller.service(nowMs);
        return controller.sessionActive() ? HilFaultResult::Ok : HilFaultResult::NoSession;
    case HilSerialCommandKind::End:
        return controller.endSession(command.caseId, command.sessionHash);
    case HilSerialCommandKind::NextBoot:
        return HilFaultResult::WrongState;
    case HilSerialCommandKind::Invalid:
        return HilFaultResult::WrongState;
    }
    return HilFaultResult::WrongState;
}

bool HilFaultSerialModule::formatResponse(const HilSerialParseResult parseResult, const HilFaultResult result,
                                          const HilFaultController& controller, char* output,
                                          const size_t outputSize) noexcept {
    if (output == nullptr || outputSize == 0) {
        return false;
    }
    const int written = std::snprintf(
        output, outputSize, "{\"ok\":%s,\"parse\":\"%s\",\"result\":\"%s\",\"case_id\":\"%s\"}\n",
        parseResult == HilSerialParseResult::Ok && result == HilFaultResult::Ok ? "true" : "false",
        parseResultName(parseResult), resultName(result), HilFaultController::caseName(controller.activeCase()));
    return written > 0 && static_cast<size_t>(written) < outputSize;
}

HilCaseId HilFaultSerialModule::parseCase(const char* token) noexcept {
    constexpr HilCaseId cases[] = {HilCaseId::Bsc02, HilCaseId::Bsc04, HilCaseId::Bsc05, HilCaseId::Bsc06,
                                   HilCaseId::Bsc10, HilCaseId::Bsc13, HilCaseId::Bsc14, HilCaseId::Bsc16};
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

void HilFaultRuntimeOwner::configureSerial(const WriteSerial writer, void* context) noexcept {
    writer_ = writer;
    writerContext_ = context;
}

void HilFaultRuntimeOwner::configureNextBoot(const StageNextBoot stage, const ClearNextBoot clear,
                                             void* context) noexcept {
    stageNextBoot_ = stage;
    clearNextBoot_ = clear;
    stageContext_ = context;
}

void HilFaultRuntimeOwner::acceptSerialByte(const char value, const uint32_t nowMs) noexcept {
    if (value == '\r') {
        return;
    }
    if (value != '\n') {
        if (lineLength_ < HilFaultSerialModule::kMaximumCommandLength) {
            line_[lineLength_++] = value;
        } else {
            lineOverflow_ = true;
        }
        return;
    }
    executeSerialLine(nowMs);
    line_.fill('\0');
    lineLength_ = 0;
    lineOverflow_ = false;
}

void HilFaultRuntimeOwner::executeSerialLine(const uint32_t nowMs) noexcept {
    HilSerialCommand command{};
    HilSerialParseResult parseResult = HilSerialParseResult::TooLong;
    HilFaultResult result = HilFaultResult::WrongState;
    if (!lineOverflow_) {
        line_[lineLength_] = '\0';
        parseResult = HilFaultSerialModule::parse(line_.data(), command);
        if (parseResult == HilSerialParseResult::Ok) {
            result = executeCommand(command, nowMs);
        }
    }
    if (writer_ == nullptr) {
        return;
    }
    char response[kResponseBytes]{};
    if (HilFaultSerialModule::formatResponse(parseResult, result, controller_, response, sizeof(response))) {
        writer_(response, writerContext_);
    }
}

HilFaultResult HilFaultRuntimeOwner::executeCommand(const HilSerialCommand& command, const uint32_t nowMs) noexcept {
    if (command.kind == HilSerialCommandKind::NextBoot) {
        HilArmedFaultIdentity requested{};
        requested.caseId = command.caseId;
        requested.faultId = command.faultId;
        requested.sessionHash = command.sessionHash;
        requested.armSequence = command.armSequence;
        HilArmedFaultIdentity armed{};
        if (!armedIdentity(command.faultId, armed) || !identitiesEqual(requested, armed) || stageNextBoot_ == nullptr ||
            !activeSessionTracked_ || deadlineReached(nowMs, activeSessionDeadlineMs_) ||
            !stageNextBoot_(requested, activeSessionDeadlineMs_, nowMs, stageContext_)) {
            return HilFaultResult::WrongState;
        }
        stagedNextBootIdentity_ = requested;
        stagedNextBoot_ = true;
        return HilFaultResult::Ok;
    }
    if (command.kind == HilSerialCommandKind::End && stagedNextBoot_ && command.caseId == activeSessionCase_ &&
        command.sessionHash == activeSessionHash_) {
        invalidateStagedNextBoot();
    }
    const HilFaultResult result = HilFaultSerialModule::execute(command, controller_, nowMs);
    bindSuccessfulCommand(command, result, nowMs);
    service(nowMs);
    return result;
}

void HilFaultRuntimeOwner::bindSuccessfulCommand(const HilSerialCommand& command, const HilFaultResult result,
                                                 const uint32_t nowMs) noexcept {
    if (result != HilFaultResult::Ok) {
        return;
    }
    if (command.kind == HilSerialCommandKind::Begin) {
        invalidateStagedNextBoot();
        bindings_.fill(HilArmedFaultIdentity{});
        activeSessionCase_ = command.caseId;
        activeSessionHash_ = command.sessionHash;
        activeSessionDeadlineMs_ = nowMs + command.durationMs;
        activeSessionTracked_ = true;
        return;
    }
    if (command.kind == HilSerialCommandKind::End) {
        bindings_.fill(HilArmedFaultIdentity{});
        invalidateStagedNextBoot();
        clearTrackedSession();
        return;
    }
    if (command.kind != HilSerialCommandKind::Arm) {
        return;
    }
    const size_t index = static_cast<size_t>(command.faultId);
    if (index >= bindings_.size()) {
        return;
    }
    bindings_[index] = {
        command.caseId,
        command.faultId,
        command.sessionHash,
        command.armSequence,
    };
}

bool HilFaultRuntimeOwner::identitiesEqual(const HilArmedFaultIdentity& left,
                                           const HilArmedFaultIdentity& right) noexcept {
    return left.caseId == right.caseId && left.faultId == right.faultId && left.armSequence == right.armSequence &&
           left.sessionHash == right.sessionHash;
}

HilFaultResult HilFaultRuntimeOwner::restoreOneShot(const HilArmedFaultIdentity& identity, const uint32_t nowMs,
                                                    const uint32_t durationMs) noexcept {
    if (!HilFaultController::isAllowed(identity.caseId, identity.faultId) || identity.armSequence == 0) {
        return HilFaultResult::WrongFault;
    }
    if (durationMs == 0 || durationMs > kRestoredSessionDurationMs) {
        return HilFaultResult::InvalidDeadline;
    }
    HilFaultResult result = controller_.beginSession(identity.caseId, identity.sessionHash, nowMs + durationMs, nowMs);
    if (result != HilFaultResult::Ok) {
        return result;
    }
    result = controller_.arm(identity.caseId, identity.faultId, identity.sessionHash, identity.armSequence, nowMs);
    if (result != HilFaultResult::Ok) {
        (void)controller_.endSession(identity.caseId, identity.sessionHash);
        return result;
    }
    const size_t index = static_cast<size_t>(identity.faultId);
    bindings_[index] = identity;
    activeSessionCase_ = identity.caseId;
    activeSessionHash_ = identity.sessionHash;
    activeSessionDeadlineMs_ = nowMs + durationMs;
    activeSessionTracked_ = true;
    stagedNextBoot_ = false;
    stagedNextBootIdentity_ = HilArmedFaultIdentity{};
    return HilFaultResult::Ok;
}

HilFaultResult HilFaultRuntimeOwner::abortSession(const HilArmedFaultIdentity& identity) noexcept {
    HilArmedFaultIdentity armed{};
    if (!armedIdentity(identity.faultId, armed) || !identitiesEqual(identity, armed)) {
        return HilFaultResult::WrongSession;
    }
    invalidateStagedNextBoot();
    const HilFaultResult result = controller_.endSession(identity.caseId, identity.sessionHash);
    if (result == HilFaultResult::Ok) {
        bindings_.fill(HilArmedFaultIdentity{});
        invalidateStagedNextBoot();
        clearTrackedSession();
    }
    return result;
}

bool HilFaultRuntimeOwner::deadlineReached(const uint32_t nowMs, const uint32_t deadlineMs) noexcept {
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

void HilFaultRuntimeOwner::invalidateStagedNextBoot() noexcept {
    if (stagedNextBoot_ && clearNextBoot_ != nullptr) {
        clearNextBoot_(stageContext_);
    }
    stagedNextBoot_ = false;
    stagedNextBootIdentity_ = HilArmedFaultIdentity{};
}

void HilFaultRuntimeOwner::clearTrackedSession() noexcept {
    activeSessionCase_ = HilCaseId::Invalid;
    activeSessionHash_ = HilSessionTokenHash{};
    activeSessionDeadlineMs_ = 0;
    activeSessionTracked_ = false;
}

void HilFaultRuntimeOwner::service(const uint32_t nowMs) noexcept {
    controller_.service(nowMs);
    if (!activeSessionTracked_ || !deadlineReached(nowMs, activeSessionDeadlineMs_)) {
        return;
    }
    invalidateStagedNextBoot();
    bindings_.fill(HilArmedFaultIdentity{});
    const HilFaultResult result = controller_.endSession(activeSessionCase_, activeSessionHash_);
    if (result != HilFaultResult::Busy) {
        clearTrackedSession();
    }
}

bool HilFaultRuntimeOwner::armedIdentity(const HilFaultId faultId, HilArmedFaultIdentity& identity) const noexcept {
    const size_t index = static_cast<size_t>(faultId);
    if (index >= bindings_.size()) {
        return false;
    }
    const HilArmedFaultIdentity& candidate = bindings_[index];
    if (candidate.caseId == HilCaseId::Invalid || candidate.faultId != faultId || candidate.armSequence == 0) {
        return false;
    }
    identity = candidate;
    return true;
}

HilFaultController& HilFaultRuntimeOwner::controller() noexcept {
    return controller_;
}

const HilFaultController& HilFaultRuntimeOwner::controller() const noexcept {
    return controller_;
}

HilFaultRuntimeOwner& hilFaultRuntimeOwner() noexcept {
    return gHilFaultRuntimeOwner;
}

#endif // V1SIMPLE_HIL_FAULT_CONTROL
