#pragma once

#include "FreeRTOS.h"

// BaseType_t/UBaseType_t come from FreeRTOS.h
// pdPASS/pdTRUE/pdFALSE/pdMS_TO_TICKS come from FreeRTOS.h

inline BaseType_t xTaskCreatePinnedToCore(void (*)(void*),
                                          const char*,
                                          uint32_t,
                                          void*,
                                          UBaseType_t,
                                          TaskHandle_t*,
                                          BaseType_t) {
    g_mock_task_create_state.standardCalls++;
    g_mock_task_create_state.lastCaps = 0;
    if (g_mock_task_create_state.failStandard) {
        return pdFALSE;
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

inline uint32_t ulTaskNotifyTake(BaseType_t, TickType_t) { return 0; }
inline void xTaskNotifyGive(TaskHandle_t) {}
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
