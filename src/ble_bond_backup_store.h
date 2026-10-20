#pragma once

#include <FS.h>

#include <cstddef>
#include <cstdint>

class StorageManager;
struct ble_store_status_event;

struct BondBackupHeader {
    uint8_t magic[4];
    uint32_t ourSecCount;
    uint32_t peerSecCount;
};

inline constexpr uint8_t kBleBondMagic[4] = {'B', 'L', 'B', 0x01};
static constexpr size_t kMaxBleBondEntries = 8;

enum class BleBondBackupStatus { Valid, Missing, Invalid, Unavailable };
// Uses the restore reader's validation without writing any entries to NimBLE.
BleBondBackupStatus inspectBleBondBackup(fs::FS& fs, const char* path);
// Load once before schema migration or runtime mutation. An unfinished restore
// keeps its SD source authoritative until a later boot completes it.
void initializeBleBondRecovery();
bool bleBondRecoveryPending();
uint32_t bleBondRecoveryEpoch();
int handleBleBondStoreStatus(ble_store_status_event* event, void* context);
int restoreBleBondBackup(fs::FS& fs, const char* livePath);

// Run after NimBLE init/schema migration, before scanning or connecting.
void restoreOrBackupBleBondsAtBoot(StorageManager& storage, size_t bondCount);
