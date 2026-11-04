#pragma once

#include <cstddef>
#include <cstdint>

#include "display_ble_context.h"
struct V1Settings;

// Values sampled by the runtime; no renderer may query a live producer.
struct PresentationStatusSnapshot {
    DisplayBleContext ble{};
    uint32_t bleSampledAtMs = 0;
    uint32_t bleBadgeSampledAtMs = 0;
    uint16_t batteryMillivolts = 0;
    uint8_t batteryPercent = 0;
    uint8_t gpsSatellites = 0;
    uint8_t alpState = 0;
    uint8_t alpHeartbeat = 0;
    bool bleReceiving = true;
    bool bleProxyEnabled = false;
    bool bleProxyClientConnected = false;
    bool obdEnabled = false;
    bool obdConnected = false;
    bool obdScanInProgress = false;
    bool obdManualScanPending = false;
    bool obdAttention = false;
    bool gpsEnabled = false;
    bool gpsStableFix = false;
    bool alpEnabled = false;
    bool hasBattery = false;
    bool wifiServiceActive = false;
    bool wifiConnected = false;
    bool wifiGaveUp = false;
    bool speedVolZeroActive = false;
    bool hasGunLabel = false;
    bool gunLabelLidActive = true;
    char gunLabel[16]{};
};
static_assert(sizeof(PresentationStatusSnapshot) <= 128, "Presentation snapshot must stay bounded");

// Synchronous borrow only. Settings and slot policy remain outside rendering.
struct DisplayStyleView {
    const V1Settings& settings;
    uint32_t configurationRevision;
    uint8_t alertPersistSec;
    bool priorityArrowOnly;
};

enum class CardPersistencePolicy : uint8_t { LiveGrace, PreviewImmediate };
struct DisplayPaintContext {
    uint32_t nowMs;
    const DisplayStyleView& style;
    const PresentationStatusSnapshot& status;
    CardPersistencePolicy cardPersistence = CardPersistencePolicy::LiveGrace;
};

struct PresentationInputs {
    PresentationStatusSnapshot status;
    DisplayStyleView style;
    uint32_t nowMs;
    DisplayPaintContext context(CardPersistencePolicy policy = CardPersistencePolicy::LiveGrace) const {
        return {nowMs, style, status, policy};
    }
};

// One synchronous visual resolution, borrowed by both keys and painters.
// Its context and text pointers never escape the current paint call.
struct ResolvedPaintStatus {
    const DisplayPaintContext& context;
    const char* profileName = "DEFAULT";
    const char* gunLabel = "";
    int profileSlot = 0;
    int v1Rssi = 0;
    int proxyRssi = 0;
    uint16_t profileColor = 0;
    uint16_t obdColor = 0;
    uint16_t alpColor = 0;
    uint16_t wifiColor = 0;
    uint16_t bleColor = 0;
    uint16_t v1RssiColor = 0;
    uint16_t proxyRssiColor = 0;
    uint16_t batteryFillColor = 0;
    uint16_t batteryTextColor = 0;
    uint16_t batteryOutlineColor = 0;
    uint8_t batteryPercent = 0;
    uint8_t batterySections = 0;
    uint8_t batteryVoltageBand = 0;
    uint8_t gpsSatellites = 0;
    uint8_t alpState = 0;
    uint8_t alpHeartbeat = 0;
    bool profileFlashActive = false;
    bool profileHidden = false;
    bool bleFresh = false;
    bool bleShown = false;
    bool obdShown = false;
    bool obdConnected = false;
    bool obdAttention = false;
    bool gpsShown = false;
    bool alpShown = false;
    bool alpAlertActive = false;
    bool wifiShown = false;
    bool batteryShown = false;
    bool batteryPercentMode = false;
    bool batteryShowOnUsb = true;
    bool hasGunLabel = false;
    bool gunLabelLidActive = false;
};
static_assert(sizeof(ResolvedPaintStatus) <= 128, "Resolved presentation must stay bounded");
