// ============================================================================
// display_segments.h — 7-segment and 14-segment rendering data tables
//
// Pure data + utility functions for segment-display rendering.
// No display-driver dependency — just geometry and character maps.
// ============================================================================
#pragma once
#include <cstdint>
#include <cstddef>

namespace DisplaySegments {

// --- Geometry ---

struct SegMetrics {
    int segLen;
    int segThick;
    int digitW;
    int digitH;
    int spacing;
    int dot;
};

/// Compute segment metrics for a given scale factor.
inline SegMetrics segMetrics(float scale) {
    int segLen   = static_cast<int>(8 * scale + 0.5f);
    int segThick = static_cast<int>(3 * scale + 0.5f);
    if (segLen < 2)   segLen   = 2;
    if (segThick < 1) segThick = 1;
    return {
        segLen,
        segThick,
        segLen + 2 * segThick,
        2 * segLen + 3 * segThick,
        segThick,
        segThick
    };
}

// --- 7-segment lookup ---

/// Standard 7-segment patterns (a–g) for digits 0–9.
constexpr bool DIGIT_SEGMENTS[10][7] = {
    // a,     b,     c,     d,     e,     f,     g
    {true,  true,  true,  true,  true,  true,  false}, // 0
    {false, true,  true,  false, false, false, false}, // 1
    {true,  true,  false, true,  true,  false, true }, // 2
    {true,  true,  true,  true,  false, false, true }, // 3
    {false, true,  true,  false, false, true,  true }, // 4
    {true,  false, true,  true,  false, true,  true }, // 5
    {true,  false, true,  true,  true,  true,  true }, // 6
    {true,  true,  true,  false, false, false, false}, // 7
    {true,  true,  true,  true,  true,  true,  true }, // 8
    {true,  true,  true,  true,  false, true,  true }  // 9
};

// --- 14-segment lookup ---
// Segments: 0=top, 1=top-right, 2=bottom-right, 3=bottom, 4=bottom-left,
//           5=top-left, 6=middle-left, 7=middle-right, 8=diag-top-left,
//           9=diag-top-right, 10=center-top, 11=center-bottom,
//           12=diag-bottom-left, 13=diag-bottom-right

struct Char14Seg {
    char     ch;
    uint16_t segs;   // bit flags for segments 0-13
};

// Segment bit flags
constexpr uint16_t S14_TOP = (1 << 0);
constexpr uint16_t S14_TR  = (1 << 1);   // top-right vertical
constexpr uint16_t S14_BR  = (1 << 2);   // bottom-right vertical
constexpr uint16_t S14_BOT = (1 << 3);
constexpr uint16_t S14_BL  = (1 << 4);   // bottom-left vertical
constexpr uint16_t S14_TL  = (1 << 5);   // top-left vertical
constexpr uint16_t S14_ML  = (1 << 6);   // middle-left horizontal
constexpr uint16_t S14_MR  = (1 << 7);   // middle-right horizontal
constexpr uint16_t S14_DTL = (1 << 8);   // diagonal top-left
constexpr uint16_t S14_DTR = (1 << 9);   // diagonal top-right
constexpr uint16_t S14_CT  = (1 << 10);  // center-top vertical
constexpr uint16_t S14_CB  = (1 << 11);  // center-bottom vertical
constexpr uint16_t S14_DBL = (1 << 12);  // diagonal bottom-left
constexpr uint16_t S14_DBR = (1 << 13);  // diagonal bottom-right

constexpr Char14Seg CHAR14_MAP[] = {
    {'0', S14_TOP | S14_TR | S14_BR | S14_BOT | S14_BL | S14_TL},
    {'1', S14_TR | S14_BR},
    {'2', S14_TOP | S14_TR | S14_ML | S14_MR | S14_BL | S14_BOT},
    {'3', S14_TOP | S14_TR | S14_MR | S14_BR | S14_BOT},
    {'4', S14_TL | S14_ML | S14_MR | S14_TR | S14_BR},
    {'5', S14_TOP | S14_TL | S14_ML | S14_MR | S14_BR | S14_BOT},
    {'6', S14_TOP | S14_TL | S14_ML | S14_MR | S14_BR | S14_BOT | S14_BL},
    {'7', S14_TOP | S14_TR | S14_BR},
    {'8', S14_TOP | S14_TR | S14_BR | S14_BOT | S14_BL | S14_TL | S14_ML | S14_MR},
    {'9', S14_TOP | S14_TR | S14_BR | S14_BOT | S14_TL | S14_ML | S14_MR},
    {'A', S14_TOP | S14_TL | S14_TR | S14_ML | S14_MR | S14_BL | S14_BR},
    {'B', S14_TOP | S14_TL | S14_BL | S14_BOT | S14_ML | S14_MR | S14_TR | S14_BR},
    {'C', S14_TOP | S14_TL | S14_BL | S14_BOT},
    {'D', S14_TOP | S14_TR | S14_BR | S14_BOT | S14_CT | S14_CB},
    {'E', S14_TOP | S14_TL | S14_ML | S14_BL | S14_BOT},
    {'L', S14_TL | S14_BL | S14_BOT},
    {'M', S14_TL | S14_TR | S14_BL | S14_BR | S14_DTL | S14_DTR},
    {'N', S14_TL | S14_BL | S14_TR | S14_BR | S14_DTL | S14_DBR},
    {'P', S14_TOP | S14_TL | S14_TR | S14_ML | S14_MR | S14_BL},
    {'R', S14_TOP | S14_TL | S14_TR | S14_ML | S14_MR | S14_BL | S14_DBR},
    {'S', S14_TOP | S14_TL | S14_ML | S14_MR | S14_BR | S14_BOT},
    {'T', S14_TOP | S14_CT | S14_CB},
    {'U', S14_TL | S14_TR | S14_BL | S14_BR | S14_BOT},
    {'-', S14_ML | S14_MR},
    {'.', 0},  // dot handled separately
};
constexpr int CHAR14_MAP_SIZE = sizeof(CHAR14_MAP) / sizeof(CHAR14_MAP[0]);

/// Look up the 14-segment bit pattern for a character (case-insensitive).
inline uint16_t get14SegPattern(char c) {
    char upper = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
    for (int i = 0; i < CHAR14_MAP_SIZE; i++) {
        if (CHAR14_MAP[i].ch == upper) return CHAR14_MAP[i].segs;
    }
    return 0;
}

} // namespace DisplaySegments
