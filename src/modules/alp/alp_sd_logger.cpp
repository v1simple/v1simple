/**
 * ALP SD Logger — implementation.
 *
 * CSV format (schema v2): millis,utc,event,from_state,to_state,byte0,byte1,byte2,checksum,direction,gun,extra
 *
 * Main-loop callers enqueue rows without waiting; a low-priority writer task
 * owns SD open/write/close. Drops are expected and OK — we log what we can
 * without blocking the display pipeline.
 */

#include "alp_sd_logger.h"

#include <cstdio>
#include <cstring>

#include "../../modules/gps/gps_publishers.h"

#ifndef UNIT_TEST
#include <Arduino.h>
#include <FS.h>
#include "../../storage_manager.h"
#else
// Stubs for unit test builds

#endif

#include "alp_runtime_module.h"

// ── Global instance ──────────────────────────────────────────────────
AlpSdLogger alpSdLogger;

static constexpr const char* ALP_DIR_PATH = "/alp";
static constexpr const char* ALP_CSV_HEADER =
    "# alp_schema=2,utc_column=1\n"
    "millis,utc,event,from_state,to_state,byte0,byte1,byte2,checksum,direction,gun,extra\n";
#ifndef UNIT_TEST
static constexpr UBaseType_t ALP_SD_QUEUE_DEPTH = 32;
static constexpr uint32_t ALP_SD_WRITER_STACK_SIZE = 8192;
static constexpr UBaseType_t ALP_SD_WRITER_PRIORITY = 1;
#endif

namespace {

void formatHexByte(char* dest, size_t destSize, uint8_t value) {
    snprintf(dest, destSize, "%02X", value);
}

// Format UTC as ISO-8601 or empty string.
// dest must hold at least 25 bytes (YYYY-MM-DDTHH:MM:SS.sssZ + NUL).
void formatUtcField(char* dest, size_t destSize, GpsTimePublisher* timePub, uint32_t nowMs) {
    if (!timePub || destSize < 2) {
        if (destSize > 0)
            dest[0] = '\0';
        return;
    }
    uint64_t utcMs = 0;
    if (!timePub->readUtc(nowMs, utcMs) || utcMs == 0) {
        dest[0] = '\0';
        return;
    }
    const uint64_t totalSec = utcMs / 1000;
    const uint32_t ms = static_cast<uint32_t>(utcMs % 1000);
    uint32_t sec = static_cast<uint32_t>(totalSec % 60);
    uint32_t min = static_cast<uint32_t>((totalSec / 60) % 60);
    uint32_t hour = static_cast<uint32_t>((totalSec / 3600) % 24);
    uint32_t days = static_cast<uint32_t>(totalSec / 86400);
    uint32_t y = 1970;
    while (true) {
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        uint32_t diy = leap ? 366u : 365u;
        if (days < diy)
            break;
        days -= diy;
        y++;
    }
    static const uint8_t mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    uint32_t mo = 1;
    for (uint32_t m = 0; m < 12; m++) {
        uint32_t md = mdays[m] + ((m == 1 && leap) ? 1u : 0u);
        if (days < md) {
            mo = m + 1;
            break;
        }
        days -= md;
    }
    snprintf(dest, destSize, "%04lu-%02lu-%02luT%02lu:%02lu:%02lu.%03luZ", static_cast<unsigned long>(y),
             static_cast<unsigned long>(mo), static_cast<unsigned long>(days + 1), static_cast<unsigned long>(hour),
             static_cast<unsigned long>(min), static_cast<unsigned long>(sec), static_cast<unsigned long>(ms));
}

void formatCsvRow(char* dest, size_t destSize, uint32_t nowMs, const char* utcStr, const char* event,
                  const char* fromState, const char* toState, const char* byte0, const char* byte1, const char* byte2,
                  const char* checksum, const char* direction, const char* gun, const char* extra) {
    snprintf(dest, destSize, "%lu,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n", static_cast<unsigned long>(nowMs),
             utcStr ? utcStr : "", event ? event : "", fromState ? fromState : "", toState ? toState : "",
             byte0 ? byte0 : "", byte1 ? byte1 : "", byte2 ? byte2 : "", checksum ? checksum : "",
             direction ? direction : "", gun ? gun : "", extra ? extra : "");
}

} // namespace

// ── begin() ──────────────────────────────────────────────────────────

void AlpSdLogger::begin(bool enabled, bool sdReady, GpsTimePublisher* timePub) {
    enabled_ = enabled;
    sdReady_ = sdReady;
    timePub_ = timePub;
    dirReady_ = false;
    headerWritten_ = false;
    linesWritten_ = 0;
    dropCount_ = 0;

    if (!enabled || !sdReady) {
        return;
    }

#ifndef UNIT_TEST
    Serial.printf("[ALP_SD] begin: enabled=%d sdReady=%d\n", enabled, sdReady);
    if (!ensureAsyncWriter()) {
        Serial.println("[ALP_SD] WARN: async writer unavailable; ALP SD rows will drop");
    }
#endif
}

