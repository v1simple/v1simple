/**
 * Stable settings data, update, and result types.
 *
 * This header deliberately excludes persistence, filesystem, and JSON manager
 * dependencies so runtime consumers can use settings values without inheriting
 * the storage implementation.
 */

#pragma once
#ifndef SETTINGS_TYPES_H
#define SETTINGS_TYPES_H

#include <Arduino.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "color_themes.h"

// V1 operating modes (from ESP library)
enum V1Mode {
    V1_MODE_UNKNOWN = 0x00,
    V1_MODE_ALL_BOGEYS = 0x01,    // All Bogeys (K+Ka) or Custom Sweeps
    V1_MODE_LOGIC = 0x02,         // Logic mode (Ka only)
    V1_MODE_ADVANCED_LOGIC = 0x03 // Advanced Logic
};

inline constexpr bool kDefaultAutoPushEnabled = true;

#ifndef CONNECTION_CYCLE_SETTINGS_CONSTANTS_DEFINED
#define CONNECTION_CYCLE_SETTINGS_CONSTANTS_DEFINED
inline constexpr uint32_t kConnectionCycleObdScanWindowMsDefault = 15000;
inline constexpr uint32_t kConnectionCycleObdScanWindowMsMin = 1000;
inline constexpr uint32_t kConnectionCycleObdScanWindowMsMax = 60000;

inline constexpr uint32_t kConnectionCycleObdRetryIntervalMsDefault = 120000;
inline constexpr uint32_t kConnectionCycleObdRetryIntervalMsMin = 30000;
inline constexpr uint32_t kConnectionCycleObdRetryIntervalMsMax = 600000;

inline constexpr uint32_t kConnectionCycleProxyOpenWindowMsDefault = 60000;
inline constexpr uint32_t kConnectionCycleProxyOpenWindowMsMin = 1000;
inline constexpr uint32_t kConnectionCycleProxyOpenWindowMsMax = 300000;

inline constexpr uint32_t kConnectionCycleV1SettleQuietMsDefault = 500;
inline constexpr uint32_t kConnectionCycleV1SettleQuietMsMin = 100;
inline constexpr uint32_t kConnectionCycleV1SettleQuietMsMax = 5000;

inline constexpr uint32_t kConnectionCycleV1SettleFallbackMsDefault = 1500;
inline constexpr uint32_t kConnectionCycleV1SettleFallbackMsMin = 500;
inline constexpr uint32_t kConnectionCycleV1SettleFallbackMsMax = 10000;

inline constexpr uint32_t kConnectionCycleTeardownAckTimeoutMsDefault = 100;
inline constexpr uint32_t kConnectionCycleTeardownAckTimeoutMsMin = 25;
inline constexpr uint32_t kConnectionCycleTeardownAckTimeoutMsMax = 1000;
#endif

// Auto-push profile slot
struct AutoPushSlot {
    String profileName;
    V1Mode mode;

    AutoPushSlot() : profileName(""), mode(V1_MODE_UNKNOWN) {}
    AutoPushSlot(const String& name, V1Mode m) : profileName(name), mode(m) {}
};

struct AutoPushPersistResult {
    bool success = false;
    bool changed = false;
};

// Outcome of one settings mutation and its requested persistence boundary.
// `success` means that boundary completed: Deferred means the NVS write was
// queued, while both Immediate modes mean NVS committed before return.
// `deferred` is deliberately narrower: it reports only that the SD backup is
// pending after a successful NVS commit; it never describes an NVS deferral.
struct SettingsPersistResult {
    bool success = false;
    bool changed = false;
    bool deferred = false;
};

inline constexpr size_t kWifiStaSlotCount = 4;

// Saved STA network metadata. Passwords intentionally stay outside the main
// settings struct in the v1wificlient NVS namespace.
struct WifiStaSlot {
    String ssid;
    String label;
    uint8_t priority;
    // Compatibility note: this member and its persisted/API/backup key retain
    // the historical `lastConnectedAtSec` name, but the value is a logical
    // connection-order token, not uptime or wall-clock seconds.  Persisting a
    // logical token keeps equal-priority recency comparable across reboot and
    // millis() wrap without inventing a clock the device does not have.
    uint32_t lastConnectedAtSec;

    WifiStaSlot() : ssid(""), label(""), priority(0), lastConnectedAtSec(0) {}

    bool isConfigured() const { return ssid.length() > 0; }
};

struct WifiStaPriorityUpdate {
    size_t index = 0;
    uint8_t priority = 0;
};

// Settings structure
// Voice alert content mode
enum VoiceAlertMode {
    VOICE_MODE_DISABLED = 0,  // Voice alerts disabled
    VOICE_MODE_BAND_ONLY = 1, // Just band name ("Ka")
    VOICE_MODE_FREQ_ONLY = 2, // Just frequency ("34.7")
    VOICE_MODE_BAND_FREQ = 3  // Band + frequency ("Ka 34.7")
};

