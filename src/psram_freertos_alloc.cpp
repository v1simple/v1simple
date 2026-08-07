#include "psram_freertos_alloc.h"

namespace {

constexpr uint32_t kPsram8BitCaps = MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM;
constexpr uint32_t kInternal8BitCaps = MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL;

} // namespace

QueueHandle_t createQueuePreferPsram(UBaseType_t length, UBaseType_t itemSize, PsramQueueAllocation& allocation,
                                     bool* usedPsram) {
    if (usedPsram) {
        *usedPsram = false;
    }

    if (!allocation.queueBuffer) {
        const size_t storageBytes = static_cast<size_t>(length) * static_cast<size_t>(itemSize);
        allocation.queueBuffer = static_cast<uint8_t*>(heap_caps_malloc(storageBytes, kPsram8BitCaps));
    }

    if (allocation.queueBuffer) {
        QueueHandle_t queue = xQueueCreateStatic(length, itemSize, allocation.queueBuffer, &allocation.queueStorage);
        if (queue) {
            if (usedPsram) {
                *usedPsram = true;
            }
            return queue;
        }

        heap_caps_free(allocation.queueBuffer);
        allocation.queueBuffer = nullptr;
    }

    return xQueueCreate(length, itemSize);
}

BaseType_t createTaskPinnedToCoreInternalStack(void (*taskEntry)(void*), const char* taskName, uint32_t stackSize,
                                               void* taskParam, UBaseType_t priority, TaskHandle_t* taskHandle,
                                               BaseType_t coreId, bool* usedInternal) {
    if (usedInternal) {
        *usedInternal = false;
    }

    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(taskEntry, taskName, stackSize, taskParam, priority, taskHandle,
                                                    coreId, kInternal8BitCaps);
    if (rc == pdPASS) {
        if (usedInternal) {
            *usedInternal = true;
        }
    }

    return rc;
}
