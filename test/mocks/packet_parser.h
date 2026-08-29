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
    int parseCalls = 0;
    bool parseReturnValue = true;
    std::vector<std::vector<uint8_t>> parsedPackets;
    std::vector<uint32_t> parseTimestamps;

    void reset() {
        state = DisplayState();
        alerts.clear();
        priorityAlert = AlertData();
        hasAlertsFlag = false;
        parseCalls = 0;
        parseReturnValue = true;
        parsedPackets.clear();
        parseTimestamps.clear();
        resetAlertAssemblyCalls = 0;
        resetAlertStateCalls = 0;
        resetV1VersionCalls = 0;
    }

    // Test helpers — set state
    void setAlerts(const std::vector<AlertData>& a) {
        alerts = a;
        hasAlertsFlag = !alerts.empty();
        if (hasAlertsFlag) {
            auto it = std::find_if(alerts.begin(), alerts.end(),
                [](const AlertData& alert) { return alert.isPriority; });
            priorityAlert = (it != alerts.end()) ? *it : alerts[0];
        }
    }

    void setMuted(bool m)              { state.muted = m; }
    void setActiveBands(uint8_t bands) { state.activeBands = bands; }
    void setMainVolume(uint8_t vol)    { state.mainVolume = vol; state.hasVolumeData = true; }
    void setMuteVolume(uint8_t vol)    { state.muteVolume = vol; }

    // Parser interface
    bool hasDisplayLaserAlert() const { return (state.activeBands & BAND_LASER) != 0; }
    bool hasAlerts()     const { return hasAlertsFlag || hasDisplayLaserAlert(); }
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

    bool parse(const uint8_t* data, size_t length, uint32_t nowMs) {
        parseCalls++;
        parsedPackets.emplace_back(data, data + length);
        parseTimestamps.push_back(nowMs);
        return parseReturnValue;
    }

    bool parse(const uint8_t* data, size_t length) { return parse(data, length, 0); }

    // Reset methods
    int resetAlertAssemblyCalls = 0;
    void resetAlertAssembly() { resetAlertAssemblyCalls++; }
    int resetAlertStateCalls = 0;
    void resetAlertState() {
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
};

#endif // PACKET_PARSER_H
