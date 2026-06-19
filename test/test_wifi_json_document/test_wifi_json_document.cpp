#include <unity.h>

#include "../mocks/mock_heap_caps_state.h"
#include "../mocks/esp_heap_caps.h"
#include "../../src/modules/wifi/wifi_json_document.h"

void setUp() {
    mock_reset_heap_caps();
}

void tearDown() {}

void test_allocator_prefers_psram_allocations() {
    void* ptr = WifiJson::allocator().allocate(64);

    TEST_ASSERT_NOT_NULL(ptr);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(WifiJson::kPsramCaps, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_heap_caps_outstanding_allocations);

    WifiJson::allocator().deallocate(ptr);

    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_heap_caps_free_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_heap_caps_outstanding_allocations);
}

void test_allocator_falls_back_to_internal_when_psram_alloc_fails() {
    g_mock_heap_caps_fail_on_call = 1u;

    void* ptr = WifiJson::allocator().allocate(64);

    TEST_ASSERT_NOT_NULL(ptr);
    TEST_ASSERT_EQUAL_UINT32(2u, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(WifiJson::kInternalCaps, g_mock_heap_caps_last_malloc_caps);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_heap_caps_outstanding_allocations);

    WifiJson::allocator().deallocate(ptr);

    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_heap_caps_free_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_heap_caps_outstanding_allocations);
}

void test_allocator_reallocates_with_psram_caps() {
    void* ptr = WifiJson::allocator().allocate(32);

    TEST_ASSERT_NOT_NULL(ptr);
    g_mock_heap_caps_last_realloc_size = 0u;
    g_mock_heap_caps_last_realloc_caps = 0u;
    g_mock_heap_caps_realloc_calls = 0u;

    void* resized = WifiJson::allocator().reallocate(ptr, 128);

    TEST_ASSERT_NOT_NULL(resized);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_heap_caps_realloc_calls);
    TEST_ASSERT_EQUAL_UINT32(WifiJson::kPsramCaps, g_mock_heap_caps_last_realloc_caps);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_heap_caps_outstanding_allocations);

    WifiJson::allocator().deallocate(resized);

    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_heap_caps_free_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_heap_caps_outstanding_allocations);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_allocator_prefers_psram_allocations);
    RUN_TEST(test_allocator_falls_back_to_internal_when_psram_alloc_fails);
    RUN_TEST(test_allocator_reallocates_with_psram_caps);
    return UNITY_END();
}
