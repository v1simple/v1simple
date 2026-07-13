// Mock FreeRTOS types for native testing
#pragma once

#include <cstdint>
#include <deque>
#include <vector>
#include <algorithm>
#include <cstring>
#include "../mock_heap_caps_state.h"

// Semaphore/Mutex types
typedef void* SemaphoreHandle_t;
typedef void* QueueHandle_t;
typedef void* TaskHandle_t;
typedef struct StaticQueue_t {
    uint8_t unused = 0;
} StaticQueue_t;

// Tick type
typedef uint32_t TickType_t;
#define portMAX_DELAY 0xFFFFFFFF

typedef int BaseType_t;
typedef unsigned int UBaseType_t;

#ifndef pdTRUE
#define pdTRUE 1
#endif
#ifndef pdFALSE
#define pdFALSE 0
#endif
#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(ms) (ms)
#endif

// Critical section stubs (ESP32 portMUX_TYPE)
typedef struct { int dummy; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux)  ((void)(mux))
#define portENTER_CRITICAL_ISR(mux) ((void)(mux))
#define portEXIT_CRITICAL_ISR(mux)  ((void)(mux))

#ifndef configASSERT
#define configASSERT(x) ((void)(x))
#endif

// Semaphore stubs
struct MockSemaphoreState {
    uint32_t takeCalls = 0;
    uint32_t giveCalls = 0;
    TickType_t lastTakeTimeout = 0;
    std::deque<int> takeResults;
};

inline MockSemaphoreState g_mock_semaphore_state{};

inline void mock_reset_semaphore_state() {
    g_mock_semaphore_state = MockSemaphoreState{};
}

inline void mock_queue_semaphore_take_result(int result) {
    g_mock_semaphore_state.takeResults.push_back(result);
}

inline SemaphoreHandle_t xSemaphoreCreateMutex() { return (void*)1; }
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return (void*)1; }
inline int xSemaphoreTake(SemaphoreHandle_t, TickType_t timeoutTicks) {
    g_mock_semaphore_state.takeCalls++;
    g_mock_semaphore_state.lastTakeTimeout = timeoutTicks;
    if (!g_mock_semaphore_state.takeResults.empty()) {
        const int result = g_mock_semaphore_state.takeResults.front();
        g_mock_semaphore_state.takeResults.pop_front();
        return result;
    }
    return 1;
}
inline int xSemaphoreGive(SemaphoreHandle_t) {
    g_mock_semaphore_state.giveCalls++;
    return 1;
}
inline void vSemaphoreDelete(SemaphoreHandle_t) {}

// Queue stubs
struct MockQueueState {
    uint32_t capacity = 0;
    uint32_t itemSize = 0;
    std::deque<std::vector<uint8_t>> items;
};

struct MockQueueCreateState {
    uint32_t dynamicCalls = 0;
    uint32_t staticCalls = 0;
    uint32_t lastLength = 0;
    uint32_t lastItemSize = 0;
    uint8_t* lastStorageBuffer = nullptr;
    bool failDynamic = false;
    bool failStatic = false;
};

inline MockQueueCreateState g_mock_queue_create_state{};

inline void mock_reset_queue_create_state() {
    g_mock_queue_create_state = MockQueueCreateState{};
}

inline QueueHandle_t xQueueCreate(uint32_t length, uint32_t itemSize) {
    g_mock_queue_create_state.dynamicCalls++;
    g_mock_queue_create_state.lastLength = length;
    g_mock_queue_create_state.lastItemSize = itemSize;
    g_mock_queue_create_state.lastStorageBuffer = nullptr;
    if (g_mock_queue_create_state.failDynamic) {
        return nullptr;
    }
    MockQueueState* q = new MockQueueState();
    q->capacity = length;
    q->itemSize = itemSize;
    return reinterpret_cast<QueueHandle_t>(q);
}

