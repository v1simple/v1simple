#include "diagnostic_log_retention.h"

#include <climits>
#include <cstdio>
#include <cstring>

namespace DiagnosticLogRetention {
namespace {

constexpr size_t MAX_PATH_BYTES = 96;
constexpr size_t DELETE_BATCH_SIZE = 4;

struct ManagedLogRank {
    uint32_t age = 0;
    uint32_t token = 0;
    bool hasToken = false;
};

const char* leafName(const char* path) {
    if (!path) {
        return "";
    }
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool parseDecimalUint32(const char*& cursor, uint32_t& value) {
    if (!cursor || *cursor < '0' || *cursor > '9') {
        return false;
    }

    uint32_t parsed = 0;
    do {
        const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
        if (parsed > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
        ++cursor;
    } while (*cursor >= '0' && *cursor <= '9');

    value = parsed;
    return true;
}

bool parseHexDigit(char c, uint8_t& value) {
    if (c >= '0' && c <= '9') {
        value = static_cast<uint8_t>(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        value = static_cast<uint8_t>(10 + c - 'a');
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        value = static_cast<uint8_t>(10 + c - 'A');
        return true;
    }
    return false;
}

bool parseManagedLogLeaf(const char* leaf, const char* prefix, uint32_t& bootId, uint32_t& token, bool& hasToken) {
    if (!leaf || !prefix) {
        return false;
    }

    const size_t prefixLength = std::strlen(prefix);
    if (std::strncmp(leaf, prefix, prefixLength) != 0) {
        return false;
    }

    const char* cursor = leaf + prefixLength;
    if (!parseDecimalUint32(cursor, bootId)) {
        return false;
    }

    token = 0;
    hasToken = false;
    if (*cursor == '-') {
        hasToken = true;
        ++cursor;
        for (size_t i = 0; i < 8; ++i) {
            uint8_t digit = 0;
            if (!parseHexDigit(*cursor, digit)) {
                return false;
            }
            token = (token << 4U) | digit;
            ++cursor;
        }
    }

    return std::strcmp(cursor, ".csv") == 0;
}

bool isNewer(const ManagedLogRank& lhs, const ManagedLogRank& rhs) {
    if (lhs.age != rhs.age) {
        return lhs.age < rhs.age;
    }
    if (lhs.hasToken != rhs.hasToken) {
        return lhs.hasToken;
    }
    return lhs.token > rhs.token;
}

bool sameRank(const ManagedLogRank& lhs, const ManagedLogRank& rhs) {
    return lhs.age == rhs.age && lhs.token == rhs.token && lhs.hasToken == rhs.hasToken;
}

void insertNewest(ManagedLogRank* retained, size_t& retainedCount, size_t capacity, const ManagedLogRank& candidate) {
    if (capacity == 0) {
        return;
    }

    if (retainedCount < capacity) {
        retained[retainedCount++] = candidate;
    } else if (isNewer(candidate, retained[retainedCount - 1])) {
        retained[retainedCount - 1] = candidate;
    } else {
        return;
    }

    size_t index = retainedCount - 1;
    while (index > 0 && isNewer(retained[index], retained[index - 1])) {
        const ManagedLogRank swap = retained[index - 1];
        retained[index - 1] = retained[index];
        retained[index] = swap;
        --index;
    }
}

bool rankForEntry(File& entry, const char* filenamePrefix, uint32_t currentBootId, ManagedLogRank& rank) {
    if (!entry || entry.isDirectory()) {
        return false;
    }

    uint32_t bootId = 0;
    if (!parseManagedLogLeaf(leafName(entry.name()), filenamePrefix, bootId, rank.token, rank.hasToken)) {
        return false;
    }
    rank.age = currentBootId - bootId;
    return true;
}

bool shouldRemove(const ManagedLogRank& candidate, const ManagedLogRank* retained, size_t retainedCount) {
    if (retainedCount == 0) {
        return true;
    }
    const ManagedLogRank& cutoff = retained[retainedCount - 1];
    return !isNewer(candidate, cutoff) && !sameRank(candidate, cutoff);
}

} // namespace

PruneResult pruneManagedDirectory(fs::FS& filesystem, const char* directory, const char* filenamePrefix,
                                  uint32_t currentBootId, size_t maxFiles) {
    PruneResult result;
    if (!directory || !filenamePrefix || maxFiles > DiagnosticLogLimits::MANAGED_FILES_PER_CATEGORY) {
        return result;
    }

    ManagedLogRank retained[DiagnosticLogLimits::MANAGED_FILES_PER_CATEGORY]{};
    size_t retainedCount = 0;

    File dir = filesystem.open(directory, FILE_READ);
    if (!dir || !dir.isDirectory()) {
        dir.close();
        return result;
    }

    File entry = dir.openNextFile();
    while (entry) {
        ManagedLogRank rank;
        if (rankForEntry(entry, filenamePrefix, currentBootId, rank)) {
            ++result.matchedFiles;
            insertNewest(retained, retainedCount, maxFiles, rank);
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();

    if (result.matchedFiles <= maxFiles) {
        return result;
    }

    // Delete in small batches after closing the directory handle. Reopening
    // avoids depending on FatFs iterator behavior while entries are removed,
    // and the fixed batch keeps boot-time stack use bounded.
    while (true) {
        char candidates[DELETE_BATCH_SIZE][MAX_PATH_BYTES]{};
        size_t candidateCount = 0;

        dir = filesystem.open(directory, FILE_READ);
        if (!dir || !dir.isDirectory()) {
            dir.close();
            break;
        }

        entry = dir.openNextFile();
        while (entry && candidateCount < DELETE_BATCH_SIZE) {
            ManagedLogRank rank;
            if (rankForEntry(entry, filenamePrefix, currentBootId, rank) &&
                shouldRemove(rank, retained, retainedCount)) {
                const char* leaf = leafName(entry.name());
                const int length = std::snprintf(candidates[candidateCount], MAX_PATH_BYTES, "%s/%s", directory, leaf);
                if (length > 0 && static_cast<size_t>(length) < MAX_PATH_BYTES) {
                    ++candidateCount;
                } else {
                    ++result.failedRemovals;
                }
            }
            entry.close();
            entry = dir.openNextFile();
        }
        entry.close();
        dir.close();

        if (candidateCount == 0) {
            break;
        }

        size_t removedThisBatch = 0;
        for (size_t i = 0; i < candidateCount; ++i) {
            if (filesystem.remove(candidates[i])) {
                ++result.removedFiles;
                ++removedThisBatch;
            } else {
                ++result.failedRemovals;
            }
        }
        if (removedThisBatch == 0) {
            break;
        }
    }

    return result;
}

BootPruneResult pruneBeforeCurrentBoot(fs::FS& filesystem, uint32_t currentBootId) {
    constexpr size_t HISTORICAL_SLOTS = DiagnosticLogLimits::MANAGED_FILES_PER_CATEGORY - 1;
    BootPruneResult result;
    result.perf = pruneManagedDirectory(filesystem, "/perf", "perf_boot_", currentBootId, HISTORICAL_SLOTS);
    result.alp = pruneManagedDirectory(filesystem, "/alp", "alp_", currentBootId, HISTORICAL_SLOTS);
    result.encounters =
        pruneManagedDirectory(filesystem, "/encounters", "encounters_", currentBootId, HISTORICAL_SLOTS);
    return result;
}

} // namespace DiagnosticLogRetention
