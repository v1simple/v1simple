#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include <cstddef>
#include <cstdint>
#include <vector>

struct ScannedNetwork {
    String ssid;
    int32_t rssi = 0;
    uint8_t encryptionType = WIFI_AUTH_OPEN;

    bool isOpen() const { return encryptionType == WIFI_AUTH_OPEN; }
};

enum class WifiScanConsumer : uint8_t {
    UI = 0,
    MAINTENANCE,
};

class WifiScanStaDropGate {
  public:
    void request() { pending_ = true; }
    void clear() { pending_ = false; }
    bool pending() const { return pending_; }

    bool takeIfReady(bool scanRunning) {
        if (!pending_ || scanRunning) {
            return false;
        }
        pending_ = false;
        return true;
    }

  private:
    bool pending_ = false;
};

class WifiScanResultOwner {
  public:
    struct Driver {
        void* ctx = nullptr;
        int16_t runningStatus = -1;
        int16_t (*start)(void* ctx) = nullptr;
        int16_t (*status)(void* ctx) = nullptr;
        String (*ssidAt)(int16_t index, void* ctx) = nullptr;
        int32_t (*rssiAt)(int16_t index, void* ctx) = nullptr;
        uint8_t (*encryptionAt)(int16_t index, void* ctx) = nullptr;
        void (*release)(void* ctx) = nullptr;
        void (*abort)(void* ctx) = nullptr;
    };

    enum class RequestResult : uint8_t {
        STARTED = 0,
        JOINED,
        FAILED,
    };

    enum class HarvestResult : uint8_t {
        IDLE = 0,
        RUNNING,
        COMPLETED,
        FAILED,
    };

    RequestResult request(WifiScanConsumer consumer, const Driver& driver);
    HarvestResult harvest(const Driver& driver);
    bool cancel(WifiScanConsumer consumer, const Driver& driver);
    void reset(const Driver& driver);

    bool isRunning() const { return running_; }
    bool isPending(WifiScanConsumer consumer) const;
    bool hasSnapshot(WifiScanConsumer consumer) const;
    uint32_t generation() const { return generation_; }
    uint32_t snapshotGeneration(WifiScanConsumer consumer) const;
    std::vector<ScannedNetwork> copySnapshot(WifiScanConsumer consumer) const;
    void clearSnapshot(WifiScanConsumer consumer);

  private:
    static constexpr size_t CONSUMER_COUNT = 2;

    static size_t consumerIndex(WifiScanConsumer consumer);
    bool hasPendingConsumer() const;
    void clearPendingGeneration(uint32_t generation);

    bool running_ = false;
    uint32_t generation_ = 0;
    uint32_t activeGeneration_ = 0;
    bool pending_[CONSUMER_COUNT] = {};
    uint32_t pendingGeneration_[CONSUMER_COUNT] = {};
    bool snapshotValid_[CONSUMER_COUNT] = {};
    uint32_t snapshotGeneration_[CONSUMER_COUNT] = {};
    std::vector<ScannedNetwork> snapshots_[CONSUMER_COUNT];
};
