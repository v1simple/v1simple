#include "ble_bond_backup_store.h"

#include "storage_manager.h"
#include "ble_bond_backup_writer.h"

#include <Arduino.h>
#include <Preferences.h>
#include <nvs.h>

extern "C" {
#include "nimble/nimble/host/include/host/ble_store.h"
#include "nimble/nimble/host/include/host/ble_sm.h"
}

#include <array>
#include <atomic>
#include <cstring>

namespace {

constexpr const char* kBondRecoveryNamespace = "ble_state";
constexpr const char* kBondRecoveryKey = "bondRestore";
enum class RecoveryState { Ready, Pending, Unavailable };
std::atomic<RecoveryState> gBondRecoveryState{RecoveryState::Ready};
std::atomic<uint32_t> gBondRecoveryEpoch{0};
bool gRestoreAttempted = false;

RecoveryState readRecoveryState() {
    nvs_handle_t handle = 0;
    const esp_err_t opened = nvs_open(kBondRecoveryNamespace, NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return RecoveryState::Ready;
    if (opened != ESP_OK) return RecoveryState::Unavailable;
    uint32_t pending = 0;
    const esp_err_t read = nvs_get_u32(handle, kBondRecoveryKey, &pending);
    nvs_close(handle);
    if (read == ESP_ERR_NVS_NOT_FOUND) return RecoveryState::Ready;
    if (read != ESP_OK || pending > 1) return RecoveryState::Unavailable;
    return pending ? RecoveryState::Pending : RecoveryState::Ready;
}

bool storeRecoveryState(bool pending) {
    Preferences prefs;
    if (!prefs.begin(kBondRecoveryNamespace, false)) return false;
    const bool written = prefs.putUInt(kBondRecoveryKey, pending ? 1u : 0u) == sizeof(uint32_t);
    prefs.end();
    return written && readRecoveryState() == (pending ? RecoveryState::Pending : RecoveryState::Ready);
}

struct BondBackupContents {
    BondBackupHeader header = {};
    std::array<ble_store_value_sec, kMaxBleBondEntries> ourSecs = {};
    std::array<ble_store_value_sec, kMaxBleBondEntries> peerSecs = {};
};

BleBondBackupStatus readBleBondBackup(fs::FS& fs, const char* path, BondBackupContents& contents) {
    if (!path || path[0] == '\0') return BleBondBackupStatus::Unavailable;
    struct stat details {};
    const StoragePathStatus status = storagePathStatus(fs, path, &details);
    if (status == StoragePathStatus::Missing) return BleBondBackupStatus::Missing;
    if (status != StoragePathStatus::Present) return BleBondBackupStatus::Unavailable;
    if (!S_ISREG(details.st_mode) || details.st_size < 0) return BleBondBackupStatus::Unavailable;

    File file = fs.open(path, FILE_READ);
    if (!file) {
        return BleBondBackupStatus::Unavailable;
    }

    // VFS File::size() can retain a default zero after its initial fstat fails.
    // Only the successful authoritative stat above may classify a live backup
    // as malformed and authorize replacement of its committed recovery copy.
    const uint64_t fileSize = static_cast<uint64_t>(details.st_size);
    const size_t maxSize = sizeof(BondBackupHeader) + 2 * kMaxBleBondEntries * sizeof(struct ble_store_value_sec);
    if (fileSize < sizeof(BondBackupHeader) || fileSize > maxSize) {
        file.close();
        Serial.printf("[BLE] Bond backup file size invalid: %u\n", static_cast<unsigned>(fileSize));
        return BleBondBackupStatus::Invalid;
    }

    BondBackupHeader& header = contents.header;
    if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        file.close();
        return BleBondBackupStatus::Unavailable;
    }

    if (memcmp(header.magic, kBleBondMagic, sizeof(header.magic)) != 0) {
        file.close();
        Serial.println("[BLE] Bond backup magic mismatch");
        return BleBondBackupStatus::Invalid;
    }

    if (header.ourSecCount > kMaxBleBondEntries || header.peerSecCount > kMaxBleBondEntries) {
        file.close();
        Serial.println("[BLE] Bond backup count out of range");
        return BleBondBackupStatus::Invalid;
    }

    const size_t expectedSize = sizeof(BondBackupHeader) + header.ourSecCount * sizeof(struct ble_store_value_sec) +
                                header.peerSecCount * sizeof(struct ble_store_value_sec);
    if (fileSize != expectedSize) {
        file.close();
        Serial.println("[BLE] Bond backup size does not match record counts");
        return BleBondBackupStatus::Invalid;
    }

    auto& ourSecs = contents.ourSecs;
    auto& peerSecs = contents.peerSecs;

    for (uint32_t i = 0; i < header.ourSecCount; ++i) {
        if (file.read(reinterpret_cast<uint8_t*>(&ourSecs[i]), sizeof(ourSecs[i])) != sizeof(ourSecs[i])) {
            file.close();
            return BleBondBackupStatus::Unavailable;
        }
    }

    for (uint32_t i = 0; i < header.peerSecCount; ++i) {
        if (file.read(reinterpret_cast<uint8_t*>(&peerSecs[i]), sizeof(peerSecs[i])) != sizeof(peerSecs[i])) {
            file.close();
            return BleBondBackupStatus::Unavailable;
        }
    }

    file.close();

    return BleBondBackupStatus::Valid;
}

struct ExistingBond {
    const ble_store_value_sec* candidate;
    bool found = false;
};

int findExistingBond(int, union ble_store_value* value, void* context) {
    auto& existing = *static_cast<ExistingBond*>(context);
    if (memcmp(&value->sec.peer_addr, &existing.candidate->peer_addr, sizeof(value->sec.peer_addr)) == 0) {
        existing.found = true;
    }
    return 0;
}

bool restoreBondRecord(int type, const ble_store_value_sec& sec, bool resuming) {
    if (resuming) {
        // Only called once before runtime on a fresh boot. The table therefore
        // represents durable NVS, including any newer pairing after the failed
        // recovery. Do not overwrite its keys with the older SD copy.
        ExistingBond existing{&sec};
        if (ble_store_iterate(type, findExistingBond, &existing) != 0) return false;
        if (existing.found) return true;
    }
    const int result = type == BLE_STORE_OBJ_TYPE_OUR_SEC ? ble_store_write_our_sec(&sec)
                                                        : ble_store_write_peer_sec(&sec);
    if (result != 0) {
        Serial.printf("[BLE] Bond recovery write failed (type=%d error=%d); keeping complete SD backup\n", type, result);
    }
    return result == 0;
}

int restoreBleBondBackupContents(BondBackupContents& contents, bool resuming) {
    const auto& header = contents.header;
    // Mark recovery before touching NimBLE: even a reset after its first
    // successful write must not make a partial NVS table authoritative.
    gRestoreAttempted = true;
    gBondRecoveryState.store(RecoveryState::Pending, std::memory_order_release);
    gBondRecoveryEpoch.fetch_add(1, std::memory_order_acq_rel);
    if (!storeRecoveryState(true)) return -1;
    for (uint32_t i = 0; i < header.ourSecCount; ++i) {
        if (!restoreBondRecord(BLE_STORE_OBJ_TYPE_OUR_SEC, contents.ourSecs[i], resuming)) return -1;
    }
    for (uint32_t i = 0; i < header.peerSecCount; ++i) {
        if (!restoreBondRecord(BLE_STORE_OBJ_TYPE_PEER_SEC, contents.peerSecs[i], resuming)) return -1;
    }
    if (!storeRecoveryState(false)) return -1;
    gBondRecoveryState.store(RecoveryState::Ready, std::memory_order_release);
    gRestoreAttempted = false;
    return static_cast<int>(header.ourSecCount + header.peerSecCount);
}

} // namespace

