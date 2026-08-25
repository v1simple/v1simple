#include "gps_runtime_module.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

GpsRuntimeModule gpsRuntimeModule;

namespace {
bool elapsedExceeded(uint32_t nowMs, uint32_t startMs, uint32_t thresholdMs) {
    if (startMs == 0) {
        return false;
    }
    return static_cast<uint32_t>(nowMs - startMs) > thresholdMs;
}

} // namespace

void GpsRuntimeModule::begin(bool enabled, uint32_t baud) {
    baud_ = (baud == 9600 || baud == 38400 || baud == 115200) ? baud : GPS_BAUD;
    setEnabled(enabled);
}

void GpsRuntimeModule::setBaud(uint32_t baud) {
    baud_ = (baud == 9600 || baud == 38400 || baud == 115200) ? baud : GPS_BAUD;
}

void GpsRuntimeModule::setEnabled(bool enabled) {
    if (enabled_ == enabled) {
        return;
    }

    enabled_ = enabled;
    enableTransitions_++;
#if !defined(UNIT_TEST)
    if (!enabled) {
        if (parserActive_) {
            Serial1.end();
        }
        // EN is not driven; the module stays powered via its internal pull-up.
    } else {
        // EN not driven — module defaults ON via internal pull-up
        Serial1.setRxBufferSize(
            256); // 256 B covers worst-case NMEA burst at 9600 baud; 2048 was over-provisioned and consumed DMA DRAM
        Serial1.begin(baud_, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    }
#endif

    resetRuntimeState();
    if (enabled_) {
        detectionStartMs_ = millis();
        if (detectionStartMs_ == 0) {
            detectionStartMs_ = 1;
        }
        parserActive_ = true;
    }
}

void GpsRuntimeModule::resetRuntimeState() {
    sampleValid_ = false;
    hasFix_ = false;
    speedMph_ = 0.0f;
    satellites_ = 0;
    hdop_ = NAN;
    sampleTsMs_ = 0;
    moduleDetected_ = false;
    detectionTimedOut_ = false;
    parserActive_ = false;
    rmcFix_ = false;
    ggaFix_ = false;
    detectionStartMs_ = 0;
    lastFixTsMs_ = 0;
    lastStableFixTsMs_ = 0;
    lastSentenceTsMs_ = 0;
    hardwareSamples_ = 0;
    bytesRead_ = 0;
    sentencesSeen_ = 0;
    sentencesParsed_ = 0;
    parseFailures_ = 0;
    checksumFailures_ = 0;
    sentencesUnknown_ = 0;
    bufferOverruns_ = 0;
    stableSatellites_ = 0;
    sentenceActive_ = false;
    sentenceLen_ = 0;
    sentenceBuf_[0] = '\0';
}

void GpsRuntimeModule::invalidateSpeedSample() {
    sampleValid_ = false;
    speedMph_ = 0.0f;
    sampleTsMs_ = 0;
}

void GpsRuntimeModule::updateDetectionTimeout(uint32_t nowMs) {
    if (!enabled_ || moduleDetected_ || detectionTimedOut_ || detectionStartMs_ == 0) {
        return;
    }
    if (!elapsedExceeded(nowMs, detectionStartMs_, DETECTION_TIMEOUT_MS)) {
        return;
    }

    detectionTimedOut_ = true;
    parserActive_ = false;
    hasFix_ = false;
    rmcFix_ = false;
    ggaFix_ = false;
    satellites_ = 0;
    hdop_ = NAN;
    lastFixTsMs_ = 0;
    invalidateSpeedSample();
    updateStableFixState(nowMs);

#if !defined(UNIT_TEST)
    Serial1.end();
    // EN not driven — module stays powered via internal pull-up (GPS_EN_PIN = -1)
#endif
}

void GpsRuntimeModule::updateFixStaleness(uint32_t nowMs) {
    if (!hasFix_ || lastFixTsMs_ == 0) {
        return;
    }
    if (!elapsedExceeded(nowMs, lastFixTsMs_, FIX_STALE_MS)) {
        return;
    }

    hasFix_ = false;
    rmcFix_ = false;
    ggaFix_ = false;
    satellites_ = 0;
    hdop_ = NAN;
    lastFixTsMs_ = 0;
    invalidateSpeedSample();
    updateStableFixState(nowMs);
}

void GpsRuntimeModule::updateStableFixState(uint32_t nowMs) {
    const uint32_t tsMs = (nowMs == 0) ? millis() : nowMs;
    if (hasFix_) {
        lastStableFixTsMs_ = tsMs;
        if (firstFixMs_ == 0) {
            firstFixMs_ = tsMs; // Latch first ever stable fix time
        }
        const uint8_t targetSatellites = satellites_;
        if (stableSatellites_ == 0) {
            stableSatellites_ = targetSatellites;
        } else if (targetSatellites > stableSatellites_ + 1) {
            stableSatellites_ = static_cast<uint8_t>(stableSatellites_ + 1);
        } else if (stableSatellites_ > targetSatellites + 1) {
            stableSatellites_ = static_cast<uint8_t>(stableSatellites_ - 1);
        } else {
            stableSatellites_ = targetSatellites;
        }
        return;
    }

    if (lastStableFixTsMs_ != 0 && static_cast<uint32_t>(tsMs - lastStableFixTsMs_) <= STABLE_FIX_HOLD_MS) {
        return;
    }
    stableSatellites_ = 0;
}

bool GpsRuntimeModule::stableHasFixAt(uint32_t nowMs) const {
    if (hasFix_) {
        return true;
    }
    if (lastStableFixTsMs_ == 0) {
        return false;
    }
    const uint32_t tsMs = (nowMs == 0) ? millis() : nowMs;
    return static_cast<uint32_t>(tsMs - lastStableFixTsMs_) <= STABLE_FIX_HOLD_MS;
}

void GpsRuntimeModule::update(uint32_t nowMs) {
    if (!enabled_) {
        return;
    }

#if !defined(UNIT_TEST)
    if (parserActive_) {
        uint16_t processed = 0;
        while (processed < MAX_BYTES_PER_UPDATE && Serial1.available() > 0) {
            const int raw = Serial1.read();
            if (raw < 0) {
                break;
            }
            ingestByte(static_cast<char>(raw), nowMs);
            processed++;
        }
    }
#endif

    updateDetectionTimeout(nowMs);
    updateFixStaleness(nowMs);
}

void GpsRuntimeModule::ingestByte(char c, uint32_t nowMs) {
    bytesRead_++;

    if (c == '$') {
        sentenceActive_ = true;
        sentenceLen_ = 0;
        sentenceBuf_[sentenceLen_++] = c;
        return;
    }

    if (!sentenceActive_) {
        return;
    }

    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        sentenceBuf_[sentenceLen_] = '\0';
        if (sentenceLen_ > 0) {
            (void)processSentence(sentenceBuf_, nowMs);
        }
        sentenceActive_ = false;
        sentenceLen_ = 0;
        return;
    }

    if (sentenceLen_ >= (NMEA_LINE_MAX - 1)) {
        sentenceActive_ = false;
        sentenceLen_ = 0;
        bufferOverruns_++;
        return;
    }

    sentenceBuf_[sentenceLen_++] = c;
}

