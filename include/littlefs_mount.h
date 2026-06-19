#pragma once

#include <cstdint>

namespace fsmount {

// Mount contract for the on-flash "storage" partition. These values must
// match partitions_v1.csv and the image built by `pio run -t buildfs`.
inline constexpr bool kAutoFormat = false;
inline constexpr char kBasePath[] = "/littlefs";
inline constexpr std::uint8_t kMaxOpenFiles = 10;
inline constexpr char kPartitionLabel[] = "storage";

// Returns true if the storage partition mounted. The caller owns any teardown;
// the panic path mounts before normal storage init and must release it itself.
bool mountStorage();

}  // namespace fsmount
