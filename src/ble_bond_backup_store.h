#pragma once

#include <FS.h>

#include <cstddef>
#include <cstdint>
#include <optional>

class StorageManager;
struct ble_store_status_event;
struct ble_store_value_sec;

struct BondBackupHeader {
    uint8_t magic[4];
    uint32_t ourSecCount;
    uint32_t peerSecCount;
};

inline constexpr uint8_t kBleBondMagic[4] = {'B', 'L', 'B', 0x01};
inline constexpr char kBleBondBackupPath[] = "/v1simple_ble_bonds.bin";
static constexpr size_t kMaxBleBondEntries = 8;

enum class BleBondBackupStatus { Valid, Missing, Invalid, Unavailable };
// Uses the restore reader's validation without writing any entries to NimBLE.
BleBondBackupStatus inspectBleBondBackup(fs::FS& fs, const char* path);
uint64_t identifyBleBondBackup(const BondBackupHeader& header, const ble_store_value_sec* ourSecs,
                               const ble_store_value_sec* peerSecs);
std::optional<uint64_t> currentBleBondBackupIdentity(StorageManager& storage);
bool restoreMatchingBleBondBackup(StorageManager& storage, uint64_t identity);
// Load once before schema migration or runtime mutation. An unfinished restore
// keeps its SD source authoritative until a later boot completes it.
void initializeBleBondRecovery();
bool bleBondRecoveryPending();
uint32_t bleBondRecoveryEpoch();
int handleBleBondStoreStatus(ble_store_status_event* event, void* context);
int restoreBleBondBackup(fs::FS& fs, const char* livePath);

// Run after NimBLE init/schema migration, before scanning or connecting.
void restoreOrBackupBleBondsAtBoot(StorageManager& storage, size_t bondCount);
