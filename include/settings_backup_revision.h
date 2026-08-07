#pragma once

#include <cstdint>

namespace settings_backup_revision {

inline uint32_t next(const uint32_t current) noexcept {
    const uint32_t candidate = current + 1u;
    return candidate == 0u ? 1u : candidate;
}

inline uint32_t nextFresh(const uint32_t current, const uint32_t dueRevision,
                          const uint32_t completedRevision) noexcept {
    uint32_t candidate = next(current);
    while (candidate == dueRevision || candidate == completedRevision) {
        candidate = next(candidate);
    }
    return candidate;
}

inline bool pending(const uint32_t dueRevision, const uint32_t completedRevision) noexcept {
    return dueRevision != 0u && dueRevision != completedRevision;
}

inline uint32_t resumeCounter(const uint32_t dueRevision, const uint32_t completedRevision) noexcept {
    if (completedRevision != 0u) {
        return completedRevision;
    }
    if (dueRevision != 0u) {
        return dueRevision;
    }
    return 1u;
}

} // namespace settings_backup_revision
