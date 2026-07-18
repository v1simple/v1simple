/**
 * ESP Packet Parser for V1 Gen2
 * Decodes display data and alert data packets
 */

#pragma once
#ifndef PACKET_PARSER_H
#define PACKET_PARSER_H

#include <Arduino.h>
#include <array>
#include <vector>

// Shared data types — Band, Direction, AlertData, DisplayState.
// Defined in a standalone header so mocks can include the same definitions
// without pulling in Arduino.h.
#include "packet_parser_types.h"

class PacketParser {
  public:
    static constexpr size_t MAX_ALERTS = 15; // V1 spec supports up to 15 simultaneous alerts

    PacketParser();

    // Parse incoming ESP packet
    bool parse(const uint8_t* data, size_t length);
    bool parse(const uint8_t* data, size_t length, uint32_t nowMs);

    // Get current display state
    const DisplayState& getDisplayState() const { return displayState_; }

    // Get resolved priority alert (follows V1 priority signal)
    AlertData getPriorityAlert() const;

    // Get a renderable priority alert (valid band + usable frequency semantics).
    // Returns true and writes to out when a renderable alert exists.
    bool getRenderablePriorityAlert(AlertData& out) const;

    // Get all alerts
    const std::array<AlertData, MAX_ALERTS>& getAllAlerts() const { return alerts_; }

    // Get number of active alerts
    size_t getAlertCount() const { return alertCount_; }

    // Check if there are active alerts
    // Only check alertCount_ - display state can lag behind
    bool hasAlerts() const { return alertCount_ > 0; }

    // Check if V1 firmware supports volume display
    // Show volume if we've received volume data OR confirmed firmware version 4.1028+
    bool supportsVolume() const {
        return displayState_.hasVolumeData || (displayState_.hasV1Version && displayState_.v1FirmwareVersion >= 41028);
    }

    // Clear any partially assembled alert chunks (used when we re-request alert data)
    void resetAlertAssembly();

    // Clear partial and published alert state at a V1 session boundary.
    void resetAlertState();

#ifdef UNIT_TEST
    bool validatePacketForTest(const uint8_t* data, size_t length) { return validatePacket(data, length); }
#endif

  private:
    static constexpr size_t RAW_ALERT_INDEX_SLOTS = MAX_ALERTS + 1; // raw indexes 0..15

    DisplayState displayState_;
    std::array<AlertData, MAX_ALERTS> alerts_;
    size_t alertCount_;
    uint8_t displayMuteConfirmCount_ = 0; // consecutive display packets with mute bit set
    std::array<std::array<uint8_t, 8>, RAW_ALERT_INDEX_SLOTS> alertChunks_; // raw alert rows by payload index
    std::array<bool, RAW_ALERT_INDEX_SLOTS> alertChunkPresent_;
    std::array<uint8_t, RAW_ALERT_INDEX_SLOTS> alertChunkCountTag_;
    std::array<uint32_t, RAW_ALERT_INDEX_SLOTS> alertChunkRxMs_;
    std::array<uint32_t, MAX_ALERTS + 1> alertTableFirstSeenMs_;

    // Packet parsing helpers
    bool parseInternal(const uint8_t* data, size_t length, bool hasNowMs, uint32_t nowMs);
    bool parseDisplayData(const uint8_t* payload, size_t length);
    bool parseAlertData(const uint8_t* payload, size_t length, uint32_t nowMs);
    bool validatePacket(const uint8_t* data, size_t length);
    void clearAlertCache();
    void clearAlertCacheForCount(uint8_t count);
    void clearPublishedAlerts();

    // Data extraction
    Band decodeBand(uint8_t bandArrow) const;
    Direction decodeDirection(uint8_t bandArrow) const;
    uint8_t decodeLEDBitmap(uint8_t bitmap) const; // V1 Gen2 LED bitmap -> local 0-8 display bars
    uint8_t mapStrengthToBars(Band band,
                              uint8_t raw) const; // Per-alert RSSI -> 0-6 card/history scale (not the main meter)
    void decodeMode(const uint8_t* payload, size_t length);
};
#endif // PACKET_PARSER_H
