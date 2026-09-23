#pragma once

#include <stdint.h>
#include <math.h> // NAN
#include <array>

// Shared pure-data protocol types used by firmware and native tests.

enum Band {
    BAND_NONE = 0,
    BAND_LASER = 1 << 0,
    BAND_KA = 1 << 1,
    BAND_K = 1 << 2,
    BAND_X = 1 << 3,
    BAND_KU = 1 << 4 // Per Valentine AlertBand.java: Ku raw band value is 0x10.
                     // Our bitmask happens to match the spec byte exactly.
};

enum Direction { DIR_NONE = 0, DIR_FRONT = 1, DIR_SIDE = 2, DIR_REAR = 4 };

struct AlertData {
    static constexpr uint8_t UNKNOWN_V1_INDEX = UINT8_MAX;

    Band band;
    Direction direction;
    uint8_t v1Index; // V1-provided alert-table assignment, or UNKNOWN_V1_INDEX for synthetic alerts
    uint8_t frontRawStrength; // V1 front-antenna RSSI byte
    uint8_t rearRawStrength;  // V1 rear-antenna RSSI byte
    uint8_t frontStrength;    // 0-8 VR bargraph bars, unmodified
    uint8_t rearStrength;     // 0-8 VR bargraph bars, unmodified
    uint32_t frequency;       // MHz
    bool isValid;
    bool isPriority;     // aux0 bit 7 — V1's priority flag
    bool isJunk;         // aux0 bit 6 — junked alert
    uint8_t photoType;   // aux0 bits 0..3 — photo type (V4.1037+)
    uint8_t rawBandBits; // bandArrow low 5 bits (VR-style raw band field)
    bool isKu;           // True when rawBandBits resolves to Ku (0x10)

    AlertData()
        : band(BAND_NONE), direction(DIR_NONE), v1Index(UNKNOWN_V1_INDEX), frontRawStrength(0), rearRawStrength(0),
          frontStrength(0), rearStrength(0), frequency(0), isValid(false), isPriority(false), isJunk(false),
          photoType(0), rawBandBits(0), isKu(false) {}

    static AlertData create(Band b, Direction d, uint8_t front, uint8_t rear, uint32_t freq, bool valid = true,
                            bool priority = false) {
        AlertData a;
        a.band = b;
        a.direction = d;
        a.frontStrength = front;
        a.rearStrength = rear;
        a.frequency = freq;
        a.isValid = valid;
        a.isPriority = priority;
        return a;
    }
};

struct DisplayState {
    uint8_t activeBands;     // Bitmap of active bands
    Direction arrows;        // Bitmap of arrow directions (all active, from display packet)
    Direction priorityArrow; // Arrow from V1's priority alert (alerts[0])
    uint8_t signalBars;      // 0-8 LEDs lit, per ESP Spec 3.015 Table 9.1; renderer clamps to the physical meter
    bool muted;
    bool systemTest;
    char modeChar;
    bool hasMode;
    bool displayOn;             // True if main display is ON (not dark)
    bool hasDisplayOn;          // True only after display state is decoded from infDisplayData
    uint8_t flashBits;          // Blink state for arrows (from display packet)
    uint8_t bandFlashBits;      // Blink state for bands (L=0x01, Ka=0x02, K=0x04, X=0x08)
    uint8_t mainVolume;         // Main volume 0-9
    uint8_t muteVolume;         // Muted volume 0-9
    uint32_t v1FirmwareVersion; // V1 firmware version as integer (e.g. 41028 for 4.1028)
    bool hasV1Version;          // True if we've received version from V1
    bool hasVolumeData;         // True after canonical display/current/all-volume evidence
    uint8_t v1PriorityIndex;    // Resolved priority alert index for current table (0-based)
    // The V1 has one bogey LED: image1 is its displayed value and image2 is
    // the blink-off mask, not a second digit (ESP Spec 3.003 p.25).
    uint8_t bogeyCounterByte;  // image1 — steady-displayed 7-segment byte
    char bogeyCounterChar;     // Decoded character from image1
    bool bogeyCounterDot;      // Decimal point from image1 (bit 7)
    uint8_t bogeyCounterByte2; // image2 — blink-off mask companion to image1
    char bogeyCounterChar2;    // Decoded character from image2 (blink-off pair)
    bool bogeyCounterDot2;     // Decimal point from image2 (bit 7)
    bool hasJunkAlert;         // True if any alert row has aux0 junk bit set
    bool hasPhotoAlert;        // True if any alert row has photo type > 0
    // Table-wide Ku presence. The V1's band-display row has no dedicated Ku
    // LED, but presentation identity remains priority-owned: this fact must
    // not by itself relabel the shared physical K cell.
    bool hasKuAlert;
    // Per Valentine InfDisplayData.isSoft(),
    // the spec-correct audio-mute flag is auxData0 bit 0 (0x01). The existing
    // `muted` field above tracks the on-screen mute LED (image1 bit 4) and
    // is debounced for icon stability; `softMuted` is the undebounced
    // spec-true mute state and should be preferred by V1 quiet-control paths.
    bool softMuted;
    // Transport and display metadata from InfDisplayData. The `has*` flags
    // keep firmware-qualified bits from being presented as known on older or
    // not-yet-versioned detectors.
    bool timeSliceHoldoff;
    bool displayActive;
    bool hasDisplayActive;
    bool logicMuted;
    bool hasLogicMuted;
    bool autoMuted;
    bool hasAutoMuted;
    bool doubleTapActive;
    bool hasDoubleTapActive;
    // Per Valentine
    // InfDisplayData.isSystemStatus() (auxData0 bit 2). True when the V1 is
    // actively searching for alerts. When false, band/arrow data in the
    // display packet is not meaningful — the parser clears `activeBands` and
    // `arrows` to avoid reporting stale indicators.
    bool systemStatus;
    // RESPALLVOLUME (0x3D) carries the
    // V1's authoritative volume state.  When a 0x3D packet is observed,
    // mainVolume/muteVolume are overwritten from the spec-true source and
    // the saved-volume pair is exposed for consumers that care about restore
    // points. Canonical infDisplayData aux2 remains the current-volume fallback
    // before the first 0x3D response.
    uint8_t savedMainVolume;
    uint8_t savedMuteVolume;
    bool hasSavedVolume;

