/**
 * ALP SD Logger — CSV event logger for ALP state machine.
 *
 * Logs state transitions, heartbeat byte1 changes, gun identifications,
 * and alert/noise/teardown events to a CSV file on SD card. Designed
 * for field capture of ALP serial patterns during drives to help
 * distinguish PDC/DLI/LID operating states and refine badge color mapping.
 *
 * Design:
 *   - Main-loop callers enqueue fixed-size rows with xQueueSend(..., 0).
 *   - A low-priority Core 0 writer owns SD open/write/close work.
 *   - CSV file per boot session: /alp/alp_<bootId>-<token8>.csv
 *     (legacy format /alp/alp_<bootId>.csv used only when no bootToken
 *     is supplied, e.g. unit tests).
 *   - Controlled by alpSdLogEnabled setting (/alp page toggle).
 *   - All methods are no-ops when disabled or SD unavailable.
 *   - High-rate diagnostics are sampled/dropped so field logging does not
 *     become a permanent Core 1 tax during normal driving.
 */

#pragma once

#include <cstdint>

#ifndef UNIT_TEST
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "../../psram_freertos_alloc.h"
#endif

// Forward declarations — no heavy includes in the header
enum class AlpState : uint8_t;
enum class AlpGunType : uint8_t;
class GpsTimePublisher;

class AlpSdLogger {
  public:
    /**
     * Initialize the logger.
     * @param enabled   true if alpSdLogEnabled setting is on
     * @param sdReady   true if StorageManager has SD mounted
     * @param timePub   optional GPS time publisher for UTC column (may be nullptr)
     */
    void begin(bool enabled, bool sdReady, GpsTimePublisher* timePub = nullptr);

    /**
     * Set boot ID and token for filename generation. Call after begin().
     */
    void setBootId(uint32_t id, uint32_t bootToken = 0);

    /**
     * Log a state transition.
     */
    void logStateTransition(uint32_t nowMs, AlpState from, AlpState to, const char* direction = "UNKNOWN");

    /**
     * Log a heartbeat byte1 change (alert detection signal).
     */
    void logHeartbeatByte1(uint32_t nowMs, uint8_t prevByte1, uint8_t newByte1, AlpState currentState,
                           const char* direction = "UNKNOWN");

    /**
     * Log a gun identification event.
     */
    void logGunIdentified(uint32_t nowMs, AlpGunType gun, uint8_t byte0, uint8_t byte1or2, bool isDetectTrigger,
                          AlpState currentState, const char* direction = "UNKNOWN");

    /**
     * Log a raw frame (for high-value frames like alert triggers, gun candidates).
     */
    void logFrame(uint32_t nowMs, const char* frameType, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t cs,
                  AlpState currentState, const char* direction = "UNKNOWN");

    /**
     * Log every B0 heartbeat frame (byte0, byte1, byte2) for trace-level
     * analysis of LISTENING sub-state cycling. Called on every B0 frame
     * regardless of whether byte1 changed — the cycling pattern within
     * 02/03/04 may encode PDC/DLI/LID state visible on the control pad LED.
     *
     * Rate-limited: writes at most one row per HEARTBEAT_LOG_INTERVAL_MS.
     * Targeted/resolve edges are still logged separately via HB_BYTE1 rows,
     * so sampling here preserves the forensic timeline without writing every
     * steady-state heartbeat.
     */
    void logHeartbeat(uint32_t nowMs, uint8_t b0, uint8_t b1, uint8_t b2, AlpState currentState,
                      const char* direction = "UNKNOWN");

    /**
     * Log a noise window entry/exit or teardown event.
     */
    void logEvent(uint32_t nowMs, const char* event, AlpState currentState, uint32_t extraValue = 0,
                  const char* direction = "UNKNOWN");

    /**
     * Log a session lifecycle event (SESSION_OPEN, SESSION_CLOSE, etc.).
     * Uses the same CSV columns but packs session-specific data:
     *   event:      SESSION_OPEN / SESSION_CLOSE / SESSION_GUN /
     *               WARMUP_FLAG / WARMUP_RELEASE
     *   from_state: current ALP state
     *   gun:        gun name (if known)
     *   extra:      freeform detail string (duration, trigger count, etc.)
     */
    void logSessionEvent(uint32_t nowMs, const char* event, AlpState currentState, AlpGunType gun, const char* extra,
                         const char* direction = "UNKNOWN");

    /** Is logging active? */
    bool isEnabled() const { return enabled_ && sdReady_; }

    /** Get the CSV file path. */
    const char* csvPath() const { return csvPathBuf_; }

#ifndef UNIT_TEST
    bool writerTaskActive() const { return writerTask_ != nullptr; }
    bool writerTaskStackInPsram() const { return writerTaskStackInPsram_; }
    uint32_t writerStackHighWaterBytes() const;
#endif

    /** Update enabled state at runtime (setting changed via web UI). */
    void setEnabled(bool enabled);

    /** Flush pending writes with timeout (car power mode shutdown). */
    void drainAndClose(uint32_t timeoutMs);

#ifdef UNIT_TEST
    const char* testGetLastLine() const { return lastLineBuf_; }
    void testClearLastLine() { lastLineBuf_[0] = '\0'; }
#endif

  private:
    bool appendLine(const char* line);
#ifndef UNIT_TEST
    struct LogItem {
        bool shutdown = false;
        char line[240];
    };
    bool ensureAsyncWriter();
    bool writeLineBlocking(const char* line);
    static void writerTaskEntry(void* param);
    void writerTaskLoop();
#endif
    bool ensureDirectory();
    bool ensureHeader();

    // Heartbeats arrive roughly every 800 ms. Sampling them at 3 s keeps a
    // trace of steady-state mode without paying a write on every cycle.
    static constexpr uint32_t HEARTBEAT_LOG_INTERVAL_MS = 3000;

    bool enabled_ = false;
    bool sdReady_ = false;
    bool dirReady_ = false;
    bool headerWritten_ = false;
    uint32_t linesWritten_ = 0;
    uint32_t dropCount_ = 0;
    uint32_t lastHeartbeatLogMs_ = 0;
    char csvPathBuf_[48] = {0};
    GpsTimePublisher* timePub_ = nullptr;

#ifndef UNIT_TEST
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t writerTask_ = nullptr;
    PsramQueueAllocation queueAllocation_ = {};
    bool queueInPsram_ = false;
    bool writerTaskStackInPsram_ = false;
#endif

#ifdef UNIT_TEST
    char lastLineBuf_[160] = {0};
#endif
};