bool GpsRuntimeModule::processSentence(char* sentence, uint32_t nowMs) {
    if (!sentence || sentence[0] != '$') {
        parseFailures_++;
        return false;
    }

    sentencesSeen_++;
    lastSentenceTsMs_ = (nowMs == 0) ? millis() : nowMs;

    char* star = std::strchr(sentence, '*');
    if (!star || star <= (sentence + 1) || star[1] == '\0' || star[2] == '\0') {
        parseFailures_++;
        return false;
    }

    uint8_t expectedChecksum = 0;
    if (!parseChecksum(star + 1, expectedChecksum)) {
        parseFailures_++;
        return false;
    }

    uint8_t computedChecksum = 0;
    for (const char* p = sentence + 1; p < star; ++p) {
        computedChecksum ^= static_cast<uint8_t>(*p);
    }
    if (computedChecksum != expectedChecksum) {
        checksumFailures_++;
        parseFailures_++;
        return false;
    }

    moduleDetected_ = true;
    detectionTimedOut_ = false;

    *star = '\0';
    char* fields[20] = {};
    const size_t fieldCount = splitCsv(sentence + 1, fields, 20);
    if (fieldCount == 0 || !fields[0]) {
        parseFailures_++;
        return false;
    }

    bool ok = true;
    bool knownType = true;
    if (sentenceTypeEquals(fields[0], "RMC")) {
        ok = parseRmc(fields, fieldCount, nowMs);
    } else if (sentenceTypeEquals(fields[0], "GGA")) {
        ok = parseGga(fields, fieldCount, nowMs);
    } else {
        knownType = false;
    }

    if (!knownType) {
        sentencesUnknown_++;
        return true;
    }
    if (ok) {
        sentencesParsed_++;
    } else {
        parseFailures_++;
    }
    return ok;
}