struct V1Settings {
    // WiFi settings
    String apSSID;            // AP mode SSID (device hotspot name)
    String apPassword;        // AP mode password

    // WiFi client (STA) settings - connect to external network
    bool wifiClientEnabled;                      // Enable WiFi client mode (AP+STA dual mode)
    WifiStaSlot wifiStaSlots[kWifiStaSlotCount]; // Saved STA networks
    String wifiClientSSID;                       // Compatibility alias: primary saved STA SSID
    // wifiClientPassword is stored separately in the secure NVS namespace.

    // BLE proxy settings
    bool proxyBLE;    // Enable BLE proxy for companion app
    String proxyName; // BLE device name when proxying

    // Display settings
    uint8_t brightness;

    // Custom display colors (RGB565 format)
    uint16_t colorBogey;           // Bogey counter color
    uint16_t colorFrequency;       // Frequency display color
    uint16_t colorArrowFront;      // Front arrow color
    uint16_t colorArrowSide;       // Side arrow color
    uint16_t colorArrowRear;       // Rear arrow color
    uint16_t colorBandL;           // Laser band color
    uint16_t colorBandKa;          // Ka band color
    uint16_t colorBandK;           // K band color
    uint16_t colorBandX;           // X band color
    uint16_t colorBandPhoto;       // Photo radar color (when V1 sends 'P')
    uint16_t colorWiFiConnected;   // WiFi icon when client connected
    uint16_t colorBleConnected;    // Bluetooth icon when client connected
    uint16_t colorBleDisconnected; // Bluetooth icon when no client
    // Signal bar colours, one per meter segment, index 0 = bottom/weakest.
    // Each of the six segments is individually addressable; there is no
    // runtime interpolation between stops.
    uint16_t colorBars[SIGNAL_BAR_COLOR_COUNT];
    uint16_t colorMuted;        // Muted alert color (shown when alerts are muted/grayed)
    uint16_t colorPersisted;    // Persisted alert color (shown after alert disappears)
    uint16_t colorVolumeMain;   // Volume indicator main volume color
    uint16_t colorVolumeMute;   // Volume indicator muted volume color
    uint16_t colorRssiV1;       // RSSI indicator V1 label color
    uint16_t colorRssiProxy;    // RSSI indicator Proxy label color
    uint16_t colorObd;          // OBD "OBD" status text color when connected
    uint16_t colorAlpConnected; // ALP badge: green — connected, idle / warm-up
    uint16_t colorAlpDli;       // ALP badge: orange — DLI active (below LID speed limit)
    uint16_t colorAlpLidActive; // ALP badge: blue — LID active (above LID speed limit, IR-capable)
    uint16_t colorAlpAlert;     // ALP badge: red — active laser alert while live
    bool freqUseBandColor;      // Use band color for frequency display instead of custom freq color

    // Display visibility settings
    bool hideWifiIcon;         // Hide WiFi icon after brief display
    bool hideProfileIndicator; // Hide profile indicator after brief display
    bool hideBatteryIcon;      // Hide battery icon
    bool showBatteryPercent;   // Show battery percentage text next to icon
    bool hideBleIcon;          // Hide BLE icon
    bool hideVolumeIndicator;  // Hide volume indicator (V1 firmware 4.1028+ only)
    bool hideRssiIndicator;    // Hide RSSI signal strength indicator

    // Voice alerts (when no app connected)
    VoiceAlertMode voiceAlertMode; // What content to speak (disabled/band/freq/band+freq)
    bool voiceDirectionEnabled;    // Append direction (ahead/side/behind) to voice
    bool announceBogeyCount;       // Announce bogey count after direction
    bool muteVoiceIfVolZero;       // Mute voice alerts when V1 volume is 0
    uint8_t voiceVolume;           // Voice alert volume (0-100%)

    // Secondary alert announcements (non-priority alerts)
    bool announceSecondaryAlerts; // Master toggle for secondary announcements
    bool secondaryLaser;          // Announce secondary Laser alerts
    bool secondaryKa;             // Announce secondary Ka alerts
    bool secondaryK;              // Announce secondary K alerts
    bool secondaryX;              // Announce secondary X alerts

    // Volume fade (reduce V1 volume after initial alert period)
    bool alertVolumeFadeEnabled;     // Enable volume fade feature
    uint8_t alertVolumeFadeDelaySec; // Seconds at full volume before fading (1-10)
    uint8_t alertVolumeFadeVolume;   // Volume to fade to (1-9; 0 triggers V1 mute feedback loop)

