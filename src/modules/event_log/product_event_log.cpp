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
    const WriterState initialState = writerState_.load(std::memory_order_acquire);
    if (initialState == WriterState::RUNNING || initialState == WriterState::STOP_REQUESTED ||
        initialState == WriterState::CLOSING) {
        return false;
    }
    enabled_.store(false, std::memory_order_release);
    accepting_.store(false, std::memory_order_release);
    writerState_.store(WriterState::STOPPED, std::memory_order_release);
    writerExitClean_.store(false, std::memory_order_release);
    stopFailureRecorded_.store(false, std::memory_order_release);
    retentionExhausted_.store(false, std::memory_order_release);
    pendingGapCount_.store(0, std::memory_order_relaxed);
    pendingGapFirstMs_.store(0, std::memory_order_relaxed);
    pendingGapLastMs_.store(0, std::memory_order_relaxed);
    storage_ = &storage;
    bootId_ = bootId;
    eventFile_ = File();
    dirty_ = false;
    fileCreated_ = false;
    retainedBytes_ = 0;
    activeBytes_ = 0;
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
    return startWriterTask();
}

bool ProductEventLog::startWriterTask() {
    WriterState expected = WriterState::STOPPED;
    if (!writerState_.compare_exchange_strong(expected, WriterState::RUNNING, std::memory_order_acq_rel)) {
        return false;
    }
    writerExitClean_.store(false, std::memory_order_release);
    const BaseType_t created = xTaskCreatePinnedToCore(&ProductEventLog::writerTaskEntry, "ProductEvents",
                                                       kWriterStackBytes, this, 1, &writerTask_, 0);
    if (created != pdPASS) {
        writerTask_ = nullptr;
        writerState_.store(WriterState::FAILED, std::memory_order_release);
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

bool ProductEventLog::stopAndFlush(uint32_t nowMs, uint32_t timeoutMs) {
    if (accepting_.load(std::memory_order_acquire)) {
        builder_.closeActive(nowMs);
    }
    accepting_.store(false, std::memory_order_release);

    WriterState state = writerState_.load(std::memory_order_acquire);
    if (state == WriterState::RUNNING) {
        (void)writerState_.compare_exchange_strong(state, WriterState::STOP_REQUESTED, std::memory_order_acq_rel);
    }
    if (writerTask_) {
        xTaskNotifyGive(writerTask_);
    }

    const uint32_t started = millis();
    while (static_cast<uint32_t>(millis() - started) < timeoutMs) {
        state = writerState_.load(std::memory_order_acquire);
        if (state == WriterState::STOPPED || state == WriterState::FAILED) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    state = writerState_.load(std::memory_order_acquire);
    const bool clean = state == WriterState::STOPPED && writerExitClean_.load(std::memory_order_acquire);
    if (!clean) {
        recordStopFailure();
    }
    return clean;
}

bool ProductEventLog::resumeAfterAbortedShutdown(uint32_t timeoutMs) {
    if (!enabled_.load(std::memory_order_acquire)) {
        return false;
    }

    const uint32_t started = millis();
    for (;;) {
        WriterState state = writerState_.load(std::memory_order_acquire);
        if (state == WriterState::RUNNING) {
            stopFailureRecorded_.store(false, std::memory_order_release);
            accepting_.store(true, std::memory_order_release);
            return true;
        }
        if (state == WriterState::STOP_REQUESTED) {
            if (writerState_.compare_exchange_strong(state, WriterState::RUNNING, std::memory_order_acq_rel)) {
                stopFailureRecorded_.store(false, std::memory_order_release);
                accepting_.store(true, std::memory_order_release);
                if (writerTask_) {
                    xTaskNotifyGive(writerTask_);
                }
                return true;
            }
            continue;
        }
        if (state == WriterState::STOPPED) {
            if (!startWriterTask()) {
                return false;
            }
            stopFailureRecorded_.store(false, std::memory_order_release);
            accepting_.store(true, std::memory_order_release);
            return true;
        }
        if (state == WriterState::FAILED || static_cast<uint32_t>(millis() - started) >= timeoutMs) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

bool ProductEventLog::writerStopped() const {
    const WriterState state = writerState_.load(std::memory_order_acquire);
    return state == WriterState::STOPPED || state == WriterState::FAILED;
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
                if (!retentionExhausted_.load(std::memory_order_acquire)) {
                    HealthCounters::recordEventDrop(1 + static_cast<uint32_t>(uxQueueMessagesWaiting(queue_)));
                }
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

        if (writerState_.load(std::memory_order_acquire) == WriterState::STOP_REQUESTED &&
            uxQueueMessagesWaiting(queue_) == 0) {
            WriterState expected = WriterState::STOP_REQUESTED;
            if (writerState_.compare_exchange_strong(expected, WriterState::CLOSING, std::memory_order_acq_rel)) {
                break;
            }
        }
        taskYIELD();
    }

    if (failed) {
        writerState_.store(WriterState::CLOSING, std::memory_order_release);
    }
    bool cleanExit = !failed;
    if (eventFile_) {
        if (!failed && !flushIfDue(true)) {
            cleanExit = false;
        }
        StorageManager::SDLockTimed lock(storage_->getSDMutex(), kWriterLockTimeoutMs);
        if (lock) {
            eventFile_.close();
            dirty_ = false;
        } else {
            cleanExit = false;
        }
    }
    accepting_.store(false, std::memory_order_release);
    writerExitClean_.store(cleanExit, std::memory_order_release);
    writerTask_ = nullptr;
    writerState_.store(failed ? WriterState::FAILED : WriterState::STOPPED, std::memory_order_release);
}

bool ProductEventLog::writeEvent(const ProductEvent& event) {
    if (!storage_ || !storage_->getFilesystem()) {
        return false;
    }
    StorageManager::SDLockTimed lock(storage_->getSDMutex(), kWriterLockTimeoutMs);
    if (!lock || !ensureFileOpenLocked()) {
        if (retentionExhausted_.load(std::memory_order_acquire)) {
            recordRetentionExhaustion(1 + static_cast<uint32_t>(uxQueueMessagesWaiting(queue_)));
        }
        return false;
    }

    ProductEvent gap{};
    const bool hasGap = takeGap(gap);
    size_t eventBytes = 0;
    size_t gapBytes = 0;
    if (!serializedEventBytes(event, eventBytes) || (hasGap && !serializedEventBytes(gap, gapBytes))) {
        return false;
    }
    if (retainedBytes_ > kMaxTotalBytes || activeBytes_ > kMaxTotalBytes - retainedBytes_ ||
        gapBytes > kMaxTotalBytes - retainedBytes_ - activeBytes_ ||
        eventBytes > kMaxTotalBytes - retainedBytes_ - activeBytes_ - gapBytes) {
        recordRetentionExhaustion(1 + static_cast<uint32_t>(uxQueueMessagesWaiting(queue_)));
        return false;
    }

    if (hasGap && !writeRowsLocked(gap)) {
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

bool ProductEventLog::serializedEventBytes(const ProductEvent& event, size_t& bytes) const {
    bytes = 0;
    char row[256];
    const size_t count = productEventRowCount(event);
    for (size_t item = 0; item < count; ++item) {
        const size_t length = serializeProductEventRow(event, item, row, sizeof(row));
        if (length == 0 || length > SIZE_MAX - bytes) {
            return false;
        }
        bytes += length;
    }
    return true;
}

bool ProductEventLog::writeRowsLocked(const ProductEvent& event) {
    char row[256];
    const size_t count = productEventRowCount(event);
    for (size_t item = 0; item < count; ++item) {
        const size_t length = serializeProductEventRow(event, item, row, sizeof(row));
        if (length == 0) {
            return false;
        }
        const size_t written = eventFile_.write(reinterpret_cast<const uint8_t*>(row), length);
        activeBytes_ += written;
        if (written != length) {
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
        if (!fileCreated_) {
            return false;
        }
        eventFile_ = fs.open(eventPath_, FILE_APPEND);
        return static_cast<bool>(eventFile_);
    }
    if (!fs.exists("/events") && !fs.mkdir("/events")) {
        return false;
    }
    const size_t headerLength = sizeof(kProductEventSchemaHeader) - 1;
    if (retainedBytes_ > kMaxTotalBytes || headerLength > kMaxTotalBytes - retainedBytes_) {
        recordRetentionExhaustion(0);
        return false;
    }
    eventFile_ = fs.open(eventPath_, FILE_WRITE);
    if (!eventFile_) {
        return false;
    }
    const size_t written = eventFile_.write(reinterpret_cast<const uint8_t*>(kProductEventSchemaHeader), headerLength);
    activeBytes_ = written;
    if (written != headerLength) {
        eventFile_.close();
        return false;
    }
    fileCreated_ = true;
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
    accepting_.store(false, std::memory_order_release);
}

void ProductEventLog::recordStopFailure() {
    if (!stopFailureRecorded_.exchange(true, std::memory_order_acq_rel)) {
        HealthCounters::recordEventShutdownFailure();
#ifndef UNIT_TEST
        Serial.println("[ProductEvents] ERROR: writer did not terminate cleanly; storage handoff blocked");
#endif
    }
}

void ProductEventLog::recordRetentionExhaustion(uint32_t dropped) {
    if (!retentionExhausted_.exchange(true, std::memory_order_acq_rel)) {
        HealthCounters::recordEventRetentionExhaustion();
#ifndef UNIT_TEST
        Serial.println("[ProductEvents] retention budget exhausted; admission closed");
#endif
    }
    if (dropped > 0) {
        HealthCounters::recordEventDrop(dropped);
    }
    disableWriter();
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
    retainedBytes_ = totalBytes;
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
        if (!retentionExhausted_.load(std::memory_order_acquire)) {
            HealthCounters::recordEventDrop(1 + static_cast<uint32_t>(uxQueueMessagesWaiting(queue_)));
        }
        xQueueReset(queue_);
    }
    return ok;
}
#endif
