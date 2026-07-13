#pragma once

// GENERATED FILE — do not edit by hand.
// Source of truth: docs/V1_PROTOCOL_REFERENCES.md (machine-readable spec tables).
// Regenerate: python3 scripts/check_protocol_spec_tables.py --write
// CI gate:    python3 scripts/check_protocol_spec_tables.py

#include <stdint.h>

namespace protocol_spec {

enum class SpecBand : uint8_t { LASER, KA, K, X, KU };
enum class SpecDirection : uint8_t { FRONT, SIDE, REAR };

struct BitmapBarsCase { uint8_t bitmap; uint8_t bars; };
inline constexpr BitmapBarsCase kLedBitmapBars[] = {
    {0x00, 0},
    {0x01, 1},
    {0x03, 3},
    {0x07, 4},
    {0x0F, 5},
    {0x1F, 7},
    {0x3F, 8},
    {0x7F, 8},
    {0xFF, 8},
};
inline constexpr uint8_t kLedBitmapOverflowBars = 8;

struct BandCase { uint8_t byte; SpecBand band; };
inline constexpr BandCase kAlertBandValues[] = {
    {0x01, SpecBand::LASER},
    {0x02, SpecBand::KA},
    {0x04, SpecBand::K},
    {0x08, SpecBand::X},
    {0x10, SpecBand::KU},
};

struct DirectionCase { uint8_t bit; SpecDirection direction; };
inline constexpr DirectionCase kAlertDirectionBits[] = {
    {0x20, SpecDirection::FRONT},
    {0x40, SpecDirection::SIDE},
    {0x80, SpecDirection::REAR},
};

// Minimum raw RSSI for each VR bargraph bar count (8..1), per band.
struct StrengthThreshold { uint8_t minRaw; uint8_t vrBars; };
inline constexpr StrengthThreshold kStrengthThresholdsKa[] = {
    {0xBA, 8},
    {0xB3, 7},
    {0xAC, 6},
    {0xA5, 5},
    {0x9E, 4},
    {0x97, 3},
    {0x90, 2},
    {0x01, 1},
};
inline constexpr StrengthThreshold kStrengthThresholdsX[] = {
    {0xD0, 8},
    {0xC5, 7},
    {0xBD, 6},
    {0xB4, 5},
    {0xAA, 4},
    {0xA0, 3},
    {0x96, 2},
    {0x01, 1},
};
inline constexpr StrengthThreshold kStrengthThresholdsK[] = {
    {0xC2, 8},
    {0xB8, 7},
    {0xAE, 6},
    {0xA4, 5},
    {0x9A, 4},
    {0x90, 3},
    {0x88, 2},
    {0x01, 1},
};
inline constexpr uint8_t kLaserVrBars = 8;

// vrBars (index 0..8) -> six-bar secondary-card scale.
inline constexpr uint8_t kVrToCardBars[9] = {
    0, 1, 2, 2, 3, 4, 5, 5, 6
};

// Alert-row byte offsets (see docs: alert-row-layout).
inline constexpr uint8_t kAlertRowOffsetIndexCount = 0;
inline constexpr uint8_t kAlertRowOffsetFreqMsb = 1;
inline constexpr uint8_t kAlertRowOffsetFreqLsb = 2;
inline constexpr uint8_t kAlertRowOffsetFrontRssi = 3;
inline constexpr uint8_t kAlertRowOffsetRearRssi = 4;
inline constexpr uint8_t kAlertRowOffsetBandArrow = 5;
inline constexpr uint8_t kAlertRowOffsetAux0 = 6;

// Alert-row aux0 bit masks (see docs: alert-aux0-bits).
inline constexpr uint8_t kAlertAux0PriorityMask = 0x80;
inline constexpr uint8_t kAlertAux0JunkMask = 0x40;
inline constexpr uint8_t kAlertAux0PhotoTypeMask = 0x0F;

// V1 user-settings bytes bit map (see docs: user-bytes-bit-map).
// kind: 0=direct (bit set == ON), 1=inverted (bit clear == ON), 2=multi-bit field.
struct UserByteField { uint8_t byteIndex; uint8_t mask; uint8_t kind; const char* name; };
inline constexpr UserByteField kUserBytesBitMap[] = {
    {0, 0x01, 0, "X_BAND"},
    {0, 0x02, 0, "K_BAND"},
    {0, 0x04, 0, "KA_BAND"},
    {0, 0x08, 0, "LASER"},
    {0, 0x10, 1, "MUTE_TO_MUTE_VOLUME"},
    {0, 0x20, 0, "BOGEY_LOCK_LOUD"},
    {0, 0x40, 1, "MUTE_X_K_REAR"},
    {0, 0x80, 1, "KU_BAND"},
    {1, 0x01, 1, "EURO_MODE"},
    {1, 0x02, 0, "K_VERIFIER"},
    {1, 0x04, 0, "LASER_REAR"},
    {1, 0x08, 1, "CUSTOM_FREQS"},
    {1, 0x10, 1, "KA_ALWAYS_PRIORITY"},
    {1, 0x20, 0, "FAST_LASER_DETECT"},
    {1, 0xC0, 2, "KA_SENSITIVITY"},
    {2, 0x01, 0, "STARTUP_SEQUENCE"},
    {2, 0x02, 0, "RESTING_DISPLAY"},
    {2, 0x04, 1, "BSM_PLUS"},
    {2, 0x18, 2, "AUTO_MUTE"},
    {2, 0x60, 2, "K_SENSITIVITY"},
    {2, 0x80, 1, "MRCT"},
    {3, 0x03, 2, "X_SENSITIVITY"},
    {3, 0x04, 1, "DRIVE_SAFE_3D"},
    {3, 0x08, 1, "DRIVE_SAFE_3D_HD"},
    {3, 0x10, 1, "REDFLEX_HALO"},
    {3, 0x20, 1, "REDFLEX_NK7"},
    {3, 0x40, 1, "EKIN"},
    {3, 0x80, 1, "PHOTO_VERIFIER"},
};

}  // namespace protocol_spec
