#pragma once

#include <Arduino.h>
#include <Preferences.h>

namespace BleFreshFlashPolicy {

constexpr const char* kNamespace = "ble_state";
constexpr const char* kFirmwareVersionKey = "fwVersion";

using BackupBondsFn = int (*)();
using ClearBondsFn = void (*)();

struct BondResetResult {
    int backedUpBondCount = -1;
    bool clearedBonds = false;
    bool recordedVersion = false;
};

String readStoredFirmwareVersion(Preferences& prefs);
bool hasFirmwareVersionMismatch(Preferences& prefs, const char* currentVersion);
bool storeFirmwareVersion(Preferences& prefs, const char* currentVersion);
BondResetResult resetBondsForFirmwareVersion(Preferences& prefs,
                                             const char* currentVersion,
                                             BackupBondsFn backupBonds,
                                             ClearBondsFn clearBonds);

}  // namespace BleFreshFlashPolicy