bool GpsRuntimeModule::parseGga(char* fields[], size_t fieldCount, uint32_t nowMs) {
    if (!fields || fieldCount < 8) {
        return false;
    }

    uint32_t fixQuality = 0;
    if (fields[6] && fields[6][0] != '\0' && !parseUIntStrict(fields[6], fixQuality)) {
        return false;
    }

    uint32_t satelliteCount = 0;
    if (fields[7] && fields[7][0] != '\0' && !parseUIntStrict(fields[7], satelliteCount)) {
        return false;
    }
    satellites_ = static_cast<uint8_t>(std::min<uint32_t>(satelliteCount, 99));
    const bool ggaFix = (fixQuality > 0) && (satellites_ > 0);

    if (ggaFix) {
        if (!fields[2] || fields[2][0] == '\0' || !fields[3] || fields[3][0] == '\0' || !fields[4] ||
            fields[4][0] == '\0' || !fields[5] || fields[5][0] == '\0') {
            return false;
        }
        if (!validateNmeaCoordinate(fields[2], fields[3], true) ||
            !validateNmeaCoordinate(fields[4], fields[5], false)) {
            return false;
        }
    }

    float parsedHdop = NAN;
    if (fieldCount > 8 && fields[8] && fields[8][0] != '\0') {
        if (!parseFloatStrict(fields[8], parsedHdop) || parsedHdop < 0.0f) {
            return false;
        }
    }
    hdop_ = std::isfinite(parsedHdop) ? parsedHdop : NAN;

    ggaFix_ = ggaFix;
    hasFix_ = ggaFix_ || rmcFix_;
    if (hasFix_) {
        lastFixTsMs_ = (nowMs == 0) ? millis() : nowMs;
    } else {
        invalidateSpeedSample();
    }

    const uint32_t tsMs = (nowMs == 0) ? millis() : nowMs;
    updateStableFixState(tsMs);

    return true;
}

bool GpsRuntimeModule::parseRmc(char* fields[], size_t fieldCount, uint32_t nowMs) {
    if (!fields || fieldCount < 8 || !fields[2]) {
        return false;
    }

    const char status = fields[2][0];
    if (status != 'A' && status != 'a') {
        rmcFix_ = false;
        hasFix_ = ggaFix_;
        if (!hasFix_) {
            invalidateSpeedSample();
        }
        const uint32_t tsMs = (nowMs == 0) ? millis() : nowMs;
        updateStableFixState(tsMs);
        return true;
    }

    if (!fields[3] || fields[3][0] == '\0' || !fields[4] || fields[4][0] == '\0' || !fields[5] ||
        fields[5][0] == '\0' || !fields[6] || fields[6][0] == '\0') {
        return false;
    }

    if (!validateNmeaCoordinate(fields[3], fields[4], true) ||
        !validateNmeaCoordinate(fields[5], fields[6], false)) {
        return false;
    }

    float speedKnots = 0.0f;
    if (fields[7] && fields[7][0] != '\0') {
        if (!parseFloatStrict(fields[7], speedKnots) || speedKnots < 0.0f) {
            return false;
        }
    }

    const float speedMph = speedKnots * KNOTS_TO_MPH;
    if (!std::isfinite(speedMph) || speedMph < 0.0f) {
        return false;
    }

    rmcFix_ = true;
    hasFix_ = true;
    sampleValid_ = true;
    speedMph_ = std::clamp(speedMph, 0.0f, MAX_VALID_SPEED_MPH);
    sampleTsMs_ = (nowMs == 0) ? millis() : nowMs;
    lastFixTsMs_ = sampleTsMs_;
    hardwareSamples_++;
    updateStableFixState(sampleTsMs_);

    return true;
}

