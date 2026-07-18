/**
 * Main internals — shared across main.cpp TU split.
 *
 * Provides: declarations for boot helpers and persistence helpers
 * extracted from main.cpp.  Each companion .cpp includes this header.
 */

#pragma once

#include <cstdint>
#include "esp_system.h" // esp_reset_reason_t
#include "main_runtime_services.h"

class QuietCoordinatorModule;

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

/// Request that the next boot enter maintenance mode. Returns false if the
/// request could not be persisted to NVS.
bool requestMaintenanceBoot();

/// Read the one-shot maintenance-boot request and immediately clear it.
/// true = this boot should skip normal drive runtime and start maintenance WiFi.
bool readAndClearMaintenanceBootRequest();

/// Show fatal error on display (if available), wait, then restart.
void fatalBootError(const char* message, bool displayAvailable);

// --- Setup orchestration helper declarations (main_setup_helpers.cpp) ---

/// Callback invoked immediately when BLE subscribe completes.
void onV1ConnectImmediate();

/// Callbacks invoked at authoritative main-loop V1 session boundaries.
void onV1SessionOpened(uint32_t sessionGeneration);
void onV1SessionClosed(uint32_t sessionGeneration);

/// Callback invoked once the BLE connect burst has settled.
void onV1Connected();

/// Mount storage + initialize profile/device stores and restore dependent
/// settings.
void initializeStorageAndProfiles();

/// Prepare persistence/runtime services for a power-off sequence before the final hardware tail runs.
void prepareForShutdown(void* context);

/// Initialize perf/observation CSV loggers and return the boot session id.
uint32_t initializeBootPerformanceLoggers(BootLoggingRuntimeServices& services);

/// Initialize touch hardware and apply persisted display controls.
void initializeTouchAndDisplayControls();

/// Configure auto-push + touch interaction modules after storage/BLE setup.
void configureUiInteractionModules(QuietCoordinatorModule& quietCoordinator);

/// Emit boot summary and WiFi startup policy logs.
void logBootSummaryAndWifiStartup(uint32_t bootId, esp_reset_reason_t resetReason);

/// Early setup diagnostics: serial settle, GPIO hold release, panic/NVS checks.
void initializeEarlyBootDiagnostics();

// --- Persistence helper declarations (main_persist.cpp) ---

/// Periodic best-effort save of deferred V1 device-store updates (Tier 7).
void processV1DeviceStoreSave(uint32_t nowMs);
