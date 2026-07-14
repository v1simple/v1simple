#pragma once
#ifndef GPS_RUNTIME_MODULE_H
#define GPS_RUNTIME_MODULE_H

#include <Arduino.h>
#include "gps_runtime_status.h"

class GpsTimePublisher;
class GpsGeoPublisher;

class GpsRuntimeModule {
  public:
    // Freshness budget for GPS sample telemetry.
    static constexpr uint32_t SAMPLE_MAX_AGE_MS = 3000;

    void begin(bool enabled, bool enablePinActiveHigh = true, uint32_t baud = 9600, GpsTimePublisher* timePub = nullptr,
               GpsGeoPublisher* geoPub = nullptr);
    void setEnabled(bool enabled);
    void setBaud(uint32_t baud);                  // Updates stored baud; takes effect on next setEnabled(true)
    void setEnablePinActiveHigh(bool activeHigh); // Deprecated no-op; EN is not driven
    bool isEnabled() const { return enabled_; }

    // Non-blocking UART/NMEA ingest with bounded per-loop processing.
    void update(uint32_t nowMs);

    // Manual sample injection path used by API/tools and test scaffolding.
    void setScaffoldSample(float speedMph, bool hasFix, uint8_t satellites, float hdop, uint32_t timestampMs,
                           float latitudeDeg = NAN, float longitudeDeg = NAN, float courseDeg = NAN);
    bool getFreshSpeed(uint32_t nowMs, float& speedMphOut, uint32_t& tsMsOut) const;
    GpsRuntimeStatus snapshot(uint32_t nowMs) const;

    // Parse RMC UTC time + date into Unix epoch milliseconds.
    // Public static for testability.
    static bool parseRmcDateTime(const char* timeField, const char* dateField, int64_t& epochMsOut);

#ifdef UNIT_TEST
    // Test-only: clear all GPS sample state to simulate fix drop.
    void clearSample();

    // Native-test hook for parser coverage without UART hardware.
    bool injectNmeaSentenceForTest(const char* nmeaSentence, uint32_t nowMs);
#endif

  private:
    // Wiring for Adafruit Ultimate GPS v3 (PID 3133, MTK3339).
    // GPS TX → GPIO1 (Serial1 RX); GPS RX ← GPIO5 (Serial1 TX).
    // EN pin not driven — internal 10kΩ pull-up to VBAT keeps module ON by default.
    static constexpr int GPS_RX_PIN = 1;  // GPIO 1  — Serial1 RX ← GPS TX
    static constexpr int GPS_TX_PIN = 5;  // GPIO 5  — Serial1 TX → GPS RX
    static constexpr int GPS_EN_PIN = -1; // Not driven; module defaults ON via pull-up
    static constexpr uint32_t GPS_BAUD = 9600;
    static constexpr uint32_t DETECTION_TIMEOUT_MS = 60000;
    static constexpr uint32_t FIX_STALE_MS = 15000;
    static constexpr uint32_t STABLE_FIX_HOLD_MS = 3000;
    static constexpr size_t NMEA_LINE_MAX = 128;
    static constexpr uint16_t MAX_BYTES_PER_UPDATE = 1024;
    static constexpr float KNOTS_TO_MPH = 1.150779f;
    static constexpr float MAX_VALID_SPEED_MPH = 250.0f;
    static constexpr uint32_t GPS_TIME_UPDATE_INTERVAL_MS = 300000; // Rebase time at most every 5 minutes

    void resetRuntimeState();
    void invalidateSpeedSample();
    void publishObservation(uint32_t timestampMs);
    void updateDetectionTimeout(uint32_t nowMs);
    void updateFixStaleness(uint32_t nowMs);
    void updateStableFixState(uint32_t nowMs);
    bool stableHasFixAt(uint32_t nowMs) const;
    void ingestByte(char c, uint32_t nowMs);
    bool processSentence(char* sentence, uint32_t nowMs);
    bool parseGga(char* fields[], size_t fieldCount, uint32_t nowMs);
    bool parseRmc(char* fields[], size_t fieldCount, uint32_t nowMs);
    static bool parseNmeaCoordinate(const char* coordText, const char* hemisphereText, bool isLatitude,
                                    float& outDegrees);
    static bool parseFloatStrict(const char* text, float& out);
    static bool parseUIntStrict(const char* text, uint32_t& out);
    static bool parseChecksum(const char* checksumText, uint8_t& out);
    static size_t splitCsv(char* payload, char* fields[], size_t maxFields);
    static bool sentenceTypeEquals(const char* type, const char* suffix);
    void tryUpdateGpsTime(const char* timeField, const char* dateField, uint32_t nowMs);

    GpsTimePublisher* timePub_ = nullptr;
    GpsGeoPublisher* geoPub_ = nullptr;
    uint32_t baud_ = GPS_BAUD;        // from settings at begin()
    bool enablePinActiveHigh_ = true; // Deprecated compatibility state; EN is not driven
    bool enabled_ = false;
    bool sampleValid_ = false;
    bool hasFix_ = false;
    float speedMph_ = 0.0f;
    uint8_t satellites_ = 0;
    float hdop_ = NAN;
    bool locationValid_ = false;
    float latitudeDeg_ = NAN;
    float longitudeDeg_ = NAN;
    bool courseValid_ = false;
    float courseDeg_ = NAN;
    uint32_t courseSampleTsMs_ = 0;
    uint32_t sampleTsMs_ = 0;
    uint32_t injectedSamples_ = 0;
    bool moduleDetected_ = false;
    bool detectionTimedOut_ = false;
    bool parserActive_ = false;
    bool rmcFix_ = false;
    bool ggaFix_ = false;
    uint32_t detectionStartMs_ = 0;
    uint32_t lastFixTsMs_ = 0;
    uint32_t lastStableFixTsMs_ = 0;
    uint32_t lastSentenceTsMs_ = 0;
    uint32_t hardwareSamples_ = 0;
    uint32_t bytesRead_ = 0;
    uint32_t sentencesSeen_ = 0;
    uint32_t sentencesParsed_ = 0;
    uint32_t parseFailures_ = 0;
    uint32_t checksumFailures_ = 0;
    uint32_t bufferOverruns_ = 0;
    uint32_t lastGpsTimeUpdateMs_ = 0;
    uint32_t lastNoFixPublishMs_ = 0; // Throttle: last no-fix observation publish timestamp
    uint8_t stableSatellites_ = 0;
    uint32_t firstFixMs_ = 0;        // millis() at first stable fix; 0 = not yet
    uint32_t enableTransitions_ = 0; // Incremented on each setEnabled() call that changes state
    uint32_t sentencesUnknown_ = 0;  // NMEA sentences with valid checksum but unrecognized type
    static constexpr uint32_t NO_FIX_PUBLISH_INTERVAL_MS = 5000; // Publish no-fix at most every 5s
    bool sentenceActive_ = false;
    size_t sentenceLen_ = 0;
    char sentenceBuf_[NMEA_LINE_MAX] = {};
};
#endif // GPS_RUNTIME_MODULE_H