inline QueueHandle_t xQueueCreateStatic(uint32_t length,
                                        uint32_t itemSize,
                                        uint8_t* storageBuffer,
                                        StaticQueue_t*) {
    g_mock_queue_create_state.staticCalls++;
    g_mock_queue_create_state.lastLength = length;
    g_mock_queue_create_state.lastItemSize = itemSize;
    g_mock_queue_create_state.lastStorageBuffer = storageBuffer;
    if (g_mock_queue_create_state.failStatic || storageBuffer == nullptr) {
        return nullptr;
    }
    MockQueueState* q = new MockQueueState();
    q->capacity = length;
    q->itemSize = itemSize;
    return reinterpret_cast<QueueHandle_t>(q);
}

inline BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t) {
    if (!queue || !item) return pdFALSE;
    MockQueueState* q = reinterpret_cast<MockQueueState*>(queue);
    if (q->items.size() >= q->capacity) return pdFALSE;
    const uint8_t* bytes = static_cast<const uint8_t*>(item);
    q->items.emplace_back(bytes, bytes + q->itemSize);
    return pdTRUE;
}

inline BaseType_t xQueueReceive(QueueHandle_t queue, void* out, TickType_t) {
    if (!queue || !out) return pdFALSE;
    MockQueueState* q = reinterpret_cast<MockQueueState*>(queue);
    if (q->items.empty()) return pdFALSE;
    std::vector<uint8_t> item = std::move(q->items.front());
    q->items.pop_front();
    std::memcpy(out, item.data(), std::min<size_t>(q->itemSize, item.size()));
    return pdTRUE;
}

inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) {
    if (!queue) return 0;
    MockQueueState* q = reinterpret_cast<MockQueueState*>(queue);
    return static_cast<UBaseType_t>(q->items.size());
}

inline void vQueueDelete(QueueHandle_t queue) {
    MockQueueState* q = reinterpret_cast<MockQueueState*>(queue);
    delete q;
}

struct MockTaskCreateState {
    uint32_t standardCalls = 0;
    uint32_t capsCalls = 0;
    uint32_t lastStackSize = 0;
    UBaseType_t lastPriority = 0;
    BaseType_t lastCore = 0;
    uint32_t lastCaps = 0;
    bool failStandard = false;
    bool failCaps = false;
};

inline MockTaskCreateState g_mock_task_create_state{};

inline void mock_reset_task_create_state() {
    g_mock_task_create_state = MockTaskCreateState{};
}

struct MockTaskDeleteState {
    uint32_t standardCalls = 0;
    uint32_t capsCalls = 0;
};

inline MockTaskDeleteState g_mock_task_delete_state{};

inline void mock_reset_task_delete_state() {
    g_mock_task_delete_state = MockTaskDeleteState{};
}

// Task stubs
inline void vTaskDelay(TickType_t) {}
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return nullptr; }

// taskENTER/EXIT_CRITICAL convenience aliases used by some IDF headers
#define taskENTER_CRITICAL(mux) portENTER_CRITICAL(mux)
#define taskEXIT_CRITICAL(mux)  portEXIT_CRITICAL(mux)

// ESP-specific
inline uint32_t esp_get_free_heap_size() { return 320000; }
inline uint32_t heap_caps_get_free_size(int) { return g_mock_heap_caps_free_size; }
#ifndef MALLOC_CAP_INTERNAL
#define MALLOC_CAP_INTERNAL 0
#endif
#ifndef MALLOC_CAP_8BIT
#define MALLOC_CAP_8BIT 0
#endif
#ifndef MALLOC_CAP_SPIRAM
#define MALLOC_CAP_SPIRAM 0
#endif
#ifndef tskNO_AFFINITY
#define tskNO_AFFINITY (-1)
#endif
#ifndef pdPASS
#define pdPASS 1
#endif

// Task stack and static-task types
typedef uint8_t StackType_t;
typedef struct { uint8_t unused; } StaticTask_t;

// Pull in task creation/deletion stubs so any TU that only includes FreeRTOS.h
// still gets xTaskCreatePinnedToCoreWithCaps, vTaskDeleteWithCaps, etc.
#include "task.h"