    // Speed-aware muting (suppress alerts below speed threshold)
    bool speedMuteEnabled;          // Enable speed-based auto-muting
    uint8_t speedMuteThresholdMph;  // Mute below this speed (5-60 mph)
    uint8_t speedMuteHysteresisMph; // Unmute at threshold + hysteresis (1-10 mph)
    uint8_t speedMuteVolume;        // V1 volume when speed-muted (0-9)
    bool speedMuteVoice;            // Also suppress voice announcements when speed-muted
    bool stealthEnabled;            // Enable stealth (blank) screen with OBD speed when idle

    // Auto-push on connection settings
    bool autoPushEnabled; // Enable auto-push profile on V1 connection
    uint8_t autoPushProfileSchemaVersion; // 0 until legacy slot detector fields are transactionally migrated
    int activeSlot;       // Which slot is active: 0=Default, 1=Highway, 2=Comfort
    String slot0Name;     // Custom display name for slot 0 (default: "DEFAULT")
    String slot1Name;     // Custom display name for slot 1 (default: "HIGHWAY")
    String slot2Name;     // Custom display name for slot 2 (default: "COMFORT")
    uint16_t slot0Color;  // Custom color for slot 0 display (default: purple 0x780F)
    uint16_t slot1Color;  // Custom color for slot 1 display (default: green 0x07E0)
    uint16_t slot2Color;  // Custom color for slot 2 display (default: grey 0x8410)
    // V1 writes these as one atomic pair: both are 0-9, or both are 0xFF=no change.
    uint8_t slot0Volume;
    uint8_t slot1Volume;
    uint8_t slot2Volume;
    uint8_t slot0MuteVolume;
    uint8_t slot1MuteVolume;
    uint8_t slot2MuteVolume;
    bool slot0DarkMode;   // V1 display off (dark mode) for slot 0
    bool slot1DarkMode;   // V1 display off (dark mode) for slot 1
    bool slot2DarkMode;   // V1 display off (dark mode) for slot 2
    // Explicit opt-ins keep migrated profile-owned slots from applying stale legacy values.
    bool slot0VolumeOverride;
    bool slot1VolumeOverride;
    bool slot2VolumeOverride;
    bool slot0DarkModeOverride;
    bool slot1DarkModeOverride;
    bool slot2DarkModeOverride;
    bool slot0MuteToZero; // Mute to zero for slot 0
    bool slot1MuteToZero; // Mute to zero for slot 1
    bool slot2MuteToZero; // Mute to zero for slot 2
    // Shared persistence window for both V1 radar persistence and ALP laser-event
    // persistence. The display pipeline intentionally reuses this per-slot knob
    // rather than adding a second ALP-specific setting or NVS key.
    uint8_t slot0AlertPersist; // Alert persistence (seconds) for slot 0 (0-5s)
    uint8_t slot1AlertPersist; // Alert persistence (seconds) for slot 1 (0-5s)
    uint8_t slot2AlertPersist; // Alert persistence (seconds) for slot 2 (0-5s)
    bool slot0PriorityArrow;   // Priority arrow only for slot 0
    bool slot1PriorityArrow;   // Priority arrow only for slot 1
    bool slot2PriorityArrow;   // Priority arrow only for slot 2
    AutoPushSlot slot0_default;
    AutoPushSlot slot1_highway;
    AutoPushSlot slot2_comfort;

    struct AutoPushSlotView {
        String& name;
        uint16_t& color;
        uint8_t& volume;
        uint8_t& muteVolume;
        bool& darkMode;
        bool& volumeOverride;
        bool& darkModeOverride;
        bool& muteToZero;
        uint8_t& alertPersist;
        bool& priorityArrow;
        AutoPushSlot& config;
    };

    struct ConstAutoPushSlotView {
        const String& name;
        const uint16_t& color;
        const uint8_t& volume;
        const uint8_t& muteVolume;
        const bool& darkMode;
        const bool& volumeOverride;
        const bool& darkModeOverride;
        const bool& muteToZero;
        const uint8_t& alertPersist;
        const bool& priorityArrow;
        const AutoPushSlot& config;
    };

    String lastV1Address; // Runtime/backup fallback; V1DeviceStore owns connected-device durability

    // Auto power-off on V1 disconnect
    uint8_t autoPowerOffMinutes; // Minutes to wait after V1 disconnect or ALP silence before power off (0=disabled)
    uint8_t apTimeoutMinutes;    // Minutes before AP auto-stops (0=always on, 5-60)

    // OBD-II speed source settings
    bool obdEnabled;          // Enable OBD module
    String obdSavedAddress;   // Saved OBDLink CX BLE address for auto-reconnect
    String obdSavedName;      // Optional friendly name for the saved OBD adapter
    uint8_t obdSavedAddrType; // Saved BLE address type (0=public, 1=random)
    int8_t obdMinRssi;        // Minimum RSSI for scan acceptance (dBm)

