#include "ble_bond_backup_store.h"

#include "storage_manager.h"

#include <Arduino.h>

extern "C" {
#include "nimble/nimble/host/include/host/ble_store.h"
#include "nimble/nimble/host/include/host/ble_sm.h"
}

#include <array>
#include <cstring>

namespace {

int restoreBleBondBackupFromPath(fs::FS& fs, const char* path) {
    if (!path || path[0] == '\0' || !fs.exists(path)) {
        return -1;
    }

    File file = fs.open(path, FILE_READ);
    if (!file) {
        return -1;
    }

    const size_t fileSize = file.size();
    const size_t maxSize = sizeof(BondBackupHeader) +
                           2 * kMaxBleBondEntries * sizeof(struct ble_store_value_sec);
    if (fileSize < sizeof(BondBackupHeader) || fileSize > maxSize) {
        file.close();
        Serial.printf("[BLE] Bond backup file size invalid: %u\n", static_cast<unsigned>(fileSize));
        return -1;
    }

    BondBackupHeader header = {};
    if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        file.close();
        return -1;
    }

    if (memcmp(header.magic, kBleBondMagic, sizeof(header.magic)) != 0) {
        file.close();
        Serial.println("[BLE] Bond backup magic mismatch");
        return -1;
    }

    if (header.ourSecCount > kMaxBleBondEntries || header.peerSecCount > kMaxBleBondEntries) {
        file.close();
        Serial.println("[BLE] Bond backup count out of range");
        return -1;
    }

    const size_t expectedSize = sizeof(BondBackupHeader) +
                                header.ourSecCount * sizeof(struct ble_store_value_sec) +
                                header.peerSecCount * sizeof(struct ble_store_value_sec);
    if (fileSize < expectedSize) {
        file.close();
        Serial.println("[BLE] Bond backup file truncated");
        return -1;
    }

    std::array<ble_store_value_sec, kMaxBleBondEntries> ourSecs = {};
    std::array<ble_store_value_sec, kMaxBleBondEntries> peerSecs = {};

    for (uint32_t i = 0; i < header.ourSecCount; ++i) {
        if (file.read(reinterpret_cast<uint8_t*>(&ourSecs[i]), sizeof(ourSecs[i])) != sizeof(ourSecs[i])) {
            file.close();
            return -1;
        }
    }

    for (uint32_t i = 0; i < header.peerSecCount; ++i) {
        if (file.read(reinterpret_cast<uint8_t*>(&peerSecs[i]), sizeof(peerSecs[i])) != sizeof(peerSecs[i])) {
            file.close();
            return -1;
        }
    }

    file.close();

    int restored = 0;
    for (uint32_t i = 0; i < header.ourSecCount; ++i) {
        if (ble_store_write_our_sec(&ourSecs[i]) == 0) {
            ++restored;
        }
    }
    for (uint32_t i = 0; i < header.peerSecCount; ++i) {
        if (ble_store_write_peer_sec(&peerSecs[i]) == 0) {
            ++restored;
        }
    }

    return restored;
}

}  // namespace

int restoreBleBondBackup(fs::FS& fs, const char* livePath) {
    if (!livePath || livePath[0] == '\0') {
        return -1;
    }

    const int liveResult = restoreBleBondBackupFromPath(fs, livePath);
    if (liveResult >= 0) {
        return liveResult;
    }

    const String rollbackPath = StorageManager::rollbackPathFor(livePath);
    if (rollbackPath.length() == 0 || !fs.exists(rollbackPath.c_str())) {
        return -1;
    }

    return restoreBleBondBackupFromPath(fs, rollbackPath.c_str());
}
