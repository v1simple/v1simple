/**
 * main_boot.cpp — Boot-time helper functions extracted from main.cpp.
 *
 * These are self-contained utilities called during setup() that have
 * no dependency on main.cpp's mutable state variables.  Extracting them
 * keeps the core setup()/loop() orchestration file focused.
 */

#include "main_internals.h"
#include "display.h"
#include "settings.h"
#include "settings_keys.h"
#include "littlefs_mount.h"
#include "modules/perf/debug_macros.h" // SerialLog
#include "esp_heap_caps.h"
#include "esp_core_dump.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <nvs.h>

// Display is defined in main.cpp; needed by fatalBootError().
extern V1Display display;

// --- resetReasonToString ---

const char* resetReasonToString(esp_reset_reason_t reason) {
    switch (reason) {
    case ESP_RST_POWERON:
        return "POWERON";
    case ESP_RST_SW:
        return "SW";
    case ESP_RST_PANIC:
        return "PANIC";
    case ESP_RST_INT_WDT:
        return "WDT_INT";
    case ESP_RST_TASK_WDT:
        return "WDT_TASK";
    case ESP_RST_WDT:
        return "WDT";
    case ESP_RST_DEEPSLEEP:
        return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
        return "BROWNOUT";
    case ESP_RST_SDIO:
        return "SDIO";
    default:
        return "UNKNOWN";
    }
}

// --- logPanicBreadcrumbs ---

// PANIC BREADCRUMBS: Log heap stats + coredump info on crash recovery
void logPanicBreadcrumbs() {
    esp_reset_reason_t reason = esp_reset_reason();
    bool isCrash =
        (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT);

    if (!isCrash)
        return;

    Serial.println("\n!!! CRASH RECOVERY DETECTED !!!");
    Serial.printf("Reset reason: %s\n", resetReasonToString(reason));

    // Log current heap stats (post-crash, but helpful for baseline)
    uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    uint32_t minFreeHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    Serial.printf("Heap now: free=%lu, largest=%lu, minEver=%lu\n", (unsigned long)freeHeap,
                  (unsigned long)largestBlock, (unsigned long)minFreeHeap);

    // Check for coredump
    esp_core_dump_summary_t summary;
    esp_err_t err = esp_core_dump_get_summary(&summary);
    if (err == ESP_OK) {
        Serial.println("Coredump found:");
        Serial.printf("  Crashed task: %s\n", summary.exc_task);
        Serial.printf("  Exception cause: %lu\n", (unsigned long)summary.ex_info.exc_cause);
        Serial.printf("  Exception PC: 0x%08lx\n", (unsigned long)summary.exc_pc);

        // Print backtrace if available
        if (summary.exc_bt_info.depth > 0) {
            Serial.print("  Backtrace: ");
            for (int i = 0; i < summary.exc_bt_info.depth && i < 16; i++) {
                Serial.printf("0x%08lx ", (unsigned long)summary.exc_bt_info.bt[i]);
            }
            Serial.println();
        }
    } else {
        Serial.printf("No coredump available (err=%d) - check serial log for backtrace\n", err);
    }

    Serial.println("!!! END CRASH RECOVERY INFO !!!\n");

    // Best-effort: Try to write panic.txt to LittleFS (SD not mounted yet)
    // This runs BEFORE storage init, so we use LittleFS directly
    if (fsmount::mountStorage()) { // never auto-format during panic logging
        File f = LittleFS.open("/panic.txt", FILE_WRITE);
        if (f) {
            f.printf("CRASH at boot (millis=%lu)\n", millis());
            f.printf("Reset reason: %s\n", resetReasonToString(reason));
            f.printf("Heap: free=%lu, largest=%lu, minEver=%lu\n", (unsigned long)freeHeap, (unsigned long)largestBlock,
                     (unsigned long)minFreeHeap);

            if (err == ESP_OK) {
                f.printf("Task: %s\n", summary.exc_task);
                f.printf("PC: 0x%08lx\n", (unsigned long)summary.exc_pc);
                if (summary.exc_bt_info.depth > 0) {
                    f.print("BT: ");
                    for (int i = 0; i < summary.exc_bt_info.depth && i < 16; i++) {
                        f.printf("0x%08lx ", (unsigned long)summary.exc_bt_info.bt[i]);
                    }
                    f.println();
                }
            }
            f.close();
            Serial.println("[PANIC] Wrote /panic.txt to LittleFS");
        }
        LittleFS.end(); // Release mount before storage manager takes ownership
    }
}

// --- nvsHealthCheck ---

