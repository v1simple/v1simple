#pragma once

#include <Arduino.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "ble_log_rate_limit.h"
#include "ble_client.h"
#include "packet_parser.h"

class SystemEventBus;
class DisplayPreviewModule;
class PowerModule;
class V1ProfileManager;

class BleQueueModule {
  public:
    struct Config {
        size_t queueDepth;
        size_t rxBufferCap;
        Config() : queueDepth(24), rxBufferCap(512) {}
    };

    bool begin(V1BLEClient* bleClient, PacketParser* parser, V1ProfileManager* profileMgr,
               DisplayPreviewModule* previewModule, PowerModule* powerModule, SystemEventBus* eventBus = nullptr,
               Config cfg = Config());

    // Release the FreeRTOS queue created by begin(). The firmware never tears
    // this module down (the queue lives for the life of the device), but
    // repeated begin() cycles — as native test fixtures do per test case —
    // must not orphan the previous queue allocation.
    void end();

    bool isReady() const { return queueHandle_ != nullptr && rxBufferReady_; }

    // Returns timestamp of last successfully parsed packet (for display latency tracking)
    uint32_t getLastParsedTimestamp() const { return lastParsedTsMs_; }

    // Returns true if a packet was successfully parsed since last check (and clears flag)
    bool consumeParsedFlag() {
        bool had = hadSuccessfulParse_;
        hadSuccessfulParse_ = false;
        return had;
    }

    // Callback entry from BLE notifications.
    void onNotify(const uint8_t* data, size_t length, uint16_t charUUID);

    // Drain queue, frame packets, parse, and forward to display pipeline.
    void process();

    unsigned long getLastRxMillis() const { return lastRxMillis_; }
    bool isBackpressured() const { return backpressureActive_; }

  private:
    struct BLEDataPacket {
        uint8_t data[256];
        size_t length;
        uint16_t charUUID;
        uint32_t tsMs;
    };

    V1BLEClient* ble_ = nullptr;
    PacketParser* parser_ = nullptr;
    V1ProfileManager* profiles_ = nullptr;
    DisplayPreviewModule* preview_ = nullptr;
    PowerModule* power_ = nullptr;
    SystemEventBus* bus_ = nullptr;
    QueueHandle_t queueHandle_ = nullptr;
    // onNotify() runs on the serialized NimBLE host callback task. Reuse this
    // member when evicting the oldest queue entry so the callback does not put
    // a second ~272-byte BLEDataPacket on its limited stack.
    BLEDataPacket dropScratch_{};

    std::vector<uint8_t> rxBuffer_;
    bool rxBufferReady_ = false;
    size_t rxReadPos_ = 0; // Logical read pointer into rxBuffer (avoids front erases)
    unsigned long lastRxMillis_ = 0;
    uint32_t lastNotifyTsMs_ = 0;
    uint32_t lastParsedTsMs_ = 0;     // Timestamp of last successful parse (for display latency)
    bool hadSuccessfulParse_ = false; // Flag: at least one packet parsed since last check
    uint32_t parsedEventSeq_ = 0;
    bool backpressureActive_ = false;
    BleLogRateLimitState tooLargeWarningLog_;
    BleLogRateLimitState missingEndWarningLog_;

    Config config_;
    void refreshBackpressureState();
};
