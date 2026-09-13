// Mock packet_parser.h for native unit testing.
// Uses same include guard as real packet_parser.h to intercept the include.
#pragma once
#ifndef PACKET_PARSER_H
#define PACKET_PARSER_H

#include <array>
#include <vector>
#include <algorithm>
#include <cstdint>

// Use the canonical type definitions — Band, Direction, AlertData, DisplayState.
// Never redefine these here; the real and mock must always share the same struct layout.
#include "../../src/packet_parser_types.h"

/**
 * Mock PacketParser — controllable state for native unit testing.
 * Only the class behavior is mocked; all data types come from packet_parser_types.h.
 */
class PacketParser {
public:
    static constexpr size_t MAX_ALERTS = 15;

    // Test-controllable state
    DisplayState state;
    std::vector<AlertData> alerts;
    AlertData priorityAlert;
    bool hasAlertsFlag = false;
    uint32_t alertLifetimeValue = 0;
    int parseCalls = 0;
    bool parseReturnValue = true;
    V1DisplayOnObservation displayOnObservationValue;
    V1ModeObservation modeObservationValue;
    V1CurrentVolumeObservation currentVolumeObservationValue;
    V1AllVolumeObservation allVolumeObservationValue;
    V1DisplayVolumeObservation displayVolumeObservationValue;
    V1BluetoothIndicatorObservation bluetoothIndicatorObservationValue;
    V1SweepSectionsObservation sweepSectionsObservationValue;
    V1SweepMaxObservation sweepMaxObservationValue;
    V1SweepDefinitionsObservation sweepDefinitionsObservationValue;
    V1SweepWriteResultObservation sweepWriteResultObservationValue;
    bool synthesizeSweepResponses = false;
    V1SweepSectionsObservation synthesizedSweepSections;
    V1SweepMaxObservation synthesizedSweepMax;
    V1SweepDefinitionsObservation synthesizedSweepDefinitions;
    std::vector<std::vector<uint8_t>> parsedPackets;
    std::vector<uint32_t> parseTimestamps;
    std::vector<uint32_t> parseIngressSequences;

    void reset() {
        state = DisplayState();
        alerts.clear();
        priorityAlert = AlertData();
        hasAlertsFlag = false;
        alertLifetimeValue = 0;
        parseCalls = 0;
        parseReturnValue = true;
        displayOnObservationValue = V1DisplayOnObservation{};
        modeObservationValue = V1ModeObservation{};
        currentVolumeObservationValue = V1CurrentVolumeObservation{};
        allVolumeObservationValue = V1AllVolumeObservation{};
        displayVolumeObservationValue = V1DisplayVolumeObservation{};
        bluetoothIndicatorObservationValue = V1BluetoothIndicatorObservation{};
        sweepSectionsObservationValue = V1SweepSectionsObservation{};
        sweepMaxObservationValue = V1SweepMaxObservation{};
        sweepDefinitionsObservationValue = V1SweepDefinitionsObservation{};
        sweepWriteResultObservationValue = V1SweepWriteResultObservation{};
        synthesizeSweepResponses = false;
        synthesizedSweepSections = V1SweepSectionsObservation{};
        synthesizedSweepMax = V1SweepMaxObservation{};
        synthesizedSweepDefinitions = V1SweepDefinitionsObservation{};
        parsedPackets.clear();
        parseTimestamps.clear();
        parseIngressSequences.clear();
        resetAlertAssemblyCalls = 0;
        resetAlertStateCalls = 0;
        resetV1VersionCalls = 0;
        resetModeAndDisplayStateCalls = 0;
    }

    // Test helpers — set state
    void setAlerts(const std::vector<AlertData>& a) {
        const bool hadAlerts = hasAlerts();
        alerts = a;
        hasAlertsFlag = !alerts.empty();
        if (hasAlertsFlag) {
            auto it = std::find_if(alerts.begin(), alerts.end(),
                [](const AlertData& alert) { return alert.isPriority; });
            priorityAlert = (it != alerts.end()) ? *it : alerts[0];
        }
        if (hadAlerts != hasAlerts()) {
            ++alertLifetimeValue;
        }
    }

