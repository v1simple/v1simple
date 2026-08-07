#include "qualification_serial_module.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage_manager.h"

namespace {
constexpr char kPrefixResp[] = "QRESP ";
constexpr char kPrefixEvent[] = "QEVENT ";
constexpr char kPrefixErr[] = "QERR ";
constexpr char kPrefixFile[] = "QFILE ";
constexpr char kPrefixChunk[] = "QCHUNK ";
constexpr char kPrefixEnd[] = "QEND ";

void copyString(char* dst, size_t dstLen, const char* src) {
    if (!dst || dstLen == 0) {
        return;
    }
    if (dst == src) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dstLen, "%s", src);
}
} // namespace

void QualificationSerialModule::begin(Stream* io, const Providers& providers) {
    io_ = io;
    providers_ = providers;
    state_ = State::Idle;
    suite_ = Suite::Core;
    mode_ = Mode::Current;
    durationMs_ = 0;
    startedAtMs_ = 0;
    finalizingAtMs_ = 0;
    finalSnapshotQueued_ = false;
    finalizedOk_ = false;
    commandLen_ = 0;
    commandBuf_[0] = '\0';
    csvPath_[0] = '\0';
    lastError_[0] = '\0';
    closeExport();
}

void QualificationSerialModule::process() {
    if (!io_) {
        return;
    }

    serviceRun();
    serviceExport();
    serviceInput();
}

uint32_t QualificationSerialModule::nowMs() const {
    if (providers_.nowMs) {
        return providers_.nowMs(providers_.ctx);
    }
    return millis();
}

const char* QualificationSerialModule::stateName() const {
    switch (state_) {
    case State::Idle:
        return "idle";
    case State::Running:
        return "running";
    case State::Finalizing:
        return "finalizing";
    case State::Done:
        return "done";
    case State::Error:
        return "error";
    }
    return "unknown";
}

const char* QualificationSerialModule::suiteName() const {
    switch (suite_) {
    case Suite::Core:
        return "core";
    case Suite::Display:
        return "display";
    }
    return "unknown";
}

const char* QualificationSerialModule::modeName() const {
    switch (mode_) {
    case Mode::Current:
        return "current";
    case Mode::Proxy:
        return "proxy";
    case Mode::Obd:
        return "obd";
    case Mode::V1Only:
        return "v1";
    }
    return "unknown";
}

void QualificationSerialModule::serviceInput() {
    while (io_->available() > 0) {
        const int raw = io_->read();
        if (raw < 0) {
            break;
        }
        const char c = static_cast<char>(raw);
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            commandBuf_[commandLen_] = '\0';
            char* line = trim(commandBuf_);
            if (line[0] != '\0') {
                handleCommand(line);
            }
            commandLen_ = 0;
            commandBuf_[0] = '\0';
            continue;
        }
        if (commandLen_ + 1 < sizeof(commandBuf_)) {
            commandBuf_[commandLen_++] = c;
        } else {
            commandLen_ = 0;
            commandBuf_[0] = '\0';
            sendErrorLine("command_too_long");
        }
    }
}

void QualificationSerialModule::serviceRun() {
    if (state_ == State::Running) {
        if (durationMs_ == 0) {
            enterFinalizing("duration_zero");
            return;
        }
        const uint32_t now = nowMs();
        if (static_cast<uint32_t>(now - startedAtMs_) >= durationMs_) {
            enterFinalizing("duration_elapsed");
        }
        return;
    }

    if (state_ != State::Finalizing) {
        return;
    }

    const uint32_t now = nowMs();
    const bool drained = providers_.tryDrainPerf && providers_.tryDrainPerf(providers_.ctx);
    if (drained) {
        finishRun(true, "done");
        return;
    }

    if (static_cast<uint32_t>(now - finalizingAtMs_) >= kFinalizeTimeoutMs) {
        setError("finalize_timeout");
        finishRun(false, "finalize_timeout");
    }
}

