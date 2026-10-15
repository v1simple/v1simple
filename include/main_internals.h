#pragma once

#include <cstdint>
#include "esp_system.h" // esp_reset_reason_t

class HealthJournal;
class ProductEventLog;
class SettingsManager;
class V1Display;

// The display is double buffered; painting alone leaves the panel unchanged.
template <typename DisplayT>
void presentFatalBootFrame(DisplayT& display) {
    display.showDisconnected();
    display.flush();
}

const char* resetReasonToString(esp_reset_reason_t reason);

/// Log crash recovery breadcrumbs (heap stats, coredump) to Serial + LittleFS.
void logPanicBreadcrumbs();

/// Check NVS health and attempt cleanup if >80% full.
void nvsHealthCheck();

// Increments the persistent counter.
uint32_t nextBootId();

// Returns the previous clean-shutdown marker and clears it for this boot.
bool readAndResetCleanShutdownMarker();

// Called after shutdown preparation completes.
void markCleanShutdown();

// Clears the marker when hardware shutdown aborts and the runtime remains active.
void markUncleanShutdown();

/// Request that the next boot enter maintenance mode. Returns false if the
/// request could not be persisted to NVS.
bool requestMaintenanceBoot();

// Consumes the one-shot request; true selects maintenance instead of drive mode.
bool readAndClearMaintenanceBootRequest();

/// Show fatal error on display (if available), wait, then restart.
void fatalBootError(V1Display& display, const char* message, bool displayAvailable);

/// Stop and flush shared persistence services before a runtime tears down its
/// own transports. Returns true when final writes and the clean marker remain safe.
bool preparePersistenceForShutdown(ProductEventLog& events, HealthJournal& health, SettingsManager& settings);

/// Bounded event drain plus lifecycle END for controlled reboot paths. Returns
/// false only while a timed-out writer still owns storage; restart still wins.
bool completeLoggingForControlledRestart(ProductEventLog& events, HealthJournal& health);

/// Restore shared persistence admission and the unclean marker after the
/// hardware shutdown tail returns without powering down or entering deep sleep.
void resumePersistenceAfterAbortedShutdown(ProductEventLog& events, HealthJournal& health);

/// Emit the privacy-safe identity line shared by normal and maintenance boots.
void logBootIdentity(uint32_t bootId, esp_reset_reason_t resetReason);

/// Early setup diagnostics: serial settle, GPIO hold release, panic/NVS checks.
void initializeEarlyBootDiagnostics();

// Best-effort save of deferred device-store updates.
class StorageManager;
class V1DeviceStore;
void processV1DeviceStoreSave(uint32_t nowMs, StorageManager& storage, V1DeviceStore& devices);