    // Connection cycle coordinator settings
    uint32_t obdScanWindowMs;           // OBD discovery window after V1 settles
    uint32_t obdRetryIntervalMs;        // OBD reconnect interval when proxy is idle
    uint32_t proxyOpenWindowMs;         // Passive proxy advertising window after OBD work
    uint32_t v1SettleQuietMs;           // Quiet time after VerifyPush match before OBD scan
    uint32_t v1SettleFallbackMs;        // Quiet time when auto-push is disabled
    uint32_t cycleTeardownAckTimeoutMs; // Per-step teardown ack timeout

    // ALP (Active Laser Protection) settings
    bool alpEnabled; // Enable ALP UART listener module
    // Laser display persistence (seconds) after an ALP session closes.
    // Defaults to 0 — the ALP has its own speaker and users found the
    // post-engagement tail on the display unhelpful after a real hit.
    // Clamped to 0..5 like the V1 slot equivalent. Not per-slot because
    // ALP is a peer source, not tied to V1 profile slots.
    uint8_t alpAlertPersistSec;
    // When ALP is enabled, clear the V1 Gen2 laser-alert bit from any
    // profile bytes pushed to the detector. The saved profile remains intact;
    // disabling this setting restores the profile's own laser bit on the next
    // push.
    bool alpDisableV1LaserOnPush;

    // GPS (optional hardware — Adafruit Ultimate GPS v3 / MTK3339)
    bool gpsEnabled;             // Enable GPS runtime module
    uint32_t gpsBaud;            // UART baud rate (9600 / 38400 / 115200)