// Log NVS statistics without mutating settings namespaces during early boot.
void nvsHealthCheck() {
    nvs_stats_t stats;
    if (nvs_get_stats(NULL, &stats) == ESP_OK) {
        if (stats.total_entries == 0) {
            Serial.println("[NVS] WARN: NVS reports zero total entries — partition may be corrupt");
            return;
        }
        uint32_t usedPct = (stats.used_entries * 100) / stats.total_entries;
        Serial.printf("[NVS] Entries: %lu/%lu used (%lu%%), namespaces: %lu, free: %lu\n",
                      (unsigned long)stats.used_entries, (unsigned long)stats.total_entries, (unsigned long)usedPct,
                      (unsigned long)stats.namespace_count, (unsigned long)stats.free_entries);

        if (usedPct > 80) {
            Serial.println("[NVS] WARN: NVS >80% full; deferring namespace cleanup until settings load resolves the "
                           "active namespace");
        }
    } else {
        Serial.println("[NVS] WARN: Could not get NVS stats");
    }
}

// --- nextBootId ---

uint32_t nextBootId() {
    Preferences prefs;
    if (!prefs.begin("v1boot", false)) {
        return 0;
    }
    uint32_t bootId = prefs.getUInt(kNvsBootId, 0) + 1;
    prefs.putUInt(kNvsBootId, bootId);
    prefs.end();
    return bootId;
}

// --- clean-shutdown marker ---
//
// A small boolean in the "v1boot" NVS namespace used to distinguish a graceful
// power-off (long-press → prepareForShutdown → markCleanShutdown → powerOff)
// from an unexpected power cut (car ignition drop, cable yank, brownout).
//
// Semantics:
//   - readAndResetCleanShutdownMarker() is called once at boot. It returns the
//     previous value and immediately writes false, so the default assumption
//     during the next run is "we died uncleanly." prepareForShutdown() flips
//     it to true, and an aborted hardware tail immediately restores false.
//   - If the marker reads false at boot, the previous run ended uncleanly.
//     This is the normal case under car-power mode with no hold-up.

bool readAndResetCleanShutdownMarker() {
    Preferences prefs;
    if (!prefs.begin("v1boot", false)) {
        return false; // assume unclean if NVS is unreachable
    }
    const bool prev = prefs.getBool(kNvsCleanShutdn, false);
    prefs.putBool(kNvsCleanShutdn, false);
    prefs.end();
    return prev;
}

void markCleanShutdown() {
    Preferences prefs;
    if (!prefs.begin("v1boot", false)) {
        return;
    }
    prefs.putBool(kNvsCleanShutdn, true);
    prefs.end();
}

void markUncleanShutdown() {
    Preferences prefs;
    if (!prefs.begin("v1boot", false)) {
        Serial.println("[Battery] WARN: Failed to open clean-shutdown marker for abort recovery");
        return;
    }
    const bool persisted = prefs.putBool(kNvsCleanShutdn, false) > 0;
    prefs.end();
    if (!persisted) {
        Serial.println("[Battery] WARN: Failed to rewrite clean-shutdown marker after abort");
    }
}

// --- maintenance boot marker ---
//
// This is intentionally one-shot. Normal runtime must not bring WiFi up late
// after BLE/display/SD have fragmented internal DMA memory. A user
// gesture writes this marker and reboots; early setup consumes it and takes a
// separate maintenance path that skips normal V1/BLE scan/runtime work.

bool requestMaintenanceBoot() {
    Preferences prefs;
    if (!prefs.begin("v1boot", false)) {
        return false;
    }
    const bool ok = prefs.putBool(kNvsMaintenanceBootReq, true) > 0;
    prefs.end();
    return ok;
}

bool readAndClearMaintenanceBootRequest() {
    Preferences prefs;
    if (!prefs.begin("v1boot", false)) {
        return false;
    }
    const bool requested = prefs.getBool(kNvsMaintenanceBootReq, false);
    prefs.putBool(kNvsMaintenanceBootReq, false);
    prefs.end();
    return requested;
}

// --- fatalBootError ---

// Helper for fatal boot errors - shows message, waits, then restarts
// displayAvailable: true if display.begin() succeeded and we can show on-screen error
void fatalBootError(const char* message, bool displayAvailable) {
    SerialLog.printf("FATAL: %s\n", message);

    if (displayAvailable) {
        // Show error on screen with countdown
        display.showDisconnected(); // Clear screen with base frame
        // Draw error message (red text, centered)
        // Note: Using drawStatusText-like approach
        SerialLog.println("Showing error on display, will restart in 10 seconds...");

        // Simple countdown - show message and wait
        for (int i = 10; i > 0; i--) {
            SerialLog.printf("Restarting in %d...\n", i);
            delay(1000);
        }
    } else {
        // No display - just wait and restart
        SerialLog.println("Display unavailable. Restarting in 10 seconds...");
        delay(10000);
    }

    SerialLog.println("Restarting...");
    ESP.restart();
}