void AlpSdLogger::setBootId(uint32_t id, uint32_t bootToken) {
    // New format: /alp/alp_<bootId>-<token8>.csv (unique per boot).
    // Legacy fallback when no token: /alp/alp_<bootId>.csv.
    if (bootToken != 0) {
        snprintf(csvPathBuf_, sizeof(csvPathBuf_), "/alp/alp_%lu-%08lx.csv", static_cast<unsigned long>(id),
                 static_cast<unsigned long>(bootToken));
    } else {
        snprintf(csvPathBuf_, sizeof(csvPathBuf_), "/alp/alp_%lu.csv", static_cast<unsigned long>(id));
    }
    // Reset header state when boot ID changes
    headerWritten_ = false;

#ifndef UNIT_TEST
    if (enabled_ && sdReady_) {
        Serial.printf("[ALP_SD] path: %s\n", csvPathBuf_);
    }
#endif
}

void AlpSdLogger::setEnabled(bool enabled) {
    if (enabled_ == enabled)
        return;
    enabled_ = enabled;
    if (!enabled) {
#ifndef UNIT_TEST
        Serial.printf("[ALP_SD] disabled (wrote %lu lines, dropped %lu)\n", (unsigned long)linesWritten_,
                      (unsigned long)dropCount_);
#endif
    } else {
#ifndef UNIT_TEST
        Serial.printf("[ALP_SD] enabled\n");
        if (!ensureAsyncWriter()) {
            Serial.println("[ALP_SD] WARN: async writer unavailable; ALP SD rows will drop");
        }
#endif
    }
}

#ifndef UNIT_TEST
uint32_t AlpSdLogger::writerStackHighWaterBytes() const {
    TaskHandle_t task = writerTask_;
    if (!task) {
        return 0;
    }
    return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(task));
}
#endif

