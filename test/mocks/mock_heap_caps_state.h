#pragma once

#include <cstddef>
#include <cstdint>

// Shared heap-capability mock state used by native tests.
// Defaults represent a healthy heap unless tests override them.
inline uint32_t g_mock_heap_caps_free_size = 320000u;
inline uint32_t g_mock_heap_caps_largest_block = 8u * 1024u * 1024u;
inline size_t g_mock_heap_caps_last_malloc_size = 0u;
inline uint32_t g_mock_heap_caps_last_malloc_caps = 0u;
inline uint32_t g_mock_heap_caps_malloc_calls = 0u;
inline size_t g_mock_heap_caps_last_realloc_size = 0u;
inline uint32_t g_mock_heap_caps_last_realloc_caps = 0u;
inline uint32_t g_mock_heap_caps_realloc_calls = 0u;
inline uint32_t g_mock_heap_caps_free_calls = 0u;
inline bool g_mock_heap_caps_fail_malloc = false;
inline bool g_mock_heap_caps_fail_realloc = false;
inline uint32_t g_mock_heap_caps_fail_on_call = 0u;
inline uint32_t g_mock_heap_caps_fail_call_mask = 0u;
inline uint32_t g_mock_heap_caps_outstanding_allocations = 0u;

inline void mock_set_heap_caps(uint32_t free_size, uint32_t largest_block) {
    g_mock_heap_caps_free_size = free_size;
    g_mock_heap_caps_largest_block = largest_block;
}

inline void mock_reset_heap_caps_tracking() {
    g_mock_heap_caps_last_malloc_size = 0u;
    g_mock_heap_caps_last_malloc_caps = 0u;
    g_mock_heap_caps_malloc_calls = 0u;
    g_mock_heap_caps_last_realloc_size = 0u;
    g_mock_heap_caps_last_realloc_caps = 0u;
    g_mock_heap_caps_realloc_calls = 0u;
    g_mock_heap_caps_free_calls = 0u;
    g_mock_heap_caps_fail_malloc = false;
    g_mock_heap_caps_fail_realloc = false;
    g_mock_heap_caps_fail_on_call = 0u;
    g_mock_heap_caps_fail_call_mask = 0u;
    g_mock_heap_caps_outstanding_allocations = 0u;
}

inline void mock_reset_heap_caps() {
    g_mock_heap_caps_free_size = 320000u;
    g_mock_heap_caps_largest_block = 8u * 1024u * 1024u;
    mock_reset_heap_caps_tracking();
}
