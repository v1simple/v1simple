#pragma once
#ifndef PACKET_PARSER_H
#define PACKET_PARSER_H

#include <Arduino.h>
#include <array>
#include <vector>

#include "packet_parser_types.h"

class PacketParser {
  public:
    static constexpr size_t MAX_ALERTS = 15; // V1 spec supports up to 15 simultaneous alerts
    using AlertTableObserver =
        void (*)(const AlertData* alerts, size_t count, uint8_t priorityIndex, uint32_t nowMs, void* context);

    PacketParser();

    bool parse(const uint8_t* data, size_t length);
    bool parse(const uint8_t* data, size_t length, uint32_t nowMs);
    bool parse(const uint8_t* data, size_t length, uint32_t nowMs, uint32_t ingressSequence);

    const DisplayState& getDisplayState() const { return displayState_; }

    // Session-scoped settings evidence. Values and revisions are updated
    // atomically from canonical packets only; Apply never pairs a canonical
    // revision with later tolerant DisplayState data.
    const V1DisplayOnObservation& displayOnObservation() const { return displayOnObservation_; }
    const V1ModeObservation& modeObservation() const { return modeObservation_; }
    // Dedicated respCurrentVolume (0x38) evidence. A delayed respAllVolume
    // must not satisfy a post-write 0x37 verification request.
    const V1CurrentVolumeObservation& currentVolumeObservation() const { return currentVolumeObservation_; }
    const V1AllVolumeObservation& allVolumeObservation() const { return allVolumeObservation_; }
    const V1DisplayVolumeObservation& displayVolumeObservation() const { return displayVolumeObservation_; }
    const V1BluetoothIndicatorObservation& bluetoothIndicatorObservation() const {
        return bluetoothIndicatorObservation_;
    }
    const V1SweepSectionsObservation& sweepSectionsObservation() const { return sweepSectionsObservation_; }
    const V1SweepMaxObservation& sweepMaxObservation() const { return sweepMaxObservation_; }
    const V1SweepDefinitionsObservation& sweepDefinitionsObservation() const {
        return sweepDefinitionsObservation_;
    }
    const V1SweepWriteResultObservation& sweepWriteResultObservation() const {
        return sweepWriteResultObservation_;
    }
    bool copyLatestCanonicalCurrentVolume(uint8_t& main, uint8_t& muted,
                                          uint32_t* ingressSequence = nullptr) const;
    uint32_t displayOnObservationRevision() const { return displayOnObservation_.revision; }
    uint32_t modeObservationRevision() const { return modeObservation_.revision; }
    uint32_t currentVolumeObservationRevision() const { return currentVolumeObservation_.revision; }
    void resetSweepSectionsObservation();
    void resetSweepMaxObservation();
    void resetSweepDefinitionsObservation();
    void resetSweepWriteResultObservation();
    void resetSweepCaptureState();

    AlertData getPriorityAlert() const;

    // Get a renderable priority alert (valid band + usable frequency semantics).
    // Returns true and writes to out when a renderable alert exists.
    bool getRenderablePriorityAlert(AlertData& out) const;

    const std::array<AlertData, MAX_ALERTS>& getAllAlerts() const { return alerts_; }

    size_t getAlertCount() const { return alertCount_; }

    // The Alert Table contains radar only. A V1 laser is live from
    // InfDisplayData even when the table has zero rows.
    bool hasAlerts() const { return alertCount_ > 0 || hasDisplayLaserAlert(); }

    // Changes on aggregate alert start/end and session reset, including when
    // multiple publications occur before a consumer next observes the parser.
    uint32_t alertLifetime() const { return alertLifetime_; }

    bool supportsVolume() const {
        return displayState_.hasVolumeData || (displayState_.hasV1Version && displayState_.v1FirmwareVersion >= 41028);
    }

    void resetAlertAssembly();

    // A known transport gap makes every partial Alert Table ambiguous. Keep the
    // last complete publication, but require a table-start row before collecting
    // another candidate.
    void markAlertStreamDiscontinuous();

    // Clear partial and published alert state at a V1 session boundary.
    void resetAlertState();

