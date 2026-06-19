#pragma once

class AutoPushModule {
public:
    enum class QueueResult : uint8_t {
        QUEUED = 0,
        V1_NOT_CONNECTED,
        ALREADY_IN_PROGRESS,
        NO_PROFILE_CONFIGURED,
        PROFILE_LOAD_FAILED,
    };

    QueueResult queueSlotPush(int slotIndex,
                              bool /*activateSlot*/ = false,
                              bool /*updateProfileIndicator*/ = true) {
        ++queueSlotPushCalls;
        lastQueueSlotPushSlot = slotIndex;
        return queueSlotPushResult;
    }

    void reset() {
        queueSlotPushCalls = 0;
        lastQueueSlotPushSlot = -1;
        queueSlotPushResult = QueueResult::QUEUED;
    }

    int queueSlotPushCalls = 0;
    int lastQueueSlotPushSlot = -1;
    QueueResult queueSlotPushResult = QueueResult::QUEUED;
};
