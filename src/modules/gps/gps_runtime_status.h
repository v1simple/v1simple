#pragma once

#include <stdint.h>
#include <math.h>   // NAN, UINT32_MAX

/**
 * Snapshot of GPS runtime state returned by GpsRuntimeModule::snapshot().
 *
 * Pure data — no Arduino dependency, safe to include in native unit tests.
 * Both the real GpsRuntimeModule header and all test mocks must include this
 * file rather than redefining the struct locally.
 */
struct GpsRuntimeStatus {
    // --- Core fix state ---
    bool enabled = false;
    bool sampleValid = false;
    bool hasFix = false;
    bool stableHasFix = false;

    // --- Position / motion ---
    float speedMph = 0.0f;
    uint8_t satellites = 0;
    uint8_t stableSatellites = 0;
    float hdop = NAN;
    bool locationValid = false;
    float latitudeDeg = NAN;
    float longitudeDeg = NAN;
    bool courseValid = false;
    float courseDeg = NAN;

    // --- Timing / age ---
    uint32_t courseSampleTsMs = 0;
    uint32_t courseAgeMs = UINT32_MAX;
    uint32_t sampleTsMs = 0;
    uint32_t sampleAgeMs = UINT32_MAX;
    uint32_t fixAgeMs = UINT32_MAX;
    uint32_t stableFixAgeMs = UINT32_MAX;

    // --- Parser / hardware telemetry ---
    uint32_t injectedSamples = 0;
    bool moduleDetected = false;
    bool detectionTimedOut = false;
    bool parserActive = false;
    uint32_t hardwareSamples = 0;
    uint32_t bytesRead = 0;
    uint32_t sentencesSeen = 0;
    uint32_t sentencesParsed = 0;
    uint32_t parseFailures = 0;
    uint32_t checksumFailures = 0;
    uint32_t sentencesUnknown = 0;   // NMEA sentences with valid checksum but unrecognized type
    uint32_t bufferOverruns = 0;
    uint32_t lastSentenceTsMs = 0;
    uint32_t lastSentenceAgeMs = UINT32_MAX;  // UINT32_MAX = no sentence yet
    uint32_t firstFixMs = 0;         // millis() at first stable fix; 0 = not yet received
    uint32_t enableTransitions = 0;  // Number of enable/disable transitions
};