    // Default constructor with sensible defaults
    V1Settings()
        : apSSID("V1-Simple"), apPassword("setupv1simple"),
          wifiClientEnabled(false),                                   // WiFi client disabled by default
          wifiClientSSID(""),                                         // No saved network
          proxyBLE(true), proxyName("V1-Proxy"),                      // Must match NVS load() default
          brightness(200), colorBogey(0xF800), // Red (same as KA)
          colorFrequency(0xF800),                                     // Red (same as KA)
          colorArrowFront(0xF800),                                    // Red (front)
          colorArrowSide(0xF800),                                     // Red (side)
          colorArrowRear(0xF800),                                     // Red (rear)
          colorBandL(0x001F),                                         // Blue (laser)
          colorBandKa(0xF800),                                        // Red
          colorBandK(0x001F),                                         // Blue
          colorBandX(0x07E0),                                         // Green
          colorBandPhoto(0x780F),                                     // Purple (photo radar)
          colorWiFiConnected(0x07E0),                                 // Green (WiFi client connected)
          colorBleConnected(0x07E0),                                  // Green (BLE connected)
          colorBleDisconnected(0x001F),                               // Blue (BLE disconnected)
          // One color per six physical segments, weakest to strongest.
          colorBars{0x07E0, 0x07E0, 0xFFE0, 0xFFE0, 0xF800, 0xF800},
          colorMuted(0x4A49),                   // Subdued grey (muted alerts) — matches NVS default
          colorPersisted(0x4208),               // Subdued grey (persisted alerts) — matches NVS default
          colorVolumeMain(0xF800),              // Red (volume bar) — matches NVS default
          colorVolumeMute(0x7BEF),              // Grey (muted volume) — matches NVS default
          colorRssiV1(0x07E0),                  // Green (V1 RSSI label) — matches NVS default
          colorRssiProxy(0x001F),               // Blue (proxy RSSI label) — matches NVS default
          colorObd(0x001F),                     // Blue OBD badge (matches existing BLE disconnected icon default)
          colorAlpConnected(0x07E0),            // Green ALP badge — connected, idle
          colorAlpDli(0xFD20),                  // Orange ALP badge — DLI active (below LID speed limit)
          colorAlpLidActive(0x001F),            // Blue ALP badge — LID active (above LID speed limit)
          colorAlpAlert(0xF800),                // Red ALP badge — active laser alert (solid)
          freqUseBandColor(false),              // Use custom freq color by default
          hideWifiIcon(false),                  // Show WiFi icon by default
          hideProfileIndicator(false),          // Show profile indicator by default
          hideBatteryIcon(false),               // Show battery icon by default
          showBatteryPercent(false),            // Hide battery % text by default — matches NVS default
          hideBleIcon(false),                   // Show BLE icon by default
          hideVolumeIndicator(false),           // Show volume indicator by default
          hideRssiIndicator(false),             // Show RSSI indicator by default — matches NVS default
          voiceAlertMode(VOICE_MODE_BAND_FREQ), // Full band+freq announcements by default
          voiceDirectionEnabled(true),          // Include direction by default
          announceBogeyCount(true),             // Announce bogey count by default
          muteVoiceIfVolZero(false),            // Don't mute voice alerts at vol 0 by default
          voiceVolume(75),                      // Voice alerts at 75% volume by default
          announceSecondaryAlerts(false),       // Secondary alerts off by default (opt-in)
          secondaryLaser(true),                 // Laser always important
          secondaryKa(true),                    // Ka usually real threats
          secondaryK(false),                    // K has more false positives
          secondaryX(false),                    // X is rare
          alertVolumeFadeEnabled(false),        // Volume fade disabled by default
          alertVolumeFadeDelaySec(2),           // 2 seconds at full volume before fade
          alertVolumeFadeVolume(1),             // Fade to volume 1 (quiet but audible)
          speedMuteEnabled(false),              // Speed mute disabled by default
          speedMuteThresholdMph(25),            // 25 mph default (city driving)
          speedMuteHysteresisMph(3),            // 3 mph hysteresis band
          speedMuteVolume(0),                   // Silent by default
          speedMuteVoice(true),                 // Suppress voice when speed-muted
          stealthEnabled(false),                // Stealth mode disabled by default
          autoPushEnabled(kDefaultAutoPushEnabled), autoPushProfileSchemaVersion(0), activeSlot(0), slot0Name("DEFAULT"), slot1Name("HIGHWAY"),
          slot2Name("COMFORT"), slot0Color(0x400A), slot1Color(0x07E0), slot2Color(0x8410), slot0Volume(0xFF),
          slot1Volume(0xFF), slot2Volume(0xFF), slot0MuteVolume(0xFF), slot1MuteVolume(0xFF), slot2MuteVolume(0xFF),
          slot0DarkMode(false), slot1DarkMode(false), slot2DarkMode(false),
          slot0VolumeOverride(false), slot1VolumeOverride(false), slot2VolumeOverride(false),
          slot0DarkModeOverride(false), slot1DarkModeOverride(false), slot2DarkModeOverride(false),
          slot0MuteToZero(false), slot1MuteToZero(false), slot2MuteToZero(false),
          slot0AlertPersist(0), slot1AlertPersist(0),
          slot2AlertPersist(0), slot0PriorityArrow(false), slot1PriorityArrow(false), slot2PriorityArrow(false),
          slot0_default(), slot1_highway(), slot2_comfort(), lastV1Address(""),
          autoPowerOffMinutes(0), // Default: disabled
          apTimeoutMinutes(0),    // Default: always on (0=unlimited)
          obdEnabled(false),      // OBD disabled by default
          obdSavedAddress(""),    // No saved device
          obdSavedName(""),       // No friendly name
          obdSavedAddrType(0),    // Default PUBLIC address type
          obdMinRssi(-90),        // Default -90 dBm minimum RSSI
          obdScanWindowMs(kConnectionCycleObdScanWindowMsDefault),
          obdRetryIntervalMs(kConnectionCycleObdRetryIntervalMsDefault),
          proxyOpenWindowMs(kConnectionCycleProxyOpenWindowMsDefault),
          v1SettleQuietMs(kConnectionCycleV1SettleQuietMsDefault),
          v1SettleFallbackMs(kConnectionCycleV1SettleFallbackMsDefault),
          cycleTeardownAckTimeoutMs(kConnectionCycleTeardownAckTimeoutMsDefault),
          alpEnabled(false),             // ALP disabled by default
          alpAlertPersistSec(0),         // ALP display persist off by default
          alpDisableV1LaserOnPush(true), // When ALP is enabled, let ALP own laser alerting
          gpsEnabled(false), // GPS disabled by default until module is installed
          gpsBaud(9600) {}   // Default UART baud for MTK3339