void initializeBleBondRecovery() {
    gRestoreAttempted = false;
    gBondRecoveryState.store(readRecoveryState(), std::memory_order_release);
    if (bleBondRecoveryPending()) {
        Serial.println("[BLE] Bond recovery pending; snapshots and bond deletion deferred");
    }
}

bool bleBondRecoveryPending() {
    return gBondRecoveryState.load(std::memory_order_acquire) != RecoveryState::Ready;
}

uint32_t bleBondRecoveryEpoch() {
    return gBondRecoveryEpoch.load(std::memory_order_acquire);
}

int handleBleBondStoreStatus(ble_store_status_event* event, void* context) {
    // NimBLE's default overflow handler deletes the oldest peer and retries.
    // During recovery that would silently replace a still-needed record.
    return bleBondRecoveryPending() ? -1 : ble_store_util_status_rr(event, context);
}

BleBondBackupStatus inspectBleBondBackup(fs::FS& fs, const char* path) {
    BondBackupContents contents;
    return readBleBondBackup(fs, path, contents);
}

int restoreBleBondBackup(fs::FS& fs, const char* livePath) {
    if (!livePath || livePath[0] == '\0') {
        return -1;
    }

    // NimBLE retains a failed write in RAM. Retrying against that table in the
    // same boot could mistake it for durable progress; retry only after reboot.
    if (gRestoreAttempted || gBondRecoveryState.load(std::memory_order_acquire) == RecoveryState::Unavailable) return -1;
    const bool resuming = bleBondRecoveryPending();
    BondBackupContents contents;
    const BleBondBackupStatus liveStatus = readBleBondBackup(fs, livePath, contents);
    if (liveStatus == BleBondBackupStatus::Valid) {
        return restoreBleBondBackupContents(contents, resuming);
    }
    // A temporary read failure says nothing about which generation is valid.
    // Restoring .prev here would seed older bonds into NVS, then normal boot
    // would snapshot them over the newer live backup.
    if (liveStatus == BleBondBackupStatus::Unavailable) return -1;

    const String rollbackPath = StorageManager::rollbackPathFor(livePath);
    if (rollbackPath.length() == 0) {
        return -1;
    }

    if (readBleBondBackup(fs, rollbackPath.c_str(), contents) != BleBondBackupStatus::Valid) return -1;
    return restoreBleBondBackupContents(contents, resuming);
}

void restoreOrBackupBleBondsAtBoot(StorageManager& storage, size_t bondCount) {
    if (bondCount > 0 && !bleBondRecoveryPending()) {
        // Normal boot captures in memory; filesystem work stays on Core 0.
        enqueueCurrentBleBondBackupSnapshot();
        return;
    }
    // Recovery runs before scanning/connecting. Missing SD must not prevent
    // normal BLE operation; a pending marker survives until a later boot.
    if (storage.isReady() && storage.isSDCard()) {
        StorageManager::SDLockBlocking sdLock(storage.getSDMutex());
        fs::FS* sdFs = sdLock ? storage.getFilesystem() : nullptr;
        if (sdFs) {
            const int restored = restoreBleBondBackup(*sdFs, "/v1simple_ble_bonds.bin");
            if (restored > 0) Serial.printf("[BLE] Restored %d bond(s) from SD backup\n", restored);
        }
    }
    if (bleBondRecoveryPending()) {
        Serial.println("[BLE] Bond recovery unfinished; snapshots and bond deletion deferred until reboot recovery");
    }
}
