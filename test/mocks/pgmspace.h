#pragma once

#include <stdint.h>

#ifndef PROGMEM
#define PROGMEM
#endif

static inline uint16_t pgm_read_word(const void* addr) {
    return *static_cast<const uint16_t*>(addr);
}

static inline uint32_t pgm_read_dword(const void* addr) {
    return *static_cast<const uint32_t*>(addr);
}