    int primaryWifiStaSlotIndex() const {
        int best = -1;
        for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
            const WifiStaSlot& slot = wifiStaSlots[i];
            if (!slot.isConfigured()) {
                continue;
            }
            if (best < 0) {
                best = static_cast<int>(i);
                continue;
            }

            const WifiStaSlot& current = wifiStaSlots[best];
            if (slot.priority < current.priority ||
                (slot.priority == current.priority && slot.lastConnectedAtSec > current.lastConnectedAtSec)) {
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    const WifiStaSlot* primaryWifiStaSlot() const {
        const int index = primaryWifiStaSlotIndex();
        return index >= 0 ? &wifiStaSlots[index] : nullptr;
    }

    WifiStaSlot* primaryWifiStaSlot() {
        const int index = primaryWifiStaSlotIndex();
        return index >= 0 ? &wifiStaSlots[index] : nullptr;
    }

    bool hasConfiguredWifiStaSlot() const { return primaryWifiStaSlotIndex() >= 0; }

    bool advanceWifiStaSlotRecency(size_t connectedIndex) {
        if (connectedIndex >= kWifiStaSlotCount || !wifiStaSlots[connectedIndex].isConfigured()) {
            return false;
        }

        uint32_t highestOrder = 0;
        for (const WifiStaSlot& slot : wifiStaSlots) {
            if (slot.isConfigured()) highestOrder = std::max(highestOrder, slot.lastConnectedAtSec);
        }

        if (highestOrder == std::numeric_limits<uint32_t>::max()) {
            // A logical sequence can eventually exhaust uint32_t. Rebase the
            // at-most-four configured slots to compact ranks while preserving
            // the exact comparison contract: larger token first, then lower
            // slot index. This is deterministic across reboot and never uses
            // uptime as persisted time.
            size_t oldestFirst[kWifiStaSlotCount] = {};
            size_t count = 0;
            for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
                if (!wifiStaSlots[index].isConfigured()) continue;
                size_t insertAt = count;
                while (insertAt > 0) {
                    const size_t previous = oldestFirst[insertAt - 1u];
                    const WifiStaSlot& candidate = wifiStaSlots[index];
                    const WifiStaSlot& prior = wifiStaSlots[previous];
                    const bool candidateIsOlder =
                        candidate.lastConnectedAtSec < prior.lastConnectedAtSec ||
                        (candidate.lastConnectedAtSec == prior.lastConnectedAtSec && index > previous);
                    if (!candidateIsOlder) break;
                    oldestFirst[insertAt] = previous;
                    --insertAt;
                }
                oldestFirst[insertAt] = index;
                ++count;
            }
            for (size_t order = 0; order < count; ++order) {
                wifiStaSlots[oldestFirst[order]].lastConnectedAtSec = static_cast<uint32_t>(order + 1u);
            }
            highestOrder = static_cast<uint32_t>(count);
        }

        wifiStaSlots[connectedIndex].lastConnectedAtSec = highestOrder + 1u;
        return true;
    }

    void refreshWifiClientAliasFromSlots() {
        if (const WifiStaSlot* slot = primaryWifiStaSlot()) {
            wifiClientSSID = slot->ssid;
        } else {
            wifiClientSSID = "";
        }
    }

    void ensureWifiStaSlotForLegacyAlias() {
        if (!hasConfiguredWifiStaSlot() && wifiClientSSID.length() > 0) {
            wifiStaSlots[0].ssid = wifiClientSSID;
            if (wifiStaSlots[0].label.length() == 0) {
                wifiStaSlots[0].label = "Saved";
            }
            wifiStaSlots[0].priority = 0;
        }
        refreshWifiClientAliasFromSlots();
    }

    static uint8_t normalizeAutoPushSlotIndex(int slotNum) { return slotNum == 1 ? 1 : (slotNum == 2 ? 2 : 0); }

    AutoPushSlotView autoPushSlotView(int slotNum) {
        switch (normalizeAutoPushSlotIndex(slotNum)) {
        case 1:
            return AutoPushSlotView{
                slot1Name,       slot1Color,        slot1Volume,        slot1MuteVolume, slot1DarkMode,
                slot1VolumeOverride, slot1DarkModeOverride,
                slot1MuteToZero, slot1AlertPersist, slot1PriorityArrow, slot1_highway,
            };
        case 2:
            return AutoPushSlotView{
                slot2Name,       slot2Color,        slot2Volume,        slot2MuteVolume, slot2DarkMode,
                slot2VolumeOverride, slot2DarkModeOverride,
                slot2MuteToZero, slot2AlertPersist, slot2PriorityArrow, slot2_comfort,
            };
        default:
            return AutoPushSlotView{
                slot0Name,       slot0Color,        slot0Volume,        slot0MuteVolume, slot0DarkMode,
                slot0VolumeOverride, slot0DarkModeOverride,
                slot0MuteToZero, slot0AlertPersist, slot0PriorityArrow, slot0_default,
            };
        }
    }

    ConstAutoPushSlotView autoPushSlotView(int slotNum) const {
        switch (normalizeAutoPushSlotIndex(slotNum)) {
        case 1:
            return ConstAutoPushSlotView{
                slot1Name,       slot1Color,        slot1Volume,        slot1MuteVolume, slot1DarkMode,
                slot1VolumeOverride, slot1DarkModeOverride,
                slot1MuteToZero, slot1AlertPersist, slot1PriorityArrow, slot1_highway,
            };
        case 2:
            return ConstAutoPushSlotView{
                slot2Name,       slot2Color,        slot2Volume,        slot2MuteVolume, slot2DarkMode,
                slot2VolumeOverride, slot2DarkModeOverride,
                slot2MuteToZero, slot2AlertPersist, slot2PriorityArrow, slot2_comfort,
            };
        default:
            return ConstAutoPushSlotView{
                slot0Name,       slot0Color,        slot0Volume,        slot0MuteVolume, slot0DarkMode,
                slot0VolumeOverride, slot0DarkModeOverride,
                slot0MuteToZero, slot0AlertPersist, slot0PriorityArrow, slot0_default,
            };
        }
    }
};

struct SettingsBackupApplyResult {
    bool success = false;
    int profilesRestored = 0;
    bool migrationPending = false;
};

enum class SettingsBackupScope : uint8_t {
    Full,
    ProfilesOnly,
};

/// Optional task-watchdog feed for applyBackupDocument().
///
/// A full restore rewrites the WiFi credential NVS namespace, re-saves every
/// profile in the backup (one filesystem write each) and then performs the A/B
/// settings NVS rewrite.  On a slow SD card that run can exceed the task
/// watchdog window and panic mid-restore.  esp_task_wdt_reset() is ESP-IDF only,
/// so the feed is injected as a plain function pointer: settings_backup_doc.cpp
/// stays host-compilable and native tests can count the feeds and pin where they
/// happen.  Default-constructed (feed == nullptr) means "never feed", which is
/// what the boot-time SD restore path uses.
struct SettingsRestoreWatchdog {
    void (*feed)(void* ctx) = nullptr;
    void* ctx = nullptr;
};

enum class SettingsPersistMode : uint8_t {
    Immediate,
    ImmediateNvsDeferredBackup,
    Deferred,
};

struct DeviceSettingsUpdate {
    bool hasApCredentials = false;
    String apSSID;
    String apPassword;