bool GpsRuntimeModule::validateNmeaCoordinate(const char* coordText, const char* hemisphereText, bool isLatitude) {
    if (!coordText || coordText[0] == '\0' || !hemisphereText || hemisphereText[0] == '\0') {
        return false;
    }

    const char hemi = hemisphereText[0];
    if (isLatitude) {
        if (hemi != 'N' && hemi != 'n' && hemi != 'S' && hemi != 's') {
            return false;
        }
    } else {
        if (hemi != 'E' && hemi != 'e' && hemi != 'W' && hemi != 'w') {
            return false;
        }
    }

    char* end = nullptr;
    const double raw = std::strtod(coordText, &end);
    if (end == coordText || *end != '\0' || !std::isfinite(raw) || raw < 0.0) {
        return false;
    }

    const double degreesPart = std::floor(raw / 100.0);
    const double minutesPart = raw - (degreesPart * 100.0);
    if (!std::isfinite(degreesPart) || !std::isfinite(minutesPart)) {
        return false;
    }
    if (minutesPart < 0.0 || minutesPart >= 60.0) {
        return false;
    }

    const double decimalDegrees = degreesPart + (minutesPart / 60.0);
    const double maxDegrees = isLatitude ? 90.0 : 180.0;
    if (decimalDegrees > maxDegrees) {
        return false;
    }

    return true;
}

