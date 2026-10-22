#pragma once

#include <cstddef>
#include <cstdint>

namespace EmbeddedWebAssets {

struct Asset {
    const char* path;
    const uint8_t* data;
    size_t size;
};

// The current firmware owns these URLs even when a requested old asset is absent.
bool isManagedPath(const char* path);
const Asset* find(const char* path);
const char* fingerprint();

} // namespace EmbeddedWebAssets