void QualificationSerialModule::serviceExport() {
    if (!exportActive_ || !io_) {
        return;
    }

    if (!exportFile_) {
        closeExport();
        sendErrorLine("export_file_closed");
        return;
    }

    // Keep each export line below the ESP32-S3 HW CDC default 256-byte TX ring.
    // A 64-byte binary chunk becomes 128 hex chars; the extra headroom covers
    // the protocol prefix, sequence number, separator, and newline without
    // forcing Print::write() into its timeout-backed blocking path.
    const int writable = io_->availableForWrite();
    if (writable < kExportWritableHeadroom) {
        return;
    }

    uint8_t bytes[kExportChunkBytes];
    size_t bytesRead = 0;
    bool eof = false;
    {
        if (!providers_.sdMutex) {
            closeExport();
            sendErrorLine("sd_mutex_unavailable");
            return;
        }
        StorageManager::SDTryLock lock(providers_.sdMutex(providers_.ctx));
        if (!lock) {
            return;
        }
        bytesRead = exportFile_.read(bytes, sizeof(bytes));
        if (bytesRead == 0) {
            exportFile_.close();
            exportActive_ = false;
            eof = true;
        }
    }

    if (eof) {
        const uint32_t finalCrc = exportCrc_ ^ 0xFFFFFFFFUL;
        io_->print(kPrefixEnd);
        io_->print("{\"ok\":true,\"bytes\":");
        io_->print(exportBytes_);
        io_->print(",\"chunks\":");
        io_->print(exportChunks_);
        io_->print(",\"crc32\":\"");
        char crcBuf[9];
        snprintf(crcBuf, sizeof(crcBuf), "%08lX", static_cast<unsigned long>(finalCrc));
        io_->print(crcBuf);
        io_->println("\"}");
        exportCrc_ = 0xFFFFFFFFUL;
        exportBytes_ = 0;
        exportSize_ = 0;
        exportChunks_ = 0;
        return;
    }

    exportCrc_ = crc32Update(exportCrc_, bytes, bytesRead);
    exportBytes_ += static_cast<uint32_t>(bytesRead);

    static constexpr char kHex[] = "0123456789ABCDEF";
    char hex[(kExportChunkBytes * 2) + 1];
    for (size_t i = 0; i < bytesRead; ++i) {
        hex[i * 2] = kHex[(bytes[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = kHex[bytes[i] & 0x0F];
    }
    hex[bytesRead * 2] = '\0';

    io_->print(kPrefixChunk);
    io_->print(exportChunks_);
    io_->print(' ');
    io_->println(hex);
    exportChunks_++;
}

void QualificationSerialModule::handleCommand(char* line) {
    if (strncmp(line, "QSTART", 6) == 0 && (line[6] == '\0' || isspace(static_cast<unsigned char>(line[6])))) {
        handleStart(trim(line + 6));
        return;
    }
    if (strcmp(line, "QSTATUS") == 0) {
        handleStatus();
        return;
    }
    if (strncmp(line, "QGETCSV", 7) == 0 && (line[7] == '\0' || isspace(static_cast<unsigned char>(line[7])))) {
        handleGetCsv(trim(line + 7));
        return;
    }
    if (strncmp(line, "QBSC08", 6) == 0 && (line[6] == '\0' || isspace(static_cast<unsigned char>(line[6])))) {
        handleBsc08(trim(line + 6));
        return;
    }
    if (strcmp(line, "QABORT") == 0) {
        handleAbort();
        return;
    }
    sendErrorLine("unknown_command");
}

void QualificationSerialModule::handleStart(char* args) {
    Suite suite = Suite::Core;
    uint32_t durationSeconds = 0;
    Mode mode = Mode::Current;
    if (!parseStartArgs(args, suite, durationSeconds, mode)) {
        sendErrorLine("invalid_start_args");
        return;
    }

    const bool ok = startRun(suite, durationSeconds, mode);
    if (!ok) {
        sendErrorLine(lastError_[0] ? lastError_ : "start_failed");
        return;
    }
    sendStatusLine(kPrefixResp, true, "started");
}

void QualificationSerialModule::handleStatus() {
    sendStatusLine(kPrefixResp, state_ != State::Error, nullptr);
}

void QualificationSerialModule::handleGetCsv(char* args) {
    if (exportActive_) {
        sendErrorLine("export_already_active");
        return;
    }
    if (state_ == State::Running || state_ == State::Finalizing) {
        sendErrorLine("run_active");
        return;
    }

    const char* requested = (args && args[0] != '\0') ? args : csvPath_;
    if (!requested || requested[0] == '\0') {
        sendErrorLine("no_csv_path");
        return;
    }
    if (!openExport(requested)) {
        sendErrorLine(lastError_[0] ? lastError_ : "export_open_failed");
    }
}

void QualificationSerialModule::handleBsc08(char* args) {
    if (!validNonce(args)) {
        sendErrorLine("invalid_bsc08_nonce");
        return;
    }
    if (!providers_.tryProxyEpochSnapshot) {
        sendErrorLine("bsc08_provider_missing");
        return;
    }

    BleProxyEpochQualificationSnapshot snapshot;
    if (!providers_.tryProxyEpochSnapshot(snapshot, providers_.ctx)) {
        io_->print("QBSC08 {\"schema\":1,\"nonce\":\"");
        io_->print(args);
        io_->println("\",\"status\":\"busy\"}");
        return;
    }

    const BleProxyEpochObserverSnapshot& epoch = snapshot.epoch;
    io_->print("QBSC08 {\"schema\":1,\"nonce\":\"");
    io_->print(args);
    io_->print("\",\"status\":\"ready\",\"epoch\":");
    io_->print(epoch.currentEpoch);
    io_->print(",\"gateEpoch\":");
    io_->print(epoch.admittedEpoch);
    io_->print(",\"active\":");
    io_->print(epoch.activeCallbacks);
    io_->print(",\"callbackEntries\":[");
    io_->print(epoch.v1ToProxyCallbackEntries);
    io_->print(',');
    io_->print(epoch.proxyToV1CallbackEntries);
    io_->print("],\"admissions\":[");
    io_->print(epoch.v1ToProxyAdmissions);
    io_->print(',');
    io_->print(epoch.proxyToV1Admissions);
    io_->print("],\"staleRejects\":[");
    io_->print(epoch.staleV1ToProxyRejections);
    io_->print(',');
    io_->print(epoch.staleProxyToV1Rejections);
    io_->print("],\"lifecycle\":[");
    io_->print(epoch.allocationCount);
    io_->print(',');
    io_->print(epoch.disableCount);
    io_->print(',');
    io_->print(epoch.releaseCount);
    io_->print(',');
    io_->print(epoch.reenableCount);
    io_->print("],\"activeOverlap\":");
    io_->print(epoch.activeCallbackObserved ? "true" : "false");
    io_->print(",\"releaseOpportunity\":");
    io_->print(epoch.releaseOpportunityObserved ? "true" : "false");
    io_->print(",\"oldForwarded\":");
    io_->print(epoch.oldEpochForwarded ? "true" : "false");
    io_->print(",\"proxyQueue\":[");
    io_->print(snapshot.proxyQueueHead);
    io_->print(',');
    io_->print(snapshot.proxyQueueTail);
    io_->print(',');
    io_->print(snapshot.proxyQueueCount);
    io_->print(',');
    io_->print(snapshot.proxyQueueCapacity);
    io_->print("],\"phoneQueue\":[");
    io_->print(snapshot.phoneQueueHead);
    io_->print(',');
    io_->print(snapshot.phoneQueueTail);
    io_->print(',');
    io_->print(snapshot.phoneQueueCount);
    io_->print(',');
    io_->print(snapshot.phoneQueueCapacity);
    io_->print("],\"heap\":[");
    io_->print(snapshot.freeInternalBytes);
    io_->print(',');
    io_->print(snapshot.largestInternalBlockBytes);
    io_->println("]}");
}

void QualificationSerialModule::handleAbort() {
    if (providers_.cancelDisplayPreview) {
        providers_.cancelDisplayPreview(providers_.ctx);
    }
    if (providers_.setSdCapturePaused) {
        providers_.setSdCapturePaused(true, providers_.ctx);
    }
    state_ = State::Error;
    setError("aborted");
    sendStatusLine(kPrefixResp, false, "aborted");
    clearQualificationModeOverride();
}

bool QualificationSerialModule::parseStartArgs(char* args, Suite& suite, uint32_t& durationSeconds, Mode& mode) const {
    char* cursor = trim(args);
    if (!cursor || cursor[0] == '\0') {
        return false;
    }

    char* suiteToken = strtok(cursor, " \t");
    char* durationToken = strtok(nullptr, " \t");
    char* modeToken = strtok(nullptr, " \t");
    char* extraToken = strtok(nullptr, " \t");
    if (!suiteToken || !durationToken) {
        return false;
    }
    if (extraToken) {
        return false;
    }

    if (strcmp(suiteToken, "core") == 0) {
        suite = Suite::Core;
    } else if (strcmp(suiteToken, "display") == 0) {
        suite = Suite::Display;
    } else {
        return false;
    }

    char* end = nullptr;
    const unsigned long parsed = strtoul(durationToken, &end, 10);
    if (!end || *end != '\0' || parsed == 0 || parsed > 86400UL) {
        return false;
    }
    durationSeconds = static_cast<uint32_t>(parsed);

    if (!modeToken || strcmp(modeToken, "current") == 0) {
        mode = Mode::Current;
    } else if (strcmp(modeToken, "proxy") == 0) {
        mode = Mode::Proxy;
    } else if (strcmp(modeToken, "obd") == 0) {
        mode = Mode::Obd;
    } else if (strcmp(modeToken, "v1") == 0 || strcmp(modeToken, "v1_only") == 0 || strcmp(modeToken, "v1-only") == 0) {
        mode = Mode::V1Only;
    } else {
        return false;
    }

    return true;
}

bool QualificationSerialModule::startRun(Suite suite, uint32_t durationSeconds, Mode mode) {
    if (state_ == State::Running || state_ == State::Finalizing || exportActive_) {
        setError("busy");
        return false;
    }
    if (!providers_.isPerfEnabled || !providers_.isPerfEnabled(providers_.ctx)) {
        setError("perf_sd_disabled");
        return false;
    }
    if (!providers_.perfCsvPath || !providers_.startPerfSession || !providers_.enqueueSnapshotNow ||
        !providers_.tryDrainPerf || !providers_.setSdCapturePaused) {
        setError("providers_missing");
        return false;
    }
    if (suite == Suite::Display && !providers_.startDisplayPreview) {
        setError("display_provider_missing");
        return false;
    }

    closeExport();
    providers_.setSdCapturePaused(true, providers_.ctx);
    if (!providers_.tryDrainPerf(providers_.ctx)) {
        setError("perf_sd_busy_retry");
        return false;
    }
    if (mode != Mode::Current) {
        if (!providers_.applyQualificationMode) {
            setError("mode_provider_missing");
            return false;
        }
        if (!providers_.applyQualificationMode(static_cast<uint8_t>(mode), providers_.ctx)) {
            setError("mode_apply_failed");
            return false;
        }
    }

    suite_ = suite;
    mode_ = mode;
    durationMs_ = durationSeconds * 1000UL;
    startedAtMs_ = nowMs();
    finalizingAtMs_ = 0;
    finalSnapshotQueued_ = false;
    finalizedOk_ = false;
    lastError_[0] = '\0';
    copyString(csvPath_, sizeof(csvPath_), providers_.perfCsvPath(providers_.ctx));

    providers_.startPerfSession(providers_.ctx);
    providers_.setSdCapturePaused(false, providers_.ctx);
    (void)providers_.enqueueSnapshotNow(providers_.ctx);

    if (suite_ == Suite::Display) {
        providers_.startDisplayPreview(durationMs_, providers_.ctx);
    }

    state_ = State::Running;
    return true;
}

void QualificationSerialModule::enterFinalizing(const char* reason) {
    if (state_ != State::Running) {
        return;
    }
    if (suite_ == Suite::Display && providers_.cancelDisplayPreview) {
        providers_.cancelDisplayPreview(providers_.ctx);
    }
    if (providers_.enqueueSnapshotNow) {
        finalSnapshotQueued_ = providers_.enqueueSnapshotNow(providers_.ctx);
    }
    if (providers_.setSdCapturePaused) {
        providers_.setSdCapturePaused(true, providers_.ctx);
    }
    finalizingAtMs_ = nowMs();
    state_ = State::Finalizing;
    sendStatusLine(kPrefixEvent, true, reason ? reason : "finalizing");
}

void QualificationSerialModule::finishRun(bool ok, const char* message) {
    finalizedOk_ = ok;
    state_ = ok ? State::Done : State::Error;
    sendStatusLine(kPrefixEvent, ok, message);
    clearQualificationModeOverride();
}

void QualificationSerialModule::clearQualificationModeOverride() {
    if (mode_ != Mode::Current && providers_.clearQualificationMode) {
        providers_.clearQualificationMode(providers_.ctx);
    }
    mode_ = Mode::Current;
}

bool QualificationSerialModule::openExport(const char* path) {
    if (!providers_.isStorageReady || !providers_.isSDCard || !providers_.filesystem || !providers_.sdMutex) {
        setError("storage_providers_missing");
        return false;
    }
    if (!providers_.isStorageReady(providers_.ctx) || !providers_.isSDCard(providers_.ctx)) {
        setError("sd_unavailable");
        return false;
    }
    if (!path || path[0] != '/' || strstr(path, "..") != nullptr) {
        setError("invalid_path");
        return false;
    }

    fs::FS* fs = providers_.filesystem(providers_.ctx);
    if (!fs) {
        setError("fs_unavailable");
        return false;
    }

    StorageManager::SDTryLock lock(providers_.sdMutex(providers_.ctx));
    if (!lock) {
        setError("sd_busy");
        return false;
    }

    exportFile_ = fs->open(path, FILE_READ);
    if (!exportFile_) {
        setError("file_not_found");
        return false;
    }
    exportActive_ = true;
    exportCrc_ = 0xFFFFFFFFUL;
    exportBytes_ = 0;
    exportSize_ = static_cast<uint32_t>(exportFile_.size());
    exportChunks_ = 0;

    io_->print(kPrefixFile);
    io_->print("{\"ok\":true,\"path\":");
    printJsonString(path);
    io_->print(",\"size\":");
    io_->print(exportSize_);
    io_->println("}");
    return true;
}

void QualificationSerialModule::closeExport() {
    if (exportFile_) {
        exportFile_.close();
    }
    exportActive_ = false;
    exportCrc_ = 0xFFFFFFFFUL;
    exportBytes_ = 0;
    exportSize_ = 0;
    exportChunks_ = 0;
}

void QualificationSerialModule::setError(const char* message) {
    copyString(lastError_, sizeof(lastError_), message ? message : "error");
}

void QualificationSerialModule::sendStatusLine(const char* prefix, bool ok, const char* message) {
    if (!io_) {
        return;
    }
    io_->print(prefix ? prefix : kPrefixResp);
    io_->print("{\"ok\":");
    io_->print(ok ? "true" : "false");
    io_->print(",\"state\":");
    printJsonString(stateName());
    io_->print(",\"suite\":");
    printJsonString(suiteName());
    io_->print(",\"mode\":");
    printJsonString(modeName());
    io_->print(",\"durationMs\":");
    io_->print(durationMs_);
    io_->print(",\"elapsedMs\":");
    const uint32_t elapsed = startedAtMs_ == 0 ? 0 : static_cast<uint32_t>(nowMs() - startedAtMs_);
    io_->print(elapsed);
    io_->print(",\"csvPath\":");
    printJsonString(csvPath_);
    io_->print(",\"finalSnapshotQueued\":");
    io_->print(finalSnapshotQueued_ ? "true" : "false");
    io_->print(",\"finalized\":");
    io_->print(finalizedOk_ ? "true" : "false");
    if (message && message[0] != '\0') {
        io_->print(",\"message\":");
        printJsonString(message);
    }
    if (lastError_[0] != '\0') {
        io_->print(",\"error\":");
        printJsonString(lastError_);
    }
    io_->println("}");
}

void QualificationSerialModule::sendErrorLine(const char* message) {
    setError(message);
    sendStatusLine(kPrefixErr, false, message);
}

void QualificationSerialModule::printJsonString(const char* value) {
    io_->print('"');
    const char* s = value ? value : "";
    while (*s) {
        const char c = *s++;
        if (c == '"' || c == '\\') {
            io_->print('\\');
            io_->print(c);
        } else if (static_cast<unsigned char>(c) < 0x20) {
            io_->print(' ');
        } else {
            io_->print(c);
        }
    }
    io_->print('"');
}

uint32_t QualificationSerialModule::crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
        }
    }
    return crc;
}

bool QualificationSerialModule::validNonce(const char* value) {
    if (!value || strlen(value) != 32) {
        return false;
    }
    for (size_t index = 0; index < 32; ++index) {
        const char c = value[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

char* QualificationSerialModule::trim(char* text) {
    if (!text) {
        return text;
    }
    while (*text && isspace(static_cast<unsigned char>(*text))) {
        ++text;
    }
    char* end = text + strlen(text);
    while (end > text && isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    *end = '\0';
    return text;
}
