#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include "mock_heap_caps_state.h"

#ifndef MALLOC_CAP_SPIRAM
#define MALLOC_CAP_SPIRAM 0x01
#endif

#ifndef MALLOC_CAP_INTERNAL
#define MALLOC_CAP_INTERNAL 0x02
#endif

#ifndef MALLOC_CAP_8BIT
#define MALLOC_CAP_8BIT 0x04
#endif

#ifndef MALLOC_CAP_DEFAULT
#define MALLOC_CAP_DEFAULT 0x00
#endif

inline void* heap_caps_malloc(size_t size, uint32_t caps) {
    g_mock_heap_caps_malloc_calls++;
    g_mock_heap_caps_last_malloc_size = size;
    g_mock_heap_caps_last_malloc_caps = caps;
    if (g_mock_heap_caps_malloc_calls <= 32u) {
        const uint32_t bit = 1u << (g_mock_heap_caps_malloc_calls - 1u);
        if ((g_mock_heap_caps_fail_call_mask & bit) != 0u) {
            return nullptr;
        }
    }
    if (g_mock_heap_caps_fail_on_call != 0u &&
        g_mock_heap_caps_malloc_calls == g_mock_heap_caps_fail_on_call) {
        g_mock_heap_caps_fail_on_call = 0u;
        return nullptr;
    }
    if (g_mock_heap_caps_fail_malloc) {
        g_mock_heap_caps_fail_malloc = false;
        return nullptr;
    }
    void* ptr = std::malloc(size);
    if (ptr != nullptr) {
        g_mock_heap_caps_outstanding_allocations++;
    }
    return ptr;
}

inline void* heap_caps_realloc(void* ptr, size_t size, uint32_t caps) {
    g_mock_heap_caps_realloc_calls++;
    g_mock_heap_caps_last_realloc_size = size;
    g_mock_heap_caps_last_realloc_caps = caps;
    if (g_mock_heap_caps_realloc_calls <= 32u) {
        const uint32_t bit = 1u << (g_mock_heap_caps_realloc_calls - 1u);
        if ((g_mock_heap_caps_fail_call_mask & bit) != 0u) {
            return nullptr;
        }
    }
    if (g_mock_heap_caps_fail_on_call != 0u &&
        g_mock_heap_caps_realloc_calls == g_mock_heap_caps_fail_on_call) {
        g_mock_heap_caps_fail_on_call = 0u;
        return nullptr;
    }
    if (g_mock_heap_caps_fail_realloc) {
        g_mock_heap_caps_fail_realloc = false;
        return nullptr;
    }

    void* resized = std::realloc(ptr, size);
    if (ptr == nullptr && resized != nullptr) {
        g_mock_heap_caps_outstanding_allocations++;
    }
    return resized;
}

inline void heap_caps_free(void* ptr) {
    g_mock_heap_caps_free_calls++;
    if (ptr != nullptr && g_mock_heap_caps_outstanding_allocations > 0u) {
        g_mock_heap_caps_outstanding_allocations--;
    }
    std::free(ptr);
}

inline uint32_t heap_caps_get_largest_free_block(uint32_t caps) {
    (void)caps;
    return g_mock_heap_caps_largest_block;
}

inline uint32_t heap_caps_get_free_size(uint32_t caps) {
    (void)caps;
    return g_mock_heap_caps_free_size;
}

inline uint32_t heap_caps_get_minimum_free_size(uint32_t caps) {
    (void)caps;
    return g_mock_heap_caps_free_size;
}