    void setMuted(bool m)              { state.muted = m; }
    void setActiveBands(uint8_t bands) {
        const bool hadAlerts = hasAlerts();
        state.activeBands = bands;
        if (hadAlerts != hasAlerts()) {
            ++alertLifetimeValue;
        }
    }
    void setMainVolume(uint8_t vol)    { state.mainVolume = vol; state.hasVolumeData = true; }
    void resetVolumeState() {
        state.mainVolume = 0;
        state.muteVolume = 0;
        state.savedMainVolume = 0;
        state.savedMuteVolume = 0;
        state.hasVolumeData = false;
        state.hasSavedVolume = false;
        currentVolumeObservationValue = V1CurrentVolumeObservation{};
        allVolumeObservationValue = V1AllVolumeObservation{};
        displayVolumeObservationValue = V1DisplayVolumeObservation{};
    }
    void setMuteVolume(uint8_t vol)    { state.muteVolume = vol; }

    // Parser interface
    bool hasDisplayLaserAlert() const { return (state.activeBands & BAND_LASER) != 0; }
    bool hasAlerts()     const { return hasAlertsFlag || hasDisplayLaserAlert(); }
    uint32_t alertLifetime() const { return alertLifetimeValue; }
    int  getAlertCount() const { return static_cast<int>(alerts.size()); }
    AlertData getPriorityAlert() const {
        if (!hasDisplayLaserAlert()) return priorityAlert;
        AlertData laser;
        laser.band = BAND_LASER;
        laser.direction = state.arrows;
        laser.isValid = true;
        laser.isPriority = true;
        return laser;
    }

    bool getRenderablePriorityAlert(AlertData& out) const {
        auto isRenderable = [](const AlertData& a) -> bool {
            if (!a.isValid || a.band == BAND_NONE) return false;
            return (a.band == BAND_LASER) || (a.frequency != 0);
        };
        const AlertData priority = getPriorityAlert();
        if (isRenderable(priority)) { out = priority; return true; }
        for (const auto& alert : alerts) {
            if (isRenderable(alert)) { out = alert; return true; }
        }
        out = AlertData();
        return false;
    }

    const std::vector<AlertData>& getAllAlerts() const { return alerts; }
    DisplayState getDisplayState() const { return state; }
    const V1DisplayOnObservation& displayOnObservation() const { return displayOnObservationValue; }
    const V1ModeObservation& modeObservation() const { return modeObservationValue; }
    const V1CurrentVolumeObservation& currentVolumeObservation() const { return currentVolumeObservationValue; }
    const V1AllVolumeObservation& allVolumeObservation() const { return allVolumeObservationValue; }
    const V1DisplayVolumeObservation& displayVolumeObservation() const { return displayVolumeObservationValue; }
    const V1BluetoothIndicatorObservation& bluetoothIndicatorObservation() const {
        return bluetoothIndicatorObservationValue;
    }
    const V1SweepSectionsObservation& sweepSectionsObservation() const { return sweepSectionsObservationValue; }
    const V1SweepMaxObservation& sweepMaxObservation() const { return sweepMaxObservationValue; }
    const V1SweepDefinitionsObservation& sweepDefinitionsObservation() const { return sweepDefinitionsObservationValue; }
    const V1SweepWriteResultObservation& sweepWriteResultObservation() const { return sweepWriteResultObservationValue; }
    void resetSweepDefinitionsObservation() { sweepDefinitionsObservationValue = V1SweepDefinitionsObservation{}; }
    void resetSweepSectionsObservation() { sweepSectionsObservationValue = V1SweepSectionsObservation{}; }
    void resetSweepMaxObservation() { sweepMaxObservationValue = V1SweepMaxObservation{}; }
    void resetSweepCaptureState() {
        sweepSectionsObservationValue = V1SweepSectionsObservation{};
        sweepMaxObservationValue = V1SweepMaxObservation{};
        sweepDefinitionsObservationValue = V1SweepDefinitionsObservation{};
        sweepWriteResultObservationValue = V1SweepWriteResultObservation{};
    }
    uint32_t displayOnObservationRevision() const { return displayOnObservationValue.revision; }
    uint32_t modeObservationRevision() const { return modeObservationValue.revision; }
    uint32_t currentVolumeObservationRevision() const { return currentVolumeObservationValue.revision; }
    bool copyLatestCanonicalCurrentVolume(uint8_t& main, uint8_t& muted,
                                          uint32_t* ingressSequence = nullptr) const {
        uint32_t latestSequence = 0;
        bool found = false;
        if (displayVolumeObservationValue.available) {
            latestSequence = displayVolumeObservationValue.sequence;
            main = displayVolumeObservationValue.main;
            muted = displayVolumeObservationValue.muted;
            if (ingressSequence) *ingressSequence = displayVolumeObservationValue.ingressSequence;
            found = true;
        }
        if (allVolumeObservationValue.available &&
            (!found || allVolumeObservationValue.sequence >= latestSequence)) {
            latestSequence = allVolumeObservationValue.sequence;
            main = allVolumeObservationValue.currentMain;
            muted = allVolumeObservationValue.currentMuted;
            if (ingressSequence) *ingressSequence = allVolumeObservationValue.ingressSequence;
            found = true;
        }
        if (currentVolumeObservationValue.available &&
            (!found || currentVolumeObservationValue.sequence >= latestSequence)) {
            main = currentVolumeObservationValue.main;
            muted = currentVolumeObservationValue.muted;
            if (ingressSequence) *ingressSequence = currentVolumeObservationValue.ingressSequence;
            found = true;
        }
        return found;
    }

