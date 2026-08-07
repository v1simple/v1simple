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

int restoreBleBondBackup(fs::FS& fs, const char* livePath);
