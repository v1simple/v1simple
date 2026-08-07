/**
 * Display Edge Logging — ALP display-decision instrumentation
 *
 * Provides structured edge-triggered logging for V1Display ALP setter entry
 * points. Production routes through the ALP runtime module's
 * display-decision logging path; native unit tests use a no-op stub.
 */

#pragma once

#include <cstdint>

#ifdef UNIT_TEST

static inline void logV1DisplaySetterEdge(uint32_t, const char*, const char* = "") {}

#else

/**
 * Log a V1Display ALP setter entry point.
 * @param nowMs    Current milliseconds
 * @param setter   Setter name recorded in the edge log.
 * @param detail   Optional detail string (gun abbreviation, direction name, etc)
 */
void logV1DisplaySetterEdge(uint32_t nowMs, const char* setter, const char* detail = "");

#endif
