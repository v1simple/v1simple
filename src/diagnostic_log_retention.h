#pragma once

#include <FS.h>
#include <cstddef>
#include <cstdint>

#include "diagnostic_log_limits.h"

namespace DiagnosticLogRetention {

struct PruneResult {
    size_t matchedFiles = 0;
    size_t removedFiles = 0;
    size_t failedRemovals = 0;
};

struct BootPruneResult {
    PruneResult perf;
    PruneResult alp;
    PruneResult encounters;
};

// Delete the oldest exact logger-generated CSV names until at most maxFiles
// remain. Unrecognized files and directories are never candidates.
PruneResult pruneManagedDirectory(fs::FS& filesystem, const char* directory, const char* filenamePrefix,
                                  uint32_t currentBootId, size_t maxFiles);

// Normal boot calls this before starting writer tasks. One slot per category
// is reserved for the current boot, keeping the post-write total at the
// documented MANAGED_FILES_PER_CATEGORY limit.
BootPruneResult pruneBeforeCurrentBoot(fs::FS& filesystem, uint32_t currentBootId);

} // namespace DiagnosticLogRetention
