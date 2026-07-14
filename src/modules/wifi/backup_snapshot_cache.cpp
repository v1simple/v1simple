#include "backup_snapshot_cache.h"

#include <esp_heap_caps.h>

#include <Arduino.h>

#include "json_stream_response.h"
#include "wifi_json_document.h"

namespace BackupApiService {

namespace {

constexpr size_t BACKUP_CACHE_GROWTH_QUANTUM = 256u;

size_t roundUpBackupCacheCapacity(size_t required) {
    return ((required + BACKUP_CACHE_GROWTH_QUANTUM - 1u) / BACKUP_CACHE_GROWTH_QUANTUM) * BACKUP_CACHE_GROWTH_QUANTUM;
}

bool hasMatchingSnapshot(const BackupSnapshotCache& cache, uint32_t settingsRevision, uint32_t profileRevision) {
    return cache.valid && cache.data != nullptr && cache.length > 0 && cache.settingsRevision == settingsRevision &&
           cache.profileRevision == profileRevision;
}

bool allocateBackupSnapshotBuffer(size_t required, char*& newData, size_t& newCapacity, bool& inPsram) {
    newCapacity = roundUpBackupCacheCapacity(required);
    newData = static_cast<char*>(heap_caps_malloc(newCapacity, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    inPsram = true;

    if (newData == nullptr) {
        Serial.printf("[BackupApi] Cache PSRAM alloc failed; falling back to internal (%lu bytes)\n",
                      static_cast<unsigned long>(newCapacity));
        newData = static_cast<char*>(heap_caps_malloc(newCapacity, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        inPsram = false;
    }

    if (newData == nullptr) {
        Serial.printf("[BackupApi] Cache alloc failed (%lu bytes); streaming uncached snapshot\n",
                      static_cast<unsigned long>(newCapacity));
        return false;
    }

    return true;
}

} // namespace

bool sendCachedBackupSnapshot(WebServer& server, BackupSnapshotCache& cache, uint32_t settingsRevision,
                              uint32_t profileRevision, BackupSnapshotBuildFn buildSnapshot, void* buildCtx,
                              uint32_t (*millisFn)(void* ctx), void* millisCtx) {
    if (hasMatchingSnapshot(cache, settingsRevision, profileRevision)) {
        sendSerializedJson(server, cache.data, cache.length);
        return true;
    }

    WifiJson::Document doc;
    const uint32_t snapshotMs = millisFn ? millisFn(millisCtx) : static_cast<uint32_t>(millis());
    if (buildSnapshot) {
        buildSnapshot(doc, snapshotMs, buildCtx);
    }

    const size_t required = measureJson(doc) + 1u;
    char* targetData = cache.data;
    size_t targetCapacity = cache.capacity;
    bool targetInPsram = cache.inPsram;
    bool usingNewAllocation = false;

    if (targetData == nullptr || targetCapacity < required) {
        char* newData = nullptr;
        size_t newCapacity = 0;
        bool newInPsram = false;
        if (!allocateBackupSnapshotBuffer(required, newData, newCapacity, newInPsram)) {
            sendJsonStream(server, doc);
            return false;
        }

        targetData = newData;
        targetCapacity = newCapacity;
        targetInPsram = newInPsram;
        usingNewAllocation = true;

        Serial.printf("[BackupApi] Cache grow %lu -> %lu bytes (%s)\n", static_cast<unsigned long>(cache.capacity),
                      static_cast<unsigned long>(targetCapacity), targetInPsram ? "psram" : "internal");
    }

    const size_t length = serializeJson(doc, targetData, targetCapacity);
    if (length == 0 || length >= targetCapacity) {
        if (usingNewAllocation) {
            heap_caps_free(targetData);
        }
        sendJsonStream(server, doc);
        return false;
    }
    targetData[length] = '\0';

    if (usingNewAllocation) {
        if (cache.data != nullptr) {
            heap_caps_free(cache.data);
        }
        cache.data = targetData;
        cache.capacity = targetCapacity;
        cache.inPsram = targetInPsram;
    }

    cache.length = length;
    cache.snapshotMs = snapshotMs;
    cache.settingsRevision = settingsRevision;
    cache.profileRevision = profileRevision;
    cache.valid = true;

    sendSerializedJson(server, cache.data, cache.length);
    return true;
}

void releaseBackupSnapshotCache(BackupSnapshotCache& cache) {
    if (cache.data != nullptr) {
        heap_caps_free(cache.data);
    }

    cache.data = nullptr;
    cache.capacity = 0;
    cache.length = 0;
    cache.inPsram = false;
    cache.snapshotMs = 0;
    cache.settingsRevision = 0;
    cache.profileRevision = 0;
    cache.valid = false;
}

} // namespace BackupApiService
