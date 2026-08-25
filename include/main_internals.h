/**
 * Shared boot, setup, runtime, and persistence declarations.
 */

#pragma once

#include <cstdint>
#include "esp_system.h" // esp_reset_reason_t

class HealthJournal;
class ProductEventLog;
class SettingsManager;
class V1Display;

// --- Boot helper declarations (main_boot.cpp) ---

/// Map ESP reset reason enum to human-readable string.
const char* resetReasonToString(esp_reset_reason_t reason);

/// Log crash recovery breadcrumbs (heap stats, coredump) to Serial + LittleFS.
void logPanicBreadcrumbs();

/// Check NVS health and attempt cleanup if >80% full.
void nvsHealthCheck();

/// Increment and return persistent boot ID counter.
uint32_t nextBootId();

/// Read the clean-shutdown marker (v1boot NVS) and immediately reset it to
/// false. Returns the previous value. true = last run called prepareForShutdown
/// cleanly; false = last run died uncleanly (brownout, car-power cut, etc.).
bool readAndResetCleanShutdownMarker();

/// Write the clean-shutdown marker to true. Called at the end of
/// prepareForShutdown() so the next boot can recognize a graceful exit.
void markCleanShutdown();

/// Rewrite the clean-shutdown marker to false when the hardware shutdown tail
/// aborts and the current runtime remains active.
void markUncleanShutdown();

/// Request that the next boot enter maintenance mode. Returns false if the
/// request could not be persisted to NVS.
bool requestMaintenanceBoot();

/// Read the one-shot maintenance-boot request and immediately clear it.
/// true = this boot should skip normal drive runtime and start maintenance WiFi.
bool readAndClearMaintenanceBootRequest();

/// Show fatal error on display (if available), wait, then restart.
void fatalBootError(V1Display& display, const char* message, bool displayAvailable);

// --- Shared persistence/boot helper declarations (main_setup_helpers.cpp) ---

/// Stop and flush shared persistence services before a runtime tears down its
/// own transports. Returns true when final writes and the clean marker remain safe.
bool preparePersistenceForShutdown(ProductEventLog& events, HealthJournal& health, SettingsManager& settings);

/// Bounded event drain plus lifecycle END for controlled reboot paths. Returns
/// false only while a timed-out writer still owns storage; restart still wins.
bool completeLoggingForControlledRestart(ProductEventLog& events, HealthJournal& health);

/// Restore shared persistence admission and the unclean marker after the
/// hardware shutdown tail returns without powering down or entering deep sleep.
void resumePersistenceAfterAbortedShutdown(ProductEventLog& events);

/// Emit the privacy-safe identity line shared by normal and maintenance boots.
void logBootIdentity(uint32_t bootId, esp_reset_reason_t resetReason, const SettingsManager& settings);

/// Early setup diagnostics: serial settle, GPIO hold release, panic/NVS checks.
void initializeEarlyBootDiagnostics();

// --- Persistence helper declarations (main_persist.cpp) ---

/// Periodic best-effort save of deferred V1 device-store updates (Tier 7).
class StorageManager;
class V1DeviceStore;
void processV1DeviceStoreSave(uint32_t nowMs, StorageManager& storage, V1DeviceStore& devices);
