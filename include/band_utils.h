#pragma once

#include "packet_parser.h"

inline const char* bandName(Band band) {
    switch (band) {
        case BAND_LASER: return "Laser";
        case BAND_KA:    return "Ka";
        case BAND_K:     return "K";
        case BAND_X:     return "X";
        default:         return "None";
    }
}