    bool hasProxyBLE = false;
    bool proxyBLE = false;

    bool hasProxyName = false;
    String proxyName;

    bool hasAutoPowerOffMinutes = false;
    uint8_t autoPowerOffMinutes = 0;

    bool hasApTimeoutMinutes = false;
    uint8_t apTimeoutMinutes = 0;

    bool hasAlpEnabled = false;
    bool alpEnabled = false;

    bool hasAlpAlertPersistSec = false;
    uint8_t alpAlertPersistSec = 0;

    bool hasAlpDisableV1LaserOnPush = false;
    bool alpDisableV1LaserOnPush = true;

    bool hasGpsEnabled = false;
    bool gpsEnabled = false;

    bool hasGpsBaud = false;
    uint32_t gpsBaud = 9600;

};

struct AudioSettingsUpdate {
    bool hasVoiceAlertMode = false;
    VoiceAlertMode voiceAlertMode = VOICE_MODE_DISABLED;

    bool hasVoiceDirectionEnabled = false;
    bool voiceDirectionEnabled = false;

    bool hasAnnounceBogeyCount = false;
    bool announceBogeyCount = false;

    bool hasMuteVoiceIfVolZero = false;
    bool muteVoiceIfVolZero = false;

    bool hasVoiceVolume = false;
    uint8_t voiceVolume = 0;

    bool hasAnnounceSecondaryAlerts = false;
    bool announceSecondaryAlerts = false;

    bool hasSecondaryLaser = false;
    bool secondaryLaser = false;

    bool hasSecondaryKa = false;
    bool secondaryKa = false;

    bool hasSecondaryK = false;
    bool secondaryK = false;

    bool hasSecondaryX = false;
    bool secondaryX = false;

    bool hasAlertVolumeFadeEnabled = false;
    bool alertVolumeFadeEnabled = false;

    bool hasAlertVolumeFadeDelaySec = false;
    uint8_t alertVolumeFadeDelaySec = 0;

    bool hasAlertVolumeFadeVolume = false;
    uint8_t alertVolumeFadeVolume = 0;

    bool hasSpeedMuteEnabled = false;
    bool speedMuteEnabled = false;

    bool hasSpeedMuteThresholdMph = false;
    uint8_t speedMuteThresholdMph = 0;

    bool hasSpeedMuteHysteresisMph = false;
    uint8_t speedMuteHysteresisMph = 0;

    bool hasSpeedMuteVolume = false;
    uint8_t speedMuteVolume = 0;

    bool hasSpeedMuteVoice = false;
    bool speedMuteVoice = true;