bool GpsRuntimeModule::parseFloatStrict(const char* text, float& out) {
    if (!text || text[0] == '\0') {
        return false;
    }

    char* end = nullptr;
    const float parsed = std::strtof(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    out = parsed;
    return true;
}

bool GpsRuntimeModule::parseUIntStrict(const char* text, uint32_t& out) {
    if (!text || text[0] == '\0') {
        return false;
    }

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    out = static_cast<uint32_t>(parsed);
    return true;
}

bool GpsRuntimeModule::parseChecksum(const char* checksumText, uint8_t& out) {
    if (!checksumText || checksumText[0] == '\0' || checksumText[1] == '\0' || checksumText[2] != '\0') {
        return false;
    }

    auto nibble = [](char c, uint8_t& value) -> bool {
        if (c >= '0' && c <= '9') {
            value = static_cast<uint8_t>(c - '0');
            return true;
        }
        if (c >= 'A' && c <= 'F') {
            value = static_cast<uint8_t>(10 + (c - 'A'));
            return true;
        }
        if (c >= 'a' && c <= 'f') {
            value = static_cast<uint8_t>(10 + (c - 'a'));
            return true;
        }
        return false;
    };

    uint8_t high = 0;
    uint8_t low = 0;
    if (!nibble(checksumText[0], high) || !nibble(checksumText[1], low)) {
        return false;
    }
    out = static_cast<uint8_t>((high << 4) | low);
    return true;
}

size_t GpsRuntimeModule::splitCsv(char* payload, char* fields[], size_t maxFields) {
    if (!payload || !fields || maxFields == 0) {
        return 0;
    }

    size_t count = 0;
    fields[count++] = payload;

    for (char* p = payload; *p != '\0' && count < maxFields; ++p) {
        if (*p == ',') {
            *p = '\0';
            fields[count++] = p + 1;
        }
    }

    return count;
}

bool GpsRuntimeModule::sentenceTypeEquals(const char* type, const char* suffix) {
    if (!type || !suffix) {
        return false;
    }

    const size_t typeLen = std::strlen(type);
    const size_t suffixLen = std::strlen(suffix);
    if (typeLen < suffixLen) {
        return false;
    }
    return std::strcmp(type + (typeLen - suffixLen), suffix) == 0;
}

void GpsRuntimeModule::setScaffoldSample(float speedMph, bool hasFix, uint8_t satellites, float hdop,
                                         uint32_t timestampMs) {
    if (!enabled_) {
        return;
    }
    if (!std::isfinite(speedMph)) {
        return;
    }

    sampleValid_ = true;
    hasFix_ = hasFix;
    rmcFix_ = hasFix;
    ggaFix_ = hasFix;
    speedMph_ = std::clamp(speedMph, 0.0f, MAX_VALID_SPEED_MPH);
    satellites_ = satellites;
    hdop_ = std::isfinite(hdop) ? std::max(0.0f, hdop) : NAN;
    sampleTsMs_ = (timestampMs == 0) ? millis() : timestampMs;
    if (hasFix_) {
        lastFixTsMs_ = sampleTsMs_;
    }
    injectedSamples_++;
    updateStableFixState(sampleTsMs_);
}

#ifdef UNIT_TEST
void GpsRuntimeModule::clearSample() {
    const uint32_t nowMs = millis();
    invalidateSpeedSample();
    hasFix_ = false;
    rmcFix_ = false;
    ggaFix_ = false;
    satellites_ = 0;
    hdop_ = NAN;
    lastFixTsMs_ = 0;
    updateStableFixState(nowMs);
}
#endif // UNIT_TEST

bool GpsRuntimeModule::getFreshSpeed(uint32_t nowMs, float& speedMphOut, uint32_t& tsMsOut) const {
    if (!enabled_ || !sampleValid_ || !hasFix_ || sampleTsMs_ == 0) {
        return false;
    }
    if (static_cast<uint32_t>(nowMs - sampleTsMs_) > SAMPLE_MAX_AGE_MS) {
        return false;
    }
    speedMphOut = speedMph_;
    tsMsOut = sampleTsMs_;
    return true;
}

GpsRuntimeStatus GpsRuntimeModule::snapshot(uint32_t nowMs) const {
    GpsRuntimeStatus status;
    status.enabled = enabled_;
    status.sampleValid = sampleValid_;
    status.hasFix = hasFix_;
    status.stableHasFix = stableHasFixAt(nowMs);
    status.speedMph = speedMph_;
    status.satellites = satellites_;
    status.stableSatellites = status.stableHasFix ? stableSatellites_ : 0;
    status.hdop = hdop_;
    status.sampleTsMs = sampleTsMs_;
    status.injectedSamples = injectedSamples_;
    status.moduleDetected = moduleDetected_;
    status.detectionTimedOut = detectionTimedOut_;
    status.parserActive = parserActive_;
    status.hardwareSamples = hardwareSamples_;
    status.bytesRead = bytesRead_;
    status.sentencesSeen = sentencesSeen_;
    status.sentencesParsed = sentencesParsed_;
    status.parseFailures = parseFailures_;
    status.checksumFailures = checksumFailures_;
    status.sentencesUnknown = sentencesUnknown_;
    status.bufferOverruns = bufferOverruns_;
    status.lastSentenceTsMs = lastSentenceTsMs_;
    status.lastSentenceAgeMs = (lastSentenceTsMs_ != 0) ? static_cast<uint32_t>(nowMs - lastSentenceTsMs_) : UINT32_MAX;
    status.firstFixMs = firstFixMs_;
    status.enableTransitions = enableTransitions_;

    if (sampleValid_ && sampleTsMs_ != 0) {
        status.sampleAgeMs = static_cast<uint32_t>(nowMs - sampleTsMs_);
    } else {
        status.sampleAgeMs = UINT32_MAX;
    }
    if (hasFix_ && lastFixTsMs_ != 0) {
        status.fixAgeMs = static_cast<uint32_t>(nowMs - lastFixTsMs_);
    } else {
        status.fixAgeMs = UINT32_MAX;
    }
    if (status.stableHasFix && lastStableFixTsMs_ != 0) {
        status.stableFixAgeMs = static_cast<uint32_t>(nowMs - lastStableFixTsMs_);
    } else {
        status.stableFixAgeMs = UINT32_MAX;
    }
    return status;
}

#ifdef UNIT_TEST
bool GpsRuntimeModule::injectNmeaSentenceForTest(const char* nmeaSentence, uint32_t nowMs) {
    if (!enabled_ || !nmeaSentence) {
        return false;
    }

    const size_t inputLen = std::strlen(nmeaSentence);
    if (inputLen == 0 || inputLen >= NMEA_LINE_MAX) {
        bufferOverruns_++;
        return false;
    }

    char localBuf[NMEA_LINE_MAX] = {};
    std::memcpy(localBuf, nmeaSentence, inputLen);
    localBuf[inputLen] = '\0';
    return processSentence(localBuf, nowMs);
}
#endif
