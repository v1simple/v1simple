#pragma once

#include <cstddef>

namespace DiagnosticLogLimits {

// The maintenance API returns one bounded list spanning all managed log
// categories. Keep the automatic retention budget below that response limit
// so a normal card remains fully inspectable in one request.
inline constexpr size_t MAX_LISTED_FILES = 64;
inline constexpr size_t MAX_SCANNED_ENTRIES = 128;
inline constexpr size_t MANAGED_CATEGORY_COUNT = 3;
inline constexpr size_t MANAGED_FILES_PER_CATEGORY = 20;

static_assert(MANAGED_CATEGORY_COUNT * MANAGED_FILES_PER_CATEGORY <= MAX_LISTED_FILES,
              "managed log retention must fit in the maintenance listing");

} // namespace DiagnosticLogLimits
