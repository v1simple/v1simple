#pragma once

#include <cstdint>

namespace obd {

// ── Scan parameters (web-UI-triggered only) ─────────────────────
static constexpr uint32_t SCAN_DURATION_MS = 5000;
static constexpr const char* DEVICE_NAME_CX = "OBDLink CX";

// ── Direct-connect (saved address) ──────────────────────────────
static constexpr uint32_t CONNECT_TIMEOUT_MS = 5000;
static constexpr uint32_t RECONNECT_BACKOFF_MS = 5000;
static constexpr uint8_t MAX_DIRECT_CONNECT_FAILURES = 3;

// ── Post-connect settle (DA14531 BLE 4.2 needs time before GATT) ─
static constexpr uint32_t POST_CONNECT_SETTLE_MS = 500;
static constexpr uint32_t SECURITY_TIMEOUT_MS = 3000;
static constexpr uint32_t POST_SUBSCRIBE_SETTLE_MS = 150;

// ── Boot dwell (V1 gets priority) ───────────────────────────────
static constexpr uint32_t POST_BOOT_DWELL_MS = 10000;

// ── Speed polling ───────────────────────────────────────────────
static constexpr uint32_t POLL_INTERVAL_MS = 500;
static constexpr uint32_t POLL_TIMEOUT_MS = 1000;
static constexpr uint32_t SEARCH_EXTENDED_TIMEOUT_MS = 10000;
static constexpr uint8_t POLL_COMMAND_RETRIES = 1;
static constexpr uint32_t SPEED_MAX_AGE_MS = 3000;
static constexpr uint8_t MAX_CONSECUTIVE_ERRORS = 5;
static constexpr uint32_t ERROR_PAUSE_MS = 5000;
static constexpr uint8_t ERRORS_BEFORE_DISCONNECT = 10;
static constexpr uint32_t AUX_INTERVAL_MS = 2000;
static constexpr uint32_t AUX_WINDOW_MIN_MS = 200;
static constexpr uint32_t AUX_COMMAND_TIMEOUT_MS = 180;
static constexpr uint32_t VIN_COMMAND_TIMEOUT_MS = 250;
static constexpr uint32_t EOT_STALE_MS = 10000;
static constexpr uint8_t BUFFER_OVERFLOWS_BEFORE_DISCONNECT = 2;

// ── ECU idle detection (car-off / petrol stop) ─────────────────
static constexpr uint8_t ECU_IDLE_BACKOFF_THRESHOLD = 6;        // backoff cycles before entering ECU_IDLE
static constexpr uint32_t ECU_IDLE_PROBE_INTERVAL_MS = 30000;   // slow reconnect probe while idling
static constexpr uint8_t EOT_INVALID_STREAK_CLEAR_CACHE = 3;
static constexpr uint8_t EOT_CACHE_PERSIST_SAMPLES = 3;
static constexpr uint8_t CORE_READY_MIN_SPEED_SAMPLES = 3;
static constexpr uint32_t CORE_READY_MIN_CONNECTED_MS = 5000;

// ── RSSI ────────────────────────────────────────────────────────
static constexpr int8_t DEFAULT_MIN_RSSI = -80;

// ── AT init sequence ────────────────────────────────────────────
static constexpr const char* COLD_INIT_COMMANDS[] = {
    "ATZ\r",
    "ATE0\r",
    "ATL0\r",
    "ATS0\r",
    "ATAL\r",
    "ATH0\r",
    "ATSP0\r",
    "ATAT1\r",
};
static constexpr size_t COLD_INIT_COMMAND_COUNT =
    sizeof(COLD_INIT_COMMANDS) / sizeof(COLD_INIT_COMMANDS[0]);

static constexpr const char* WARM_INIT_COMMANDS[] = {
    "ATE0\r",
    "ATL0\r",
    "ATS0\r",
    "ATAL\r",
    "ATH0\r",
    "ATSP0\r",
    "ATAT1\r",
};
static constexpr size_t WARM_INIT_COMMAND_COUNT =
    sizeof(WARM_INIT_COMMANDS) / sizeof(WARM_INIT_COMMANDS[0]);
static constexpr uint32_t AT_INIT_RESPONSE_TIMEOUT_MS = 2000;
static constexpr uint8_t AT_INIT_RETRIES = 1;

// ── Speed poll command ──────────────────────────────────────────
static constexpr const char* SPEED_POLL_CMD = "010D\r";
static constexpr const char* VIN_POLL_CMD = "0902\r";

}  // namespace obd
