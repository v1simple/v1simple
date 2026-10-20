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
    if (fileSize < expectedSize) {
        file.close();
        Serial.println("[BLE] Bond backup file truncated");
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

int restoreBleBondBackupContents(BondBackupContents& contents) {
    const auto& header = contents.header;
    auto& ourSecs = contents.ourSecs;
    auto& peerSecs = contents.peerSecs;
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

} // namespace

BleBondBackupStatus inspectBleBondBackup(fs::FS& fs, const char* path) {
    BondBackupContents contents;
    return readBleBondBackup(fs, path, contents);
}

int restoreBleBondBackup(fs::FS& fs, const char* livePath) {
    if (!livePath || livePath[0] == '\0') {
        return -1;
    }

    BondBackupContents contents;
    const BleBondBackupStatus liveStatus = readBleBondBackup(fs, livePath, contents);
    if (liveStatus == BleBondBackupStatus::Valid) {
        return restoreBleBondBackupContents(contents);
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
    return restoreBleBondBackupContents(contents);
}
