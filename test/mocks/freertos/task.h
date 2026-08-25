#pragma once

#include "FreeRTOS.h"

// BaseType_t/UBaseType_t come from FreeRTOS.h
// pdPASS/pdTRUE/pdFALSE/pdMS_TO_TICKS come from FreeRTOS.h

inline BaseType_t xTaskCreatePinnedToCore(void (*)(void*),
                                          const char*,
                                          uint32_t stackSize,
                                          void*,
                                          UBaseType_t priority,
                                          TaskHandle_t* taskHandle,
                                          BaseType_t core) {
    g_mock_task_create_state.standardCalls++;
    g_mock_task_create_state.lastStackSize = stackSize;
    g_mock_task_create_state.lastPriority = priority;
    g_mock_task_create_state.lastCore = core;
    g_mock_task_create_state.lastCaps = 0;
    g_mock_task_create_state.lastTaskHandleOutput = taskHandle;
    if (g_mock_task_create_state.failStandard) {
        return pdFALSE;
    }
    if (taskHandle) {
        *taskHandle = reinterpret_cast<TaskHandle_t>(1);
    }
    return pdPASS;
}

inline BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*)(void*),
                                                  const char*,
                                                  uint32_t stackSize,
                                                  void*,
                                                  UBaseType_t priority,
                                                  TaskHandle_t*,
                                                  BaseType_t core,
                                                  uint32_t caps) {
    g_mock_task_create_state.capsCalls++;
    g_mock_task_create_state.lastStackSize = stackSize;
    g_mock_task_create_state.lastPriority = priority;
    g_mock_task_create_state.lastCore = core;
    g_mock_task_create_state.lastCaps = caps;
    if (g_mock_task_create_state.failCaps) {
        return pdFALSE;
    }
    return pdPASS;
}

inline void vTaskDelete(void*) {
    g_mock_task_delete_state.standardCalls++;
}

inline void vTaskDeleteWithCaps(void*) {
    g_mock_task_delete_state.capsCalls++;
}

inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 1024; }
inline uint32_t ulTaskNotifyTake(BaseType_t, TickType_t) { return 0; }

struct MockTaskNotifyState {
    uint32_t giveCalls = 0;
    TaskHandle_t lastHandle = nullptr;
};

inline MockTaskNotifyState g_mock_task_notify_state{};

inline void mock_reset_task_notify_state() {
    g_mock_task_notify_state = MockTaskNotifyState{};
}

inline void xTaskNotifyGive(TaskHandle_t handle) {
    g_mock_task_notify_state.giveCalls++;
    g_mock_task_notify_state.lastHandle = handle;
}
inline void taskYIELD() {}

inline TaskHandle_t xTaskCreateStaticPinnedToCore(
    void (*pxTaskCode)(void*),
    const char* /*pcName*/,
    uint32_t /*usStackDepth*/,
    void* pvParameters,
    UBaseType_t /*uxPriority*/,
    StackType_t* /*puxStackBuffer*/,
    StaticTask_t* /*pxTaskBuffer*/,
    BaseType_t /*xCoreID*/) {
    (void)pxTaskCode; (void)pvParameters;
    g_mock_task_create_state.standardCalls++;
    return (TaskHandle_t)1;
}
