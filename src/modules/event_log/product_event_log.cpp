#include "product_event_log.h"

#include <cstdio>
#include <cstring>

#include "modules/health/health_journal.h"
#include "product_event_csv.h"
#include "storage_manager.h"

ProductEventLog productEventLog;

namespace {

constexpr uint32_t kWriterLockTimeoutMs = 250;
constexpr uint32_t kFlushIntervalMs = 5000;
constexpr TickType_t kWriterPollTicks = pdMS_TO_TICKS(100);

struct RetainedEventFile {
    char path[64];
    uint32_t bootId;
    size_t size;
};

bool parseEventFilename(const char* name, uint32_t& bootId) {
    if (!name) {
        return false;
    }
    const char* base = std::strrchr(name, '/');
    base = base ? base + 1 : name;
    constexpr char prefix[] = "events_";
    constexpr char suffix[] = ".csv";
    if (std::strncmp(base, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char* digits = base + sizeof(prefix) - 1;
    if (*digits == '\0') {
        return false;
    }
    uint32_t value = 0;
    const char* p = digits;
    for (; *p >= '0' && *p <= '9'; ++p) {
        const uint32_t digit = static_cast<uint32_t>(*p - '0');
        if (value > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    if (std::strcmp(p, suffix) != 0 || value == 0) {
        return false;
    }
    bootId = value;
    return true;
}

size_t oldestIndex(const RetainedEventFile* files, size_t count) {
    size_t oldest = 0;
    for (size_t i = 1; i < count; ++i) {
        if (files[i].bootId < files[oldest].bootId) {
            oldest = i;
        }
    }
    return oldest;
}

} // namespace

bool ProductEventLog::begin(uint32_t bootId, StorageManager& storage) {
    enabled_.store(false, std::memory_order_release);
    accepting_.store(false, std::memory_order_release);
    shutdownRequested_.store(false, std::memory_order_release);
    taskRunning_.store(false, std::memory_order_release);
    pendingGapCount_.store(0, std::memory_order_relaxed);
    pendingGapFirstMs_.store(0, std::memory_order_relaxed);
    pendingGapLastMs_.store(0, std::memory_order_relaxed);
    storage_ = &storage;
    bootId_ = bootId;
    eventFile_ = File();
    dirty_ = false;
    lastFlushMs_ = 0;
    gapSequence_ = 0;

    if (bootId == 0 || !storage.isReady() || !storage.isSDCard() || !storage.getFilesystem()) {
        return false;
    }
    const int pathLength = std::snprintf(eventPath_, sizeof(eventPath_), "/events/events_%lu.csv",
                                         static_cast<unsigned long>(bootId));
    if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(eventPath_) || !pruneRetention()) {
        return false;
    }

    queue_ = xQueueCreateStatic(kQueueCapacity, sizeof(ProductEvent), queueStorage_, &queueControl_);
    if (!queue_) {
        return false;
    }

    builder_.begin(&ProductEventLog::emitFromBuilder, this);
    enabled_.store(true, std::memory_order_release);
    accepting_.store(true, std::memory_order_release);
    taskRunning_.store(true, std::memory_order_release);
    const BaseType_t created = xTaskCreatePinnedToCore(&ProductEventLog::writerTaskEntry, "ProductEvents",
                                                       kWriterStackBytes, this, 1, &writerTask_, 0);
    if (created != pdPASS) {
        taskRunning_.store(false, std::memory_order_release);
        enabled_.store(false, std::memory_order_release);
        accepting_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void ProductEventLog::observeV1Table(const AlertData* alerts, size_t count, uint8_t priorityIndex, uint32_t nowMs) {
    if (accepting_.load(std::memory_order_acquire) && (alerts || count == 0)) {
        builder_.observeV1Table(alerts, count, priorityIndex, nowMs);
    }
}

void ProductEventLog::observeV1Link(bool connected, uint32_t nowMs) {
    if (accepting_.load(std::memory_order_acquire)) {
        builder_.observeV1Link(connected, nowMs);
    }
}

void ProductEventLog::observeAlp(const AlpProductObservation& observation, uint32_t nowMs) {
    if (accepting_.load(std::memory_order_acquire)) {
        builder_.observeAlp(observation, nowMs);
    }
}

void ProductEventLog::stopAndFlush(uint32_t nowMs, uint32_t timeoutMs) {
    if (!accepting_.load(std::memory_order_acquire) && !taskRunning_.load(std::memory_order_acquire)) {
        return;
    }
    if (accepting_.load(std::memory_order_acquire)) {
        builder_.closeActive(nowMs);
    }
    accepting_.store(false, std::memory_order_release);
    shutdownRequested_.store(true, std::memory_order_release);
    if (writerTask_) {
        xTaskNotifyGive(writerTask_);
    }

    const uint32_t started = millis();
    while (taskRunning_.load(std::memory_order_acquire) && static_cast<uint32_t>(millis() - started) < timeoutMs) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

bool ProductEventLog::emitFromBuilder(const ProductEvent& event, void* context) {
    return context && static_cast<ProductEventLog*>(context)->enqueue(event);
}

bool ProductEventLog::enqueue(const ProductEvent& event) {
    if (!enabled_.load(std::memory_order_acquire) || !accepting_.load(std::memory_order_acquire) || !queue_ ||
        xQueueSend(queue_, &event, 0) != pdTRUE) {
        noteDrop(event.ms);
        return false;
    }
    return true;
}

void ProductEventLog::noteDrop(uint32_t nowMs) {
    HealthCounters::recordEventDrop();
    uint32_t expected = 0;
    (void)pendingGapFirstMs_.compare_exchange_strong(expected, nowMs, std::memory_order_relaxed);
    pendingGapLastMs_.store(nowMs, std::memory_order_relaxed);
    pendingGapCount_.fetch_add(1, std::memory_order_release);
}

bool ProductEventLog::takeGap(ProductEvent& event) {
    const uint32_t lost = pendingGapCount_.exchange(0, std::memory_order_acq_rel);
    if (lost == 0) {
        return false;
    }
    event = ProductEvent{};
    event.ms = pendingGapLastMs_.exchange(0, std::memory_order_relaxed);
    event.source = ProductEventSource::SYS;
    event.kind = ProductEventKind::GAP;
    event.sequence = ++gapSequence_;
    event.data.gap.lost = lost;
    event.data.gap.firstMs = pendingGapFirstMs_.exchange(0, std::memory_order_relaxed);
    event.data.gap.lastMs = event.ms;
    return true;
}

void ProductEventLog::writerTaskEntry(void* context) {
    if (context) {
        static_cast<ProductEventLog*>(context)->writerLoop();
    }
    vTaskDelete(nullptr);
}

void ProductEventLog::writerLoop() {
    bool failed = false;
    while (enabled_.load(std::memory_order_acquire)) {
        ProductEvent event{};
        if (xQueueReceive(queue_, &event, kWriterPollTicks) == pdTRUE) {
            if (!writeEvent(event)) {
                disableWriter();
                HealthCounters::recordEventDrop(1 + static_cast<uint32_t>(uxQueueMessagesWaiting(queue_)));
                xQueueReset(queue_);
                failed = true;
                break;
            }
        } else if (!flushIfDue(false)) {
            disableWriter();
            HealthCounters::recordEventDrop(static_cast<uint32_t>(uxQueueMessagesWaiting(queue_)));
            xQueueReset(queue_);
            failed = true;
            break;
        }

        if (shutdownRequested_.load(std::memory_order_acquire) && uxQueueMessagesWaiting(queue_) == 0) {
            break;
        }
        taskYIELD();
    }

    if (eventFile_) {
        if (!failed) {
            (void)flushIfDue(true);
        }
        StorageManager::SDLockTimed lock(storage_->getSDMutex(), kWriterLockTimeoutMs);
        if (lock) {
            eventFile_.close();
        }
    }
    if (shutdownRequested_.load(std::memory_order_acquire)) {
        accepting_.store(false, std::memory_order_release);
    }
    taskRunning_.store(false, std::memory_order_release);
    writerTask_ = nullptr;
}

bool ProductEventLog::writeEvent(const ProductEvent& event) {
    if (!storage_ || !storage_->getFilesystem()) {
        return false;
    }
    StorageManager::SDLockTimed lock(storage_->getSDMutex(), kWriterLockTimeoutMs);
    if (!lock || !ensureFileOpenLocked()) {
        return false;
    }

    ProductEvent gap{};
    if (takeGap(gap) && !writeRowsLocked(gap)) {
        return false;
    }
    if (!writeRowsLocked(event)) {
        return false;
    }
    dirty_ = true;

    const bool endEvent = event.kind == ProductEventKind::END;
    if (endEvent || static_cast<uint32_t>(millis() - lastFlushMs_) >= kFlushIntervalMs) {
        eventFile_.flush();
        dirty_ = false;
        lastFlushMs_ = millis();
    }
    return true;
}

bool ProductEventLog::writeRowsLocked(const ProductEvent& event) {
    char row[256];
    const size_t count = productEventRowCount(event);
    for (size_t item = 0; item < count; ++item) {
        const size_t length = serializeProductEventRow(event, item, row, sizeof(row));
        if (length == 0 || eventFile_.write(reinterpret_cast<const uint8_t*>(row), length) != length) {
            return false;
        }
    }
    return true;
}

bool ProductEventLog::ensureFileOpenLocked() {
    if (eventFile_) {
        return true;
    }
    fs::FS& fs = *storage_->getFilesystem();
    if (fs.exists(eventPath_)) {
        return false;
    }
    if (!fs.exists("/events") && !fs.mkdir("/events")) {
        return false;
    }
    eventFile_ = fs.open(eventPath_, FILE_WRITE);
    if (!eventFile_) {
        return false;
    }
    const size_t headerLength = sizeof(kProductEventSchemaHeader) - 1;
    if (eventFile_.write(reinterpret_cast<const uint8_t*>(kProductEventSchemaHeader), headerLength) != headerLength) {
        eventFile_.close();
        return false;
    }
    dirty_ = true;
    lastFlushMs_ = millis();
    return true;
}

bool ProductEventLog::flushIfDue(bool force) {
    if (!eventFile_ || !dirty_) {
        return true;
    }
    if (!force && static_cast<uint32_t>(millis() - lastFlushMs_) < kFlushIntervalMs) {
        return true;
    }
    StorageManager::SDLockTimed lock(storage_->getSDMutex(), kWriterLockTimeoutMs);
    if (!lock) {
        return false;
    }
    eventFile_.flush();
    dirty_ = false;
    lastFlushMs_ = millis();
    return true;
}

void ProductEventLog::disableWriter() {
    enabled_.store(false, std::memory_order_release);
}

bool ProductEventLog::pruneRetention() {
    fs::FS& fs = *storage_->getFilesystem();
    StorageManager::SDLockTimed lock(storage_->getSDMutex(), kWriterLockTimeoutMs);
    if (!lock || !fs.exists("/events")) {
        return static_cast<bool>(lock);
    }

    File directory = fs.open("/events", FILE_READ);
    if (!directory || !directory.isDirectory()) {
        return false;
    }

    RetainedEventFile retained[kMaxFiles]{};
    size_t retainedCount = 0;
    File entry;
    while ((entry = directory.openNextFile())) {
        if (!entry.isDirectory()) {
            uint32_t fileBootId = 0;
            if (parseEventFilename(entry.name(), fileBootId)) {
                RetainedEventFile candidate{};
                const int length = std::snprintf(candidate.path, sizeof(candidate.path), "/events/%s", entry.name());
                if (length <= 0 || static_cast<size_t>(length) >= sizeof(candidate.path)) {
                    entry.close();
                    directory.close();
                    return false;
                }
                candidate.bootId = fileBootId;
                candidate.size = entry.size();
                // Reserve one of the bounded file slots for this boot's lazy
                // event file. A quiet boot never consumes the reserved slot.
                if (retainedCount < kMaxFiles - 1U) {
                    retained[retainedCount++] = candidate;
                } else {
                    const size_t oldest = oldestIndex(retained, retainedCount);
                    if (candidate.bootId > retained[oldest].bootId) {
                        if (!fs.remove(retained[oldest].path)) {
                            entry.close();
                            directory.close();
                            return false;
                        }
                        retained[oldest] = candidate;
                    } else if (!fs.remove(candidate.path)) {
                        entry.close();
                        directory.close();
                        return false;
                    }
                }
            }
        }
        entry.close();
    }
    directory.close();

    size_t totalBytes = 0;
    for (size_t i = 0; i < retainedCount; ++i) {
        totalBytes += retained[i].size;
    }
    while (retainedCount > 0 && totalBytes > kMaxTotalBytes) {
        const size_t oldest = oldestIndex(retained, retainedCount);
        if (!fs.remove(retained[oldest].path)) {
            return false;
        }
        totalBytes -= retained[oldest].size;
        retained[oldest] = retained[retainedCount - 1];
        --retainedCount;
    }
    return true;
}

#ifdef UNIT_TEST
bool ProductEventLog::drainOneForTest() {
    ProductEvent event{};
    if (!queue_ || xQueueReceive(queue_, &event, 0) != pdTRUE) {
        return false;
    }
    const bool ok = writeEvent(event);
    if (!ok) {
        disableWriter();
        HealthCounters::recordEventDrop(1 + static_cast<uint32_t>(uxQueueMessagesWaiting(queue_)));
        xQueueReset(queue_);
    }
    return ok;
}
#endif
