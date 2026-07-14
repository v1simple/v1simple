#include "littlefs_mount.h"

#include <LittleFS.h>

namespace fsmount {

bool mountStorage() {
    return LittleFS.begin(kAutoFormat, kBasePath, kMaxOpenFiles, kPartitionLabel);
}

} // namespace fsmount
