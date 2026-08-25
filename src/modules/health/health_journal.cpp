#include "health_journal.h"

#include <Arduino.h>
#include <FS.h>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "storage_manager.h"

namespace {

constexpr uint32_t kHealthLockTimeoutMs = 500;
constexpr char kSchemaHeader[] = "# health_schema=1\n";

void copyToken(char* out, size_t capacity, const char* value) {
    if (!out || capacity == 0) {
        return;
    }
    size_t write = 0;
    for (const char* p = value ? value : "UNKNOWN"; *p != '\0' && write + 1 < capacity; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        out[write++] = (std::isalnum(c) || c == '_' || c == '-' || c == '.') ? static_cast<char>(c) : '_';
    }
    out[write] = '\0';
}

} // namespace

bool HealthJournal::begin(StorageManager& storage, uint32_t bootId, const char* imageId, const char* resetReason,
                          bool previousClean, bool panicEvidencePresent) {
    storage_ = &storage;
    bootId_ = bootId;
    enabled_ = false;
    readyWritten_ = false;
    endWritten_ = false;

    if (bootId == 0 || !storage.isReady() || !storage.isSDCard() || !storage.getFilesystem()) {
        return false;
    }

    fs::FS& fs = *storage.getFilesystem();
    StorageManager::SDLockTimed lock(storage.getSDMutex(), kHealthLockTimeoutMs);
    if (!lock) {
        return false;
    }

    bool needsHeader = true;
    if (fs.exists(kPath)) {
        File existing = fs.open(kPath, FILE_READ);
        if (!existing) {
            return false;
        }
        const size_t size = existing.size();
        needsHeader = size == 0;
        existing.close();
        if (size >= kMaxBytes) {
            if (fs.exists(kPreviousPath)) {
                fs.remove(kPreviousPath);
            }
            if (!fs.rename(kPath, kPreviousPath)) {
                return false;
            }
            needsHeader = true;
        }
    }

    File file = fs.open(kPath, FILE_APPEND);
    if (!file) {
        return false;
    }
    if (needsHeader && file.write(reinterpret_cast<const uint8_t*>(kSchemaHeader), sizeof(kSchemaHeader) - 1) !=
                           sizeof(kSchemaHeader) - 1) {
        file.close();
        return false;
    }

    char image[80];
    char reset[32];
    copyToken(image, sizeof(image), imageId);
    copyToken(reset, sizeof(reset), resetReason);
    char line[256];
    const int length = std::snprintf(line, sizeof(line),
                                     "BOOT,boot=%lu,image=%s,reset=%s,previous=%s,panic=%s\n",
                                     static_cast<unsigned long>(bootId), image, reset,
                                     previousClean ? "CLEAN" : "UNCLEAN",
                                     panicEvidencePresent ? "PRESENT" : "NONE");
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(line) ||
        file.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(length)) !=
            static_cast<size_t>(length)) {
        file.close();
        return false;
    }
    file.flush();
    file.close();
    enabled_ = true;
    return true;
}

void HealthJournal::ready(uint32_t nowMs) {
    if (!enabled_ || readyWritten_) {
        return;
    }
    char line[96];
    const int length = std::snprintf(line, sizeof(line), "READY,boot=%lu,ms=%lu\n",
                                     static_cast<unsigned long>(bootId_), static_cast<unsigned long>(nowMs));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(line) ||
        !appendLine(line, static_cast<size_t>(length))) {
        disable();
        return;
    }
    readyWritten_ = true;
}

void HealthJournal::end(uint32_t nowMs) {
    if (!enabled_ || endWritten_) {
        return;
    }
    char line[256];
    const int length =
        std::snprintf(line, sizeof(line),
                      "END,boot=%lu,ms=%lu,result=CLEAN,input_drop=%lu,event_drop=%lu,"
                      "event_shutdown_fail=%lu,event_retention_full=%lu\n",
                      static_cast<unsigned long>(bootId_), static_cast<unsigned long>(nowMs),
                      static_cast<unsigned long>(HealthCounters::inputDrops()),
                      static_cast<unsigned long>(HealthCounters::eventDrops()),
                      static_cast<unsigned long>(HealthCounters::eventShutdownFailures()),
                      static_cast<unsigned long>(HealthCounters::eventRetentionExhaustions()));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(line) ||
        !appendLine(line, static_cast<size_t>(length))) {
        disable();
        return;
    }
    endWritten_ = true;
}

bool HealthJournal::appendLine(const char* line, size_t length) {
    if (!storage_ || !storage_->getFilesystem()) {
        return false;
    }
    StorageManager::SDLockTimed lock(storage_->getSDMutex(), kHealthLockTimeoutMs);
    if (!lock) {
        return false;
    }
    File file = storage_->getFilesystem()->open(kPath, FILE_APPEND);
    if (!file) {
        return false;
    }
    const bool ok = file.write(reinterpret_cast<const uint8_t*>(line), length) == length;
    if (ok) {
        file.flush();
    }
    file.close();
    return ok;
}

void HealthJournal::disable() {
    enabled_ = false;
#ifndef UNIT_TEST
    Serial.println("[Health] SD journal disabled for this boot");
#endif
}
