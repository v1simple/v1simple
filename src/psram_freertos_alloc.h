#pragma once

#include <stdint.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

struct PsramQueueAllocation {
    StaticQueue_t queueStorage = {};
    uint8_t* queueBuffer = nullptr;
};

QueueHandle_t createQueuePreferPsram(UBaseType_t length, UBaseType_t itemSize, PsramQueueAllocation& allocation,
                                     bool* usedPsram = nullptr);

BaseType_t createTaskPinnedToCoreInternalStack(void (*taskEntry)(void*), const char* taskName, uint32_t stackSize,
                                               void* taskParam, UBaseType_t priority, TaskHandle_t* taskHandle,
                                               BaseType_t coreId, bool* usedInternal = nullptr);