    // Invalidate detector-version knowledge at a V1 session boundary.
    void resetV1Version();

    // Volume values are authoritative only within the link that reported them.
    void resetVolumeState();

    // Mode and display observations belong to one detector link and must not
    // leak into a snapshot captured from a later connection.
    void resetModeAndDisplayState();

    // Observe complete V1 alert tables. The callback runs on the parser path,
    // so it must remain non-blocking and allocation-free.
    void setAlertTableObserver(AlertTableObserver observer, void* context = nullptr) {
        alertTableObserver_ = observer;
        alertTableObserverContext_ = context;
    }

#ifdef UNIT_TEST
    bool validatePacketForTest(const uint8_t* data, size_t length) { return validatePacket(data, length); }
#endif

  private:
    static constexpr size_t RAW_ALERT_INDEX_SLOTS = MAX_ALERTS + 1; // raw indexes 0..15

    enum class AlertIndexMode : uint8_t {
        Unknown = 0,
        ZeroBased,
        OneBased,
    };

    DisplayState displayState_;

    std::array<AlertData, MAX_ALERTS> alerts_;
    size_t alertCount_;
    uint32_t alertLifetime_ = 0;
    V1DisplayOnObservation displayOnObservation_;
    V1ModeObservation modeObservation_;
    V1CurrentVolumeObservation currentVolumeObservation_;
    V1AllVolumeObservation allVolumeObservation_;
    V1DisplayVolumeObservation displayVolumeObservation_;
    V1BluetoothIndicatorObservation bluetoothIndicatorObservation_;
    V1SweepSectionsObservation sweepSectionsObservation_;
    V1SweepMaxObservation sweepMaxObservation_;
    V1SweepDefinitionsObservation sweepDefinitionsObservation_;
    V1SweepWriteResultObservation sweepWriteResultObservation_;
    uint32_t settingsObservationSequence_ = 0;
    uint8_t displayMuteConfirmCount_ = 0; // consecutive display packets with mute bit set
    std::array<std::array<uint8_t, 8>, RAW_ALERT_INDEX_SLOTS> alertChunks_; // raw alert rows by payload index
    std::array<bool, RAW_ALERT_INDEX_SLOTS> alertChunkPresent_;
    std::array<uint8_t, RAW_ALERT_INDEX_SLOTS> alertChunkCountTag_;
    std::array<uint32_t, RAW_ALERT_INDEX_SLOTS> alertChunkRxMs_;
    std::array<uint32_t, MAX_ALERTS + 1> alertTableFirstSeenMs_;
    AlertIndexMode alertIndexMode_ = AlertIndexMode::Unknown;
    bool alertResyncRequired_ = false;
    AlertTableObserver alertTableObserver_ = nullptr;
    void* alertTableObserverContext_ = nullptr;

    bool hasDisplayLaserAlert() const { return (displayState_.activeBands & BAND_LASER) != 0; }

    bool parseInternal(const uint8_t* data, size_t length, bool hasNowMs, uint32_t nowMs,
                       uint32_t ingressSequence);
    bool parseDisplayData(const uint8_t* payload, size_t length);
    bool parseAlertData(const uint8_t* payload, size_t length, uint32_t nowMs);
    bool validatePacket(const uint8_t* data, size_t length);
    void clearAlertCache();
    void clearAlertCacheForCount(uint8_t count);
    void clearPublishedAlerts();
    void resetAlertStateAt(uint32_t nowMs);
    void notifyAlertTableObserver(uint32_t nowMs);

    Band decodeBand(uint8_t bandArrow) const;
    Direction decodeDirection(uint8_t bandArrow) const;
    uint8_t decodeLEDBitmap(uint8_t bitmap) const; // Bar graph byte -> 0-8 LEDs, ESP Spec 3.015 Table 9.1
    uint8_t mapStrengthToBars(Band band,
                              uint8_t raw) const; // Per-alert RSSI -> 0-8 Table 9.1 scale (not the main meter)
    void decodeMode(const uint8_t* payload, size_t length);
};
#endif // PACKET_PARSER_H
