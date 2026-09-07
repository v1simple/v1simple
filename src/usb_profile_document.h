#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "settings.h"

class V1ProfileManager;

inline constexpr size_t kUsbProfileDocumentMaxBytes = 128 * 1024;

// Complete profile catalog and three Auto-Push slots; excludes network secrets
// and unrelated device settings. Converges pending storage recovery first;
// an export failure never returns a partial bundle.
bool buildUsbProfileDocument(JsonDocument& doc, SettingsManager& settings, V1ProfileManager& profiles, String& error);

// Strictly validates the complete bundle before entering the existing recoverable
// restore transaction. Success means profile storage and settings NVS committed;
// the ordinary SD backup may still be pending.
SettingsBackupApplyResult applyUsbProfileDocument(SettingsManager& settings, V1ProfileManager& profiles,
                                                  const JsonDocument& doc, String& error,
                                                  const SettingsRestoreWatchdog& watchdog = {});