    DisplayState()
        : activeBands(BAND_NONE), arrows(DIR_NONE), priorityArrow(DIR_NONE), signalBars(0), muted(false),
          systemTest(false), modeChar(0), hasMode(false), displayOn(true), hasDisplayOn(false), flashBits(0),
          bandFlashBits(0), mainVolume(0), muteVolume(0), v1FirmwareVersion(0), hasV1Version(false),
          hasVolumeData(false), v1PriorityIndex(0), bogeyCounterByte(0), bogeyCounterChar('0'), bogeyCounterDot(false),
          bogeyCounterByte2(0), bogeyCounterChar2(' '), bogeyCounterDot2(false), hasJunkAlert(false),
          hasPhotoAlert(false), hasKuAlert(false), softMuted(false), timeSliceHoldoff(true), displayActive(false),
          hasDisplayActive(false), logicMuted(false), hasLogicMuted(false), autoMuted(false), hasAutoMuted(false),
          doubleTapActive(false), hasDoubleTapActive(false), systemStatus(true), savedMainVolume(0),
          savedMuteVolume(0), hasSavedVolume(false) {}

    bool supportsVolume() const { return hasVolumeData || (hasV1Version && v1FirmwareVersion >= 41028); }
};

// Settings transactions consume these canonical wire observations. They live
// beside, rather than inside, DisplayState so Apply keeps each value inseparable
// from the canonical packet revision that proved it.
struct V1DisplayOnObservation {
    uint32_t revision = 0;
    uint32_t sequence = 0;
    // BLE notification admission order for the first byte of this frame.
    // Apply uses this separately from parse-time revision so a response that
    // was already queued before a command cannot prove that command.
    uint32_t ingressSequence = 0;
    bool available = false;
    bool value = true;
};

struct V1ModeObservation {
    uint32_t revision = 0;
    uint32_t sequence = 0;
    uint32_t ingressSequence = 0;
    bool available = false;
    char value = 0;
};

struct V1CurrentVolumeObservation {
    uint32_t revision = 0;
    uint32_t sequence = 0;
    uint32_t ingressSequence = 0;
    bool available = false;
    uint8_t main = 0;
    uint8_t muted = 0;
};

struct V1AllVolumeObservation {
    uint32_t revision = 0;
    uint32_t sequence = 0;
    uint32_t ingressSequence = 0;
    bool available = false;
    uint8_t currentMain = 0;
    uint8_t currentMuted = 0;
    uint8_t savedMain = 0;
    uint8_t savedMuted = 0;
};

struct V1DisplayVolumeObservation {
    uint32_t revision = 0;
    uint32_t sequence = 0;
    uint32_t ingressSequence = 0;
    bool available = false;
    uint8_t main = 0;
    uint8_t muted = 0;
};

enum class V1BluetoothIndicatorState : uint8_t {
    Off = 0,
    Blinking = 1,
    On = 2,
    Invalid = 3,
};

struct V1BluetoothIndicatorObservation {
    uint32_t revision = 0;
    uint32_t sequence = 0;
    uint32_t ingressSequence = 0;
    bool available = false;
    bool image1 = false;
    bool image2 = false;
    V1BluetoothIndicatorState state = V1BluetoothIndicatorState::Invalid;
};

struct V1SweepSectionObservation {
    uint8_t index = 0;
    uint8_t count = 0;
    uint16_t lowerMHz = 0;
    uint16_t upperMHz = 0;
};

struct V1SweepSectionsObservation {
    uint32_t revision = 0;
    uint32_t sequence = 0;
    uint32_t ingressSequence = 0;
    bool available = false;
    bool complete = false;
    bool poisoned = false;
    uint8_t count = 0;
    uint16_t presentMask = 0;
    std::array<V1SweepSectionObservation, 15> sections{};
};

struct V1SweepMaxObservation {
    uint32_t revision = 0;
    uint32_t sequence = 0;
    uint32_t ingressSequence = 0;
    bool available = false;
    bool poisoned = false;
    uint8_t maxIndex = 0;
};

struct V1SweepDefinitionObservation {
    uint8_t index = 0;
    uint16_t lowerMHz = 0;
    uint16_t upperMHz = 0;
};

struct V1SweepDefinitionsObservation {
    uint32_t revision = 0;
    uint32_t sequence = 0;
    uint32_t ingressSequence = 0;
    uint64_t presentMask = 0;
    bool poisoned = false;
    std::array<V1SweepDefinitionObservation, 64> definitions{};
    // Each response packet carries one definition. Preserve its individual
    // transport ingress so a fresh aggregate cannot launder a queued packet
    // that entered before a read-request boundary.
    std::array<uint32_t, 64> ingressSequences{};
};

struct V1SweepWriteResultObservation {
    uint32_t revision = 0;
    uint32_t sequence = 0;
    uint32_t ingressSequence = 0;
    bool available = false;
    uint8_t result = 0;
};
