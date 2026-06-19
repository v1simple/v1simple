#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../mocks/mock_heap_caps_state.h"
#include "../../src/psram_freertos_alloc.cpp"

namespace {

void dummyTask(void*) {}

}  // namespace

void setUp() {
    mock_reset_heap_caps();
    mock_reset_queue_create_state();
    mock_reset_task_create_state();
}

void tearDown() {}

void test_create_queue_prefer_psram_uses_static_psram_queue() {
    PsramQueueAllocation allocation;
    bool usedPsram = false;

    QueueHandle_t queue = createQueuePreferPsram(4, sizeof(uint32_t), allocation, &usedPsram);

    TEST_ASSERT_NOT_NULL(queue);
    TEST_ASSERT_TRUE(usedPsram);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_queue_create_state.staticCalls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_queue_create_state.dynamicCalls);
    TEST_ASSERT_EQUAL_UINT32(4u * sizeof(uint32_t), g_mock_heap_caps_last_malloc_size);
    TEST_ASSERT_TRUE((g_mock_heap_caps_last_malloc_caps & MALLOC_CAP_SPIRAM) != 0);
    TEST_ASSERT_EQUAL_PTR(allocation.queueBuffer, g_mock_queue_create_state.lastStorageBuffer);

    vQueueDelete(queue);
    heap_caps_free(allocation.queueBuffer);
}

void test_create_queue_prefer_psram_falls_back_when_alloc_fails() {
    PsramQueueAllocation allocation;
    bool usedPsram = true;
    g_mock_heap_caps_fail_malloc = true;

    QueueHandle_t queue = createQueuePreferPsram(8, sizeof(uint16_t), allocation, &usedPsram);

    TEST_ASSERT_NOT_NULL(queue);
    TEST_ASSERT_FALSE(usedPsram);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_queue_create_state.staticCalls);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_queue_create_state.dynamicCalls);
    TEST_ASSERT_NULL(allocation.queueBuffer);

    vQueueDelete(queue);
}

void test_create_queue_prefer_psram_falls_back_when_static_create_fails() {
    PsramQueueAllocation allocation;
    bool usedPsram = true;
    g_mock_queue_create_state.failStatic = true;

    QueueHandle_t queue = createQueuePreferPsram(2, sizeof(uint64_t), allocation, &usedPsram);

    TEST_ASSERT_NOT_NULL(queue);
    TEST_ASSERT_FALSE(usedPsram);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_queue_create_state.staticCalls);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_queue_create_state.dynamicCalls);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_heap_caps_free_calls);
    TEST_ASSERT_NULL(allocation.queueBuffer);

    vQueueDelete(queue);
}

void test_create_task_internal_stack_uses_internal_caps_allocator() {
    bool usedInternal = false;

    BaseType_t rc = createTaskPinnedToCoreInternalStack(dummyTask,
                                                        "Dummy",
                                                        4096,
                                                        nullptr,
                                                        1,
                                                        nullptr,
                                                        0,
                                                        &usedInternal);

    TEST_ASSERT_EQUAL(pdPASS, rc);
    TEST_ASSERT_TRUE(usedInternal);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_task_create_state.capsCalls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_task_create_state.standardCalls);
    TEST_ASSERT_EQUAL_UINT32(4096, g_mock_task_create_state.lastStackSize);
    TEST_ASSERT_TRUE((g_mock_task_create_state.lastCaps & MALLOC_CAP_INTERNAL) != 0);
    TEST_ASSERT_TRUE((g_mock_task_create_state.lastCaps & MALLOC_CAP_SPIRAM) == 0);
}

void test_create_task_internal_stack_does_not_fall_back_to_uncapped_create() {
    bool usedInternal = true;
    g_mock_task_create_state.failCaps = true;

    BaseType_t rc = createTaskPinnedToCoreInternalStack(dummyTask,
                                                        "Dummy",
                                                        2048,
                                                        nullptr,
                                                        1,
                                                        nullptr,
                                                        1,
                                                        &usedInternal);

    TEST_ASSERT_NOT_EQUAL(pdPASS, rc);
    TEST_ASSERT_FALSE(usedInternal);
    TEST_ASSERT_EQUAL_UINT32(1, g_mock_task_create_state.capsCalls);
    TEST_ASSERT_EQUAL_UINT32(0, g_mock_task_create_state.standardCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_create_queue_prefer_psram_uses_static_psram_queue);
    RUN_TEST(test_create_queue_prefer_psram_falls_back_when_alloc_fails);
    RUN_TEST(test_create_queue_prefer_psram_falls_back_when_static_create_fails);
    RUN_TEST(test_create_task_internal_stack_uses_internal_caps_allocator);
    RUN_TEST(test_create_task_internal_stack_does_not_fall_back_to_uncapped_create);
    return UNITY_END();
}