    bool hasStealthEnabled = false;
    bool stealthEnabled = false;
};

struct DisplaySettingsUpdate {
    bool hasColorBogey = false;
    uint16_t colorBogey = 0;
    bool hasColorFrequency = false;
    uint16_t colorFrequency = 0;
    bool hasColorArrowFront = false;
    uint16_t colorArrowFront = 0;
    bool hasColorArrowSide = false;
    uint16_t colorArrowSide = 0;
    bool hasColorArrowRear = false;
    uint16_t colorArrowRear = 0;
    bool hasColorBandL = false;
    uint16_t colorBandL = 0;
    bool hasColorBandKa = false;
    uint16_t colorBandKa = 0;
    bool hasColorBandK = false;
    uint16_t colorBandK = 0;
    bool hasColorBandX = false;
    uint16_t colorBandX = 0;
    bool hasColorBandPhoto = false;
    uint16_t colorBandPhoto = 0;
    bool hasColorWiFiConnected = false;
    uint16_t colorWiFiConnected = 0;
    bool hasColorBleConnected = false;
    uint16_t colorBleConnected = 0;
    bool hasColorBleDisconnected = false;
    uint16_t colorBleDisconnected = 0;
    bool hasColorBar[SIGNAL_BAR_COLOR_COUNT] = {};
    uint16_t colorBars[SIGNAL_BAR_COLOR_COUNT] = {};
    bool hasColorMuted = false;
    uint16_t colorMuted = 0;
    bool hasColorPersisted = false;
    uint16_t colorPersisted = 0;
    bool hasColorVolumeMain = false;
    uint16_t colorVolumeMain = 0;
    bool hasColorVolumeMute = false;
    uint16_t colorVolumeMute = 0;
    bool hasColorRssiV1 = false;
    uint16_t colorRssiV1 = 0;
    bool hasColorRssiProxy = false;
    uint16_t colorRssiProxy = 0;
    bool hasColorObd = false;
    uint16_t colorObd = 0;
    bool hasColorAlpConnected = false;
    uint16_t colorAlpConnected = 0;
    bool hasColorAlpDli = false;
    uint16_t colorAlpDli = 0;
    bool hasColorAlpLidActive = false;
    uint16_t colorAlpLidActive = 0;
    bool hasColorAlpAlert = false;
    uint16_t colorAlpAlert = 0;
    bool hasFreqUseBandColor = false;
    bool freqUseBandColor = false;
    bool hasHideWifiIcon = false;
    bool hideWifiIcon = false;
    bool hasHideProfileIndicator = false;
    bool hideProfileIndicator = false;
    bool hasHideBatteryIcon = false;
    bool hideBatteryIcon = false;
    bool hasShowBatteryPercent = false;
    bool showBatteryPercent = false;
    bool hasHideBleIcon = false;
    bool hideBleIcon = false;
    bool hasHideVolumeIndicator = false;
    bool hideVolumeIndicator = false;
    bool hasHideRssiIndicator = false;
    bool hideRssiIndicator = false;
    bool hasBrightness = false;
    uint8_t brightness = 0;
};

struct ObdSettingsUpdate {
    bool hasEnabled = false;
    bool enabled = false;

    bool hasMinRssi = false;
    int8_t minRssi = -80;

    bool hasObdScanWindowMs = false;
    uint32_t obdScanWindowMs = 0;

    bool hasObdRetryIntervalMs = false;
    uint32_t obdRetryIntervalMs = 0;

    bool hasProxyOpenWindowMs = false;
    uint32_t proxyOpenWindowMs = 0;

    bool hasV1SettleQuietMs = false;
    uint32_t v1SettleQuietMs = 0;

    bool hasV1SettleFallbackMs = false;
    uint32_t v1SettleFallbackMs = 0;

    bool hasCycleTeardownAckTimeoutMs = false;
    uint32_t cycleTeardownAckTimeoutMs = 0;

    bool hasSavedAddress = false;
    String savedAddress;

    bool hasSavedName = false;
    String savedName;

    bool hasSavedAddrType = false;
    uint8_t savedAddrType = 0;

    bool resetSavedNameOnAddressChange = false;
};

struct AutoPushSlotUpdate {
    int slot = 0;

    bool hasName = false;
    String name;

    bool hasColor = false;
    uint16_t color = 0;

    bool hasVolume = false;
    uint8_t volume = 0xFF;

    bool hasMuteVolume = false;
    uint8_t muteVolume = 0xFF;

    bool hasDarkMode = false;
    bool darkMode = false;

    bool hasVolumeOverride = false;
    bool volumeOverride = false;

    bool hasDarkModeOverride = false;
    bool darkModeOverride = false;

    bool hasMuteToZero = false;
    bool muteToZero = false;

    bool hasAlertPersist = false;
    uint8_t alertPersist = 0;

    bool hasPriorityArrowOnly = false;
    bool priorityArrowOnly = false;

    bool hasProfileName = false;
    String profileName;

    bool hasMode = false;
    V1Mode mode = V1_MODE_UNKNOWN;
};

struct AutoPushStateUpdate {
    bool hasActiveSlot = false;
    int activeSlot = 0;

    bool hasEnabled = false;
    bool enabled = false;
};

#endif // SETTINGS_TYPES_H