    bool parse(const uint8_t* data, size_t length, uint32_t nowMs, uint32_t ingressSequence) {
        parseCalls++;
        parsedPackets.emplace_back(data, data + length);
        parseTimestamps.push_back(nowMs);
        parseIngressSequences.push_back(ingressSequence);
        if (parseReturnValue && synthesizeSweepResponses && data && length > 3) {
            if (data[3] == 0x23) sweepSectionsObservationValue = synthesizedSweepSections;
            else if (data[3] == 0x20) sweepMaxObservationValue = synthesizedSweepMax;
            else if (data[3] == 0x17) sweepDefinitionsObservationValue = synthesizedSweepDefinitions;
        }
        return parseReturnValue;
    }

    bool parse(const uint8_t* data, size_t length, uint32_t nowMs) {
        return parse(data, length, nowMs, 0);
    }

    bool parse(const uint8_t* data, size_t length) { return parse(data, length, 0); }

    // Reset methods
    int resetAlertAssemblyCalls = 0;
    void resetAlertAssembly() { resetAlertAssemblyCalls++; }
    int resetAlertStateCalls = 0;
    void resetAlertState() {
        ++alertLifetimeValue;
        resetAlertStateCalls++;
        alerts.clear();
        priorityAlert = AlertData();
        hasAlertsFlag = false;
        state.priorityArrow = DIR_NONE;
        state.activeBands = BAND_NONE;
        state.arrows = DIR_NONE;
        state.signalBars = 0;
        state.flashBits = 0;
        state.bandFlashBits = 0;
        state.v1PriorityIndex = 0;
        state.hasJunkAlert = false;
        state.hasPhotoAlert = false;
        state.hasKuAlert = false;
    }
    int resetV1VersionCalls = 0;
    void resetV1Version() {
        resetV1VersionCalls++;
        state.v1FirmwareVersion = 0;
        state.hasV1Version = false;
    }
    int resetModeAndDisplayStateCalls = 0;
    void resetModeAndDisplayState() {
        resetModeAndDisplayStateCalls++;
        state.modeChar = 0;
        state.hasMode = false;
        state.displayOn = true;
        state.hasDisplayOn = false;
        displayOnObservationValue = V1DisplayOnObservation{};
        modeObservationValue = V1ModeObservation{};
        bluetoothIndicatorObservationValue = V1BluetoothIndicatorObservation{};
        resetSweepCaptureState();
    }
};

#endif // PACKET_PARSER_H