void AlpSdLogger::drainAndClose(uint32_t timeoutMs) {
#ifndef UNIT_TEST
    Serial.printf("[ALP_SD] Drain requested (car power mode, wrote %lu lines, queued %lu)\n",
                  (unsigned long)linesWritten_, queue_ ? (unsigned long)uxQueueMessagesWaiting(queue_) : 0UL);
    if (!queue_ || !writerTask_) {
        return;
    }
    const uint32_t startMs = millis();
    while (uxQueueMessagesWaiting(queue_) > 0) {
        if (millis() - startMs > timeoutMs) {
            Serial.printf("[ALP_SD] Drain timeout after %lums, %lu items remaining\n", (unsigned long)timeoutMs,
                          (unsigned long)uxQueueMessagesWaiting(queue_));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (uxQueueMessagesWaiting(queue_) == 0) {
        LogItem item{};
        item.shutdown = true;
        if (xQueueSend(queue_, &item, pdMS_TO_TICKS(10)) == pdTRUE) {
            while (writerTask_) {
                if (millis() - startMs > timeoutMs) {
                    Serial.println("[ALP_SD] Drain timeout waiting for writer shutdown");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
#endif
}

// ── Logging methods ──────────────────────────────────────────────────

void AlpSdLogger::logStateTransition(uint32_t nowMs, AlpState from, AlpState to, const char* direction) {
    if (!enabled_ || !sdReady_)
        return;

    char line[160];
    char utcBuf[26] = {};
    formatUtcField(utcBuf, sizeof(utcBuf), timePub_, nowMs);
    formatCsvRow(line, sizeof(line), nowMs, utcBuf, "STATE", alpStateName(from), alpStateName(to), "", "", "", "",
                 direction, "", "");
    appendLine(line);
}

void AlpSdLogger::logHeartbeatByte1(uint32_t nowMs, uint8_t prevByte1, uint8_t newByte1, AlpState currentState,
                                    const char* direction) {
    if (!enabled_ || !sdReady_)
        return;

    char prevByteBuf[3];
    char newByteBuf[3];
    char line[160];
    char utcBuf[26] = {};
    formatUtcField(utcBuf, sizeof(utcBuf), timePub_, nowMs);
    formatHexByte(prevByteBuf, sizeof(prevByteBuf), prevByte1);
    formatHexByte(newByteBuf, sizeof(newByteBuf), newByte1);
    formatCsvRow(line, sizeof(line), nowMs, utcBuf, "HB_BYTE1", alpStateName(currentState), "", prevByteBuf, newByteBuf,
                 "", "", direction, "", newByte1 == 0x01 ? "ALERT" : "IDLE");
    appendLine(line);
}

void AlpSdLogger::logGunIdentified(uint32_t nowMs, AlpGunType gun, uint8_t byte0, uint8_t byte1or2,
                                   bool isDetectTrigger, AlpState currentState, const char* direction) {
    if (!enabled_ || !sdReady_)
        return;

    const uint8_t frameByte1 = isDetectTrigger ? byte1or2 : 0x00;
    const uint8_t frameByte2 = isDetectTrigger ? 0x00 : byte1or2;
    char byte0Buf[3];
    char byte1Buf[3];
    char byte2Buf[3];
    char checksumBuf[3];
    char line[192];
    char utcBuf[26] = {};
    formatUtcField(utcBuf, sizeof(utcBuf), timePub_, nowMs);
    formatHexByte(byte0Buf, sizeof(byte0Buf), byte0);
    formatHexByte(byte1Buf, sizeof(byte1Buf), frameByte1);
    formatHexByte(byte2Buf, sizeof(byte2Buf), frameByte2);
    formatHexByte(checksumBuf, sizeof(checksumBuf), alpChecksum(byte0, frameByte1, frameByte2));
    formatCsvRow(line, sizeof(line), nowMs, utcBuf, "GUN_ID", alpStateName(currentState), "", byte0Buf, byte1Buf,
                 byte2Buf, checksumBuf, direction, alpGunName(gun), isDetectTrigger ? "detect" : "lid_deploy");
    appendLine(line);
}

void AlpSdLogger::logFrame(uint32_t nowMs, const char* frameType, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t cs,
                           AlpState currentState, const char* direction) {
    if (!enabled_ || !sdReady_)
        return;

    char byte0Buf[3];
    char byte1Buf[3];
    char byte2Buf[3];
    char checksumBuf[3];
    char line[160];
    char utcBuf[26] = {};
    formatUtcField(utcBuf, sizeof(utcBuf), timePub_, nowMs);
    formatHexByte(byte0Buf, sizeof(byte0Buf), b0);
    formatHexByte(byte1Buf, sizeof(byte1Buf), b1);
    formatHexByte(byte2Buf, sizeof(byte2Buf), b2);
    formatHexByte(checksumBuf, sizeof(checksumBuf), cs);
    formatCsvRow(line, sizeof(line), nowMs, utcBuf, frameType, alpStateName(currentState), "", byte0Buf, byte1Buf,
                 byte2Buf, checksumBuf, direction, "", "");
    appendLine(line);
}

void AlpSdLogger::logHeartbeat(uint32_t nowMs, uint8_t b0, uint8_t b1, uint8_t b2, AlpState currentState,
                               const char* direction) {
    if (!enabled_ || !sdReady_)
        return;

    // Rate-limit if configured (0 = log every heartbeat)
    if (HEARTBEAT_LOG_INTERVAL_MS > 0 && (nowMs - lastHeartbeatLogMs_) < HEARTBEAT_LOG_INTERVAL_MS) {
        return;
    }
    lastHeartbeatLogMs_ = nowMs;

    char byte0Buf[3];
    char byte1Buf[3];
    char byte2Buf[3];
    char checksumBuf[3];
    char line[160];
    char utcBuf[26] = {};
    formatUtcField(utcBuf, sizeof(utcBuf), timePub_, nowMs);
    formatHexByte(byte0Buf, sizeof(byte0Buf), b0);
    formatHexByte(byte1Buf, sizeof(byte1Buf), b1);
    formatHexByte(byte2Buf, sizeof(byte2Buf), b2);
    formatHexByte(checksumBuf, sizeof(checksumBuf), alpChecksum(b0, b1, b2));
    formatCsvRow(line, sizeof(line), nowMs, utcBuf, "HEARTBEAT", alpStateName(currentState), "", byte0Buf, byte1Buf,
                 byte2Buf, checksumBuf, direction, "", "");
    appendLine(line);
}

void AlpSdLogger::logEvent(uint32_t nowMs, const char* event, AlpState currentState, uint32_t extraValue,
                           const char* direction) {
    if (!enabled_ || !sdReady_)
        return;

    char extraBuf[16];
    char line[160];
    char utcBuf[26] = {};
    formatUtcField(utcBuf, sizeof(utcBuf), timePub_, nowMs);
    snprintf(extraBuf, sizeof(extraBuf), "%lu", static_cast<unsigned long>(extraValue));
    formatCsvRow(line, sizeof(line), nowMs, utcBuf, event, alpStateName(currentState), "", "", "", "", "", direction,
                 "", extraBuf);
    appendLine(line);
}

void AlpSdLogger::logSessionEvent(uint32_t nowMs, const char* event, AlpState currentState, AlpGunType gun,
                                  const char* extra, const char* direction) {
    if (!enabled_ || !sdReady_)
        return;

    char line[220];
    char utcBuf[26] = {};
    formatUtcField(utcBuf, sizeof(utcBuf), timePub_, nowMs);
    formatCsvRow(line, sizeof(line), nowMs, utcBuf, event, alpStateName(currentState), "", "", "", "", "", direction,
                 (gun != AlpGunType::UNKNOWN) ? alpGunName(gun) : "", extra ? extra : "");
    appendLine(line);
}

// ── Internal helpers ─────────────────────────────────────────────────

#ifndef UNIT_TEST
bool AlpSdLogger::ensureAsyncWriter() {
    if (!enabled_ || !sdReady_) {
        return false;
    }
    if (!queue_) {
        queue_ = createQueuePreferPsram(ALP_SD_QUEUE_DEPTH, sizeof(LogItem), queueAllocation_, &queueInPsram_);
        if (!queue_) {
            return false;
        }
        if (!queueInPsram_) {
            Serial.println("[ALP_SD] WARN: log queue using internal SRAM fallback");
        }
    }
    if (!writerTask_) {
        BaseType_t rc = createTaskPinnedToCoreInternalStack(writerTaskEntry, "AlpSdWriter", ALP_SD_WRITER_STACK_SIZE,
                                                            this, ALP_SD_WRITER_PRIORITY, &writerTask_, 0);
        if (rc != pdPASS) {
            return false;
        }
    }
    return true;
}

void AlpSdLogger::writerTaskEntry(void* param) {
    static_cast<AlpSdLogger*>(param)->writerTaskLoop();
}

void AlpSdLogger::writerTaskLoop() {
    while (true) {
        LogItem item{};
        if (xQueueReceive(queue_, &item, portMAX_DELAY) == pdTRUE) {
            if (item.shutdown) {
                break;
            }
            if (!writeLineBlocking(item.line)) {
                dropCount_++;
            }
            taskYIELD();
        }
    }
    writerTask_ = nullptr;
    vTaskDeleteWithCaps(nullptr);
}
#endif

bool AlpSdLogger::ensureDirectory() {
#ifndef UNIT_TEST
    if (dirReady_)
        return true;

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs)
        return false;

    // Try to create /alp directory
    if (!fs->exists(ALP_DIR_PATH)) {
        if (!fs->mkdir(ALP_DIR_PATH)) {
            return false;
        }
    }
    dirReady_ = true;
    return true;
#else
    dirReady_ = true;
    return true;
#endif
}

bool AlpSdLogger::ensureHeader() {
#ifndef UNIT_TEST
    if (headerWritten_)
        return true;
    if (csvPathBuf_[0] == '\0')
        return false;

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs)
        return false;

    // Check if file already exists and has content
    if (fs->exists(csvPathBuf_)) {
        File f = fs->open(csvPathBuf_, FILE_READ);
        if (f && f.size() > 0) {
            f.close();
            headerWritten_ = true;
            return true;
        }
        if (f)
            f.close();
    }

    // Write header to new file
    File f = fs->open(csvPathBuf_, FILE_APPEND, true);
    if (!f)
        return false;
    f.print(ALP_CSV_HEADER);
    f.close();
    headerWritten_ = true;
    return true;
#else
    headerWritten_ = true;
    return true;
#endif
}

#ifndef UNIT_TEST
bool AlpSdLogger::writeLineBlocking(const char* line) {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return false;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) {
        return false;
    }

    if (!ensureDirectory() || !ensureHeader()) {
        return false;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        return false;
    }

    File f = fs->open(csvPathBuf_, FILE_APPEND);
    if (!f) {
        return false;
    }

    f.print(line);
    f.close();
    linesWritten_++;
    return true;
}
#endif

bool AlpSdLogger::appendLine(const char* line) {
#ifndef UNIT_TEST
    // Main-loop callers must never stall on SD. Enqueue a bounded copy and let
    // the low-priority Core 0 writer do the open/write/close work. Drops are
    // expected for diagnostics when the queue is full or the writer is not up.
    if (!line || !enabled_ || !sdReady_) {
        dropCount_++;
        return false;
    }
    if (strlen(line) >= sizeof(LogItem::line)) {
        dropCount_++;
        return false;
    }
    if (!ensureAsyncWriter()) {
        dropCount_++;
        return false;
    }
    LogItem item{};
    item.shutdown = false;
    snprintf(item.line, sizeof(item.line), "%s", line);
    if (xQueueSend(queue_, &item, 0) != pdTRUE) {
        dropCount_++;
        return false;
    }
    return true;
#else
    snprintf(lastLineBuf_, sizeof(lastLineBuf_), "%s", line);
    linesWritten_++;
    return true;
#endif
}
