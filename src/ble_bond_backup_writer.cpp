#include "ble_bond_backup_writer.h"

#include "ble_bond_backup_store.h"
#include "psram_freertos_alloc.h"
#include "storage_manager.h"

#include <Arduino.h>

extern "C" {
#include "nimble/nimble/host/include/host/ble_store.h"
#include "nimble/nimble/host/include/host/ble_sm.h"
}

#include <array>
#include <atomic>
#include <cstring>
#include <new>
#include <type_traits>

namespace {

constexpr const char* kBleBondBackupPath = "/v1simple_ble_bonds.bin";
constexpr UBaseType_t kBondBackupQueueDepth = 1;
constexpr uint32_t kBondBackupWriterStackSize = 4096;
constexpr UBaseType_t kBondBackupWriterPriority = 1;
constexpr uint32_t kBondBackupRetryDelayMs = 1000;
constexpr uint32_t kBootBackupWaitTimeoutMs = 1500;
constexpr uint32_t kBootBackupPollMs = 5;

struct BondCollector {
    std::array<ble_store_value_sec, kMaxBleBondEntries> entries = {};
    size_t count = 0;
};

struct BondBackupSnapshot {
    BondCollector ourSecs;
    BondCollector peerSecs;
    uint32_t sequence = 0;

    int totalCount() const { return static_cast<int>(ourSecs.count + peerSecs.count); }
};

static_assert(std::is_trivially_destructible<BondBackupSnapshot>::value,
              "bond snapshots must be safe to release from the writer");
static_assert(sizeof(BondBackupSnapshot) <= 2048, "bond snapshot outgrew its bounded PSRAM allocation budget");

struct BondBackupWriterState {
    QueueHandle_t queue = nullptr;
    PsramQueueAllocation queueAllocation = {};
    bool queueInPsram = false;
    std::atomic<bool> writerActive{false};
    std::atomic<bool> shutdownRequested{false};
    std::atomic<uint32_t> enqueuedSnapshots{0};
    std::atomic<uint32_t> coalescedSnapshots{0};
    std::atomic<uint32_t> droppedSnapshots{0};
    std::atomic<uint32_t> successfulWrites{0};
    std::atomic<uint32_t> writeFailures{0};
    std::atomic<uint32_t> retryRequeues{0};
    std::atomic<uint32_t> nextSequence{0};
    std::atomic<uint32_t> lastSuccessfulSequence{0};
    std::atomic<uint32_t> writerStackMinFreeBytes{UINT32_MAX};
};

BondBackupWriterState gBondBackupWriterState;

bool shouldLogCount(uint32_t count) {
    return count == 1 || (count != 0 && (count & (count - 1)) == 0);
}

void recordBondBackupWriterStackMinFreeBytes(uint32_t observedBytes) {
    uint32_t current = gBondBackupWriterState.writerStackMinFreeBytes.load(std::memory_order_relaxed);
    while (observedBytes < current &&
           !gBondBackupWriterState.writerStackMinFreeBytes.compare_exchange_weak(
               current, observedBytes, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

void sampleBondBackupWriterStack() {
    recordBondBackupWriterStackMinFreeBytes(static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)));
}

BondBackupSnapshot* allocateBondBackupSnapshot() {
    void* memory = heap_caps_malloc(sizeof(BondBackupSnapshot), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!memory) {
        memory = heap_caps_malloc(sizeof(BondBackupSnapshot), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    }
    if (!memory) {
        return nullptr;
    }
    return new (memory) BondBackupSnapshot{};
}

void releaseBondBackupSnapshot(BondBackupSnapshot* snapshot) {
    if (!snapshot) {
        return;
    }
    snapshot->~BondBackupSnapshot();
    heap_caps_free(snapshot);
}

int bondCollectCallback(int objType, union ble_store_value* value, void* cookie) {
    (void)objType;
    auto* collector = static_cast<BondCollector*>(cookie);
    if (collector && value && collector->count < collector->entries.size()) {
        memcpy(&collector->entries[collector->count], &value->sec, sizeof(ble_store_value_sec));
        ++collector->count;
    }
    return 0;
}

void collectCurrentBondSnapshot(BondBackupSnapshot& snapshot) {
    ble_store_iterate(BLE_STORE_OBJ_TYPE_OUR_SEC, bondCollectCallback, &snapshot.ourSecs);
    ble_store_iterate(BLE_STORE_OBJ_TYPE_PEER_SEC, bondCollectCallback, &snapshot.peerSecs);
}

int writeBondBackupSnapshot(fs::FS& sdFs, const BondBackupSnapshot& snapshot) {
    const String tmpPath = String(kBleBondBackupPath) + ".tmp";
    File file = sdFs.open(tmpPath.c_str(), "w");
    if (!file) {
        Serial.println("[BLE] WARN: Failed to open bond backup tmp file");
        return -1;
    }

    BondBackupHeader header = {};
    memcpy(header.magic, kBleBondMagic, sizeof(header.magic));
    header.ourSecCount = static_cast<uint32_t>(snapshot.ourSecs.count);
    header.peerSecCount = static_cast<uint32_t>(snapshot.peerSecs.count);

    bool ok = file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header);
    if (ok && snapshot.ourSecs.count > 0) {
        const size_t bytes = snapshot.ourSecs.count * sizeof(ble_store_value_sec);
        ok = file.write(reinterpret_cast<const uint8_t*>(snapshot.ourSecs.entries.data()), bytes) == bytes;
    }
    if (ok && snapshot.peerSecs.count > 0) {
        const size_t bytes = snapshot.peerSecs.count * sizeof(ble_store_value_sec);
        ok = file.write(reinterpret_cast<const uint8_t*>(snapshot.peerSecs.entries.data()), bytes) == bytes;
    }
    file.flush();
    file.close();

    if (!ok) {
        sdFs.remove(tmpPath.c_str());
        Serial.println("[BLE] WARN: Bond backup write incomplete");
        return -1;
    }

    if (!StorageManager::promoteTempFileWithRollback(sdFs, tmpPath.c_str(), kBleBondBackupPath)) {
        Serial.println("[BLE] WARN: Bond backup rename failed");
        return -1;
    }

    const int total = snapshot.totalCount();
    Serial.printf("[BLE] Backed up %d bond(s) to SD (%u our, %u peer)\n", total,
                  static_cast<unsigned>(snapshot.ourSecs.count), static_cast<unsigned>(snapshot.peerSecs.count));
    return total;
}

int writeBondBackupSnapshotWithSdLock(const BondBackupSnapshot& snapshot) {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return -1;
    }

    fs::FS* sdFs = storageManager.getFilesystem();
    if (!sdFs) {
        return -1;
    }

    // This function is called only by the Core-0 writer or the explicit
    // pre-runtime fresh-flash migration path.
    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
    if (!sdLock) {
        return -1;
    }

    return writeBondBackupSnapshot(*sdFs, snapshot);
}

void requeueOrReleaseFailedSnapshot(BondBackupSnapshot* snapshot) {
    if (gBondBackupWriterState.queue && xQueueSend(gBondBackupWriterState.queue, &snapshot, 0) == pdTRUE) {
        gBondBackupWriterState.retryRequeues.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // A full queue already contains a newer snapshot, so it safely supersedes
    // this failed one. A missing queue can only occur during test teardown.
    releaseBondBackupSnapshot(snapshot);
}

bool processBondBackupSnapshot(BondBackupSnapshot* snapshot) {
    if (!snapshot) {
        return false;
    }

    if (writeBondBackupSnapshotWithSdLock(*snapshot) >= 0) {
        gBondBackupWriterState.successfulWrites.fetch_add(1, std::memory_order_relaxed);
        gBondBackupWriterState.lastSuccessfulSequence.store(snapshot->sequence, std::memory_order_release);
        releaseBondBackupSnapshot(snapshot);
        return true;
    }

    const uint32_t failures = gBondBackupWriterState.writeFailures.fetch_add(1, std::memory_order_relaxed) + 1;
    requeueOrReleaseFailedSnapshot(snapshot);
    if (shouldLogCount(failures)) {
        Serial.printf("[BLE] WARN: Core-0 bond backup failed; queued for retry (failures=%lu)\n",
                      static_cast<unsigned long>(failures));
    }
    return false;
}

void bondBackupWriterTaskEntry(void*) {
    sampleBondBackupWriterStack();
    while (true) {
        if (gBondBackupWriterState.shutdownRequested.load(std::memory_order_acquire) &&
            uxQueueMessagesWaiting(gBondBackupWriterState.queue) == 0) {
            break;
        }

        BondBackupSnapshot* snapshot = nullptr;
        if (xQueueReceive(gBondBackupWriterState.queue, &snapshot, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }

        const bool ok = processBondBackupSnapshot(snapshot);
        // uxTaskGetStackHighWaterMark retains the task's lifetime minimum, so
        // sampling after each write captures the deepest filesystem path while
        // keeping the periodic reporter independent of the live task handle.
        sampleBondBackupWriterStack();
        if (!ok) {
            if (gBondBackupWriterState.shutdownRequested.load(std::memory_order_acquire)) {
                // Power-off is already bounded by the shutdown caller. Do not
                // keep a failed retry alive after that deadline.
                BondBackupSnapshot* discarded = nullptr;
                if (xQueueReceive(gBondBackupWriterState.queue, &discarded, 0) == pdTRUE) {
                    releaseBondBackupSnapshot(discarded);
                }
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(kBondBackupRetryDelayMs));
        }
        taskYIELD();
    }

    sampleBondBackupWriterStack();
    gBondBackupWriterState.writerActive.store(false, std::memory_order_release);
    vTaskDeleteWithCaps(nullptr);
}

bool ensureBondBackupWriterReady() {
    if (gBondBackupWriterState.shutdownRequested.load(std::memory_order_acquire)) {
        return false;
    }

    if (!gBondBackupWriterState.queue) {
        gBondBackupWriterState.queue =
            createQueuePreferPsram(kBondBackupQueueDepth, sizeof(BondBackupSnapshot*),
                                   gBondBackupWriterState.queueAllocation, &gBondBackupWriterState.queueInPsram);
        if (!gBondBackupWriterState.queue) {
            Serial.println("[BLE] ERROR: Failed to create bond backup queue");
            return false;
        }
    }

    bool expectedInactive = false;
    if (gBondBackupWriterState.writerActive.compare_exchange_strong(expectedInactive, true,
                                                                    std::memory_order_acq_rel)) {
        TaskHandle_t createdTask = nullptr;
        const BaseType_t rc =
            createTaskPinnedToCoreInternalStack(bondBackupWriterTaskEntry, "BleBondBackup", kBondBackupWriterStackSize,
                                                nullptr, kBondBackupWriterPriority, &createdTask, 0);
        if (rc != pdPASS) {
            gBondBackupWriterState.writerActive.store(false, std::memory_order_release);
            Serial.println("[BLE] ERROR: Failed to create Core-0 bond backup writer");
            return false;
        }
    }

    return true;
}

bool enqueueLatestSnapshot(BondBackupSnapshot* snapshot) {
    if (xQueueSend(gBondBackupWriterState.queue, &snapshot, 0) == pdTRUE) {
        return true;
    }

    BondBackupSnapshot* displaced = nullptr;
    const bool replaced = xQueueReceive(gBondBackupWriterState.queue, &displaced, 0) == pdTRUE;
    if (replaced) {
        releaseBondBackupSnapshot(displaced);
    }
    if (xQueueSend(gBondBackupWriterState.queue, &snapshot, 0) == pdTRUE) {
        if (replaced) {
            const uint32_t coalesced =
                gBondBackupWriterState.coalescedSnapshots.fetch_add(1, std::memory_order_relaxed) + 1;
            if (shouldLogCount(coalesced)) {
                Serial.printf("[BLE] Bond backup queue coalesced older snapshot (count=%lu)\n",
                              static_cast<unsigned long>(coalesced));
            }
        }
        return true;
    }

    const uint32_t dropped = gBondBackupWriterState.droppedSnapshots.fetch_add(1, std::memory_order_relaxed) + 1;
    if (shouldLogCount(dropped)) {
        Serial.printf("[BLE] WARN: Bond backup snapshot dropped (count=%lu)\n", static_cast<unsigned long>(dropped));
    }
    releaseBondBackupSnapshot(snapshot);
    return false;
}

int enqueueCurrentBondSnapshotImpl(uint32_t* sequenceOut) {
    // NimBLE owns the live bond table, so capture it on the caller while the
    // deletion/pairing event is current. No SD mutex or filesystem operation is
    // performed on this path.
    BondBackupSnapshot* snapshot = allocateBondBackupSnapshot();
    if (!snapshot) {
        const uint32_t dropped = gBondBackupWriterState.droppedSnapshots.fetch_add(1, std::memory_order_relaxed) + 1;
        if (shouldLogCount(dropped)) {
            Serial.printf("[BLE] WARN: Bond backup snapshot allocation failed (count=%lu)\n",
                          static_cast<unsigned long>(dropped));
        }
        return -1;
    }
    collectCurrentBondSnapshot(*snapshot);
    const int totalCount = snapshot->totalCount();
    snapshot->sequence = gBondBackupWriterState.nextSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    if (snapshot->sequence == 0) {
        snapshot->sequence = gBondBackupWriterState.nextSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    const uint32_t snapshotSequence = snapshot->sequence;

    if (!ensureBondBackupWriterReady()) {
        releaseBondBackupSnapshot(snapshot);
        gBondBackupWriterState.droppedSnapshots.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    // enqueueLatestSnapshot owns the allocation on both success and failure.
    if (!enqueueLatestSnapshot(snapshot)) {
        return -1;
    }

    gBondBackupWriterState.enqueuedSnapshots.fetch_add(1, std::memory_order_relaxed);
    if (sequenceOut) {
        *sequenceOut = snapshotSequence;
    }
    return totalCount;
}

} // namespace

int enqueueCurrentBleBondBackupSnapshot() {
    return enqueueCurrentBondSnapshotImpl(nullptr);
}

int backupCurrentBleBondsViaCore0AtBoot() {
    uint32_t targetSequence = 0;
    const int bondCount = enqueueCurrentBondSnapshotImpl(&targetSequence);
    if (bondCount < 0) {
        return -1;
    }
    const uint32_t startMs = millis();
    while (gBondBackupWriterState.lastSuccessfulSequence.load(std::memory_order_acquire) != targetSequence) {
        if (static_cast<uint32_t>(millis() - startMs) >= kBootBackupWaitTimeoutMs) {
            Serial.println("[BLE] WARN: Timed out waiting for Core-0 fresh-flash bond backup");
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(kBootBackupPollMs));
    }
    return bondCount;
}

BleBondBackupWriterStats bleBondBackupWriterStats() {
    BleBondBackupWriterStats stats;
    stats.enqueuedSnapshots = gBondBackupWriterState.enqueuedSnapshots.load(std::memory_order_relaxed);
    stats.coalescedSnapshots = gBondBackupWriterState.coalescedSnapshots.load(std::memory_order_relaxed);
    stats.droppedSnapshots = gBondBackupWriterState.droppedSnapshots.load(std::memory_order_relaxed);
    stats.successfulWrites = gBondBackupWriterState.successfulWrites.load(std::memory_order_relaxed);
    stats.writeFailures = gBondBackupWriterState.writeFailures.load(std::memory_order_relaxed);
    stats.retryRequeues = gBondBackupWriterState.retryRequeues.load(std::memory_order_relaxed);
    stats.writerStackMinFreeBytes = gBondBackupWriterState.writerStackMinFreeBytes.load(std::memory_order_relaxed);
    return stats;
}

void shutdownBleBondBackupWriter(uint32_t timeoutMs) {
    gBondBackupWriterState.shutdownRequested.store(true, std::memory_order_release);
    if (!gBondBackupWriterState.writerActive.load(std::memory_order_acquire)) {
        return;
    }

    const uint32_t startMs = millis();
    while (gBondBackupWriterState.writerActive.load(std::memory_order_acquire)) {
        if (static_cast<uint32_t>(millis() - startMs) > timeoutMs) {
            Serial.println("[BLE] WARN: Timed out draining Core-0 bond backup writer");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

#ifdef UNIT_TEST
void resetBleBondBackupWriterForTest() {
    if (gBondBackupWriterState.queue) {
        BondBackupSnapshot* snapshot = nullptr;
        while (xQueueReceive(gBondBackupWriterState.queue, &snapshot, 0) == pdTRUE) {
            releaseBondBackupSnapshot(snapshot);
            snapshot = nullptr;
        }
        vQueueDelete(gBondBackupWriterState.queue);
    }
    gBondBackupWriterState.queue = nullptr;
    gBondBackupWriterState.writerActive.store(false, std::memory_order_relaxed);
    if (gBondBackupWriterState.queueAllocation.queueBuffer) {
        heap_caps_free(gBondBackupWriterState.queueAllocation.queueBuffer);
        gBondBackupWriterState.queueAllocation.queueBuffer = nullptr;
    }
    gBondBackupWriterState.queueInPsram = false;
    gBondBackupWriterState.shutdownRequested.store(false, std::memory_order_relaxed);
    gBondBackupWriterState.enqueuedSnapshots.store(0, std::memory_order_relaxed);
    gBondBackupWriterState.coalescedSnapshots.store(0, std::memory_order_relaxed);
    gBondBackupWriterState.droppedSnapshots.store(0, std::memory_order_relaxed);
    gBondBackupWriterState.successfulWrites.store(0, std::memory_order_relaxed);
    gBondBackupWriterState.writeFailures.store(0, std::memory_order_relaxed);
    gBondBackupWriterState.retryRequeues.store(0, std::memory_order_relaxed);
    gBondBackupWriterState.nextSequence.store(0, std::memory_order_relaxed);
    gBondBackupWriterState.lastSuccessfulSequence.store(0, std::memory_order_relaxed);
    gBondBackupWriterState.writerStackMinFreeBytes.store(UINT32_MAX, std::memory_order_relaxed);
}

bool runBleBondBackupWriterOnceForTest() {
    if (!gBondBackupWriterState.queue) {
        return false;
    }

    BondBackupSnapshot* snapshot = nullptr;
    if (xQueueReceive(gBondBackupWriterState.queue, &snapshot, 0) != pdTRUE) {
        return false;
    }
    return processBondBackupSnapshot(snapshot);
}

size_t bleBondBackupQueueDepthForTest() {
    if (!gBondBackupWriterState.queue) {
        return 0;
    }
    return static_cast<size_t>(uxQueueMessagesWaiting(gBondBackupWriterState.queue));
}

size_t bleBondBackupSnapshotSizeForTest() {
    return sizeof(BondBackupSnapshot);
}

void recordBleBondBackupWriterStackSampleForTest(uint32_t freeBytes) {
    recordBondBackupWriterStackMinFreeBytes(freeBytes);
}
#endif
