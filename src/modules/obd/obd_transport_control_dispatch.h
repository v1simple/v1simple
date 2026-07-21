#pragma once

#include <atomic>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Main-loop cancellation is published before its priority control item is
// queued. A transport request claims the current epoch immediately before it
// enters the low-level client call; work from an older epoch is discarded.
class ObdTransportRequestEpoch {
  public:
    uint32_t snapshot() const { return token_.load(std::memory_order_acquire) & ~uint32_t{1}; }

    void cancelQueuedWork() { token_.fetch_add(2, std::memory_order_acq_rel); }

    bool tryClaim(uint32_t requestEpoch) {
        uint32_t expected = requestEpoch;
        return token_.compare_exchange_strong(expected, requestEpoch | uint32_t{1}, std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

    void releaseClaim() { token_.fetch_and(~uint32_t{1}, std::memory_order_acq_rel); }

  private:
    std::atomic<uint32_t> token_{0};
};

// Stateful priority-control dispatcher. A control request purges queued data
// work, optionally deletes the bond while connection identity is available,
// and then retries disconnect initiation until accepted. Completion is not
// acknowledged until the client reports fully disconnected on two service
// passes; the transport task delays between passes so NimBLE can finish its
// callback bookkeeping after publishing link-down.
template <typename Request> class ObdTransportControlDispatch {
  public:
    static constexpr uint8_t MAX_COMMAND_ATTEMPTS = 3;
    static constexpr uint16_t MAX_CONFIRM_PASSES = 250;

    template <typename Client, typename Ack>
    bool service(QueueHandle_t controlQueue, QueueHandle_t requestQueue, Client& client, Ack acknowledge) {
        if (!active_) {
            if (!controlQueue || xQueueReceive(controlQueue, &request_, 0) != pdTRUE) {
                return false;
            }

            active_ = true;
            commandAccepted_ = false;
            settledPasses_ = 0;
            commandAttempts_ = 0;
            confirmPasses_ = 0;
            targetGeneration_ = client.activeLinkGeneration();
            bondDeleteAttempted_ = request_.deleteBond;
            bondDeleted_ = !request_.deleteBond;
            if (requestQueue) {
                xQueueReset(requestQueue);
            }
            client.serviceDeferredLinkState();
            if (request_.deleteBond) {
                bondDeleted_ = client.deleteBond(request_.address, request_.addrType);
            }
        }

        client.serviceDeferredLinkState();
        if (!commandAccepted_) {
            commandAttempts_++;
            if (!client.disconnect()) {
                settledPasses_ = 0;
                if (commandAttempts_ >= MAX_COMMAND_ATTEMPTS) {
                    acknowledge(request_, bondDeleteAttempted_, bondDeleted_, false, false);
                    reset();
                }
                return true;
            }
            commandAccepted_ = true;
            settledPasses_ = 0;
            confirmPasses_ = 0;
            return true;
        }

        if (!client.linkDownConfirmed(targetGeneration_)) {
            settledPasses_ = 0;
            if (++confirmPasses_ >= MAX_CONFIRM_PASSES) {
                acknowledge(request_, bondDeleteAttempted_, bondDeleted_, false, true);
                reset();
            }
            return true;
        }

        if (++settledPasses_ < 2) {
            return true;
        }

        client.serviceDeferredLinkState();
        acknowledge(request_, bondDeleteAttempted_, bondDeleted_, true, false);
        reset();
        return true;
    }

    bool active() const { return active_; }

  private:
    void reset() {
        active_ = false;
        commandAccepted_ = false;
        settledPasses_ = 0;
        commandAttempts_ = 0;
        confirmPasses_ = 0;
    }

    Request request_{};
    bool active_ = false;
    bool commandAccepted_ = false;
    bool bondDeleteAttempted_ = false;
    bool bondDeleted_ = false;
    uint8_t settledPasses_ = 0;
    uint8_t commandAttempts_ = 0;
    uint16_t confirmPasses_ = 0;
    uint32_t targetGeneration_ = 0;
};
