#pragma once

#include <FS.h>

#include <cstddef>
#include <cstdint>

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
int restoreBleBondBackup(fs::FS& fs, const char* livePath);
