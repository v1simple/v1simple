#pragma once

#include <Arduino.h>
#include <array>
#include <atomic>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "ble_log_rate_limit.h"
#include "ble_client.h"
#include "packet_parser.h"

class DisplayPreviewModule;
class PowerModule;
class V1ProfileManager;

// V1 notification integration boundary, not a generic byte queue. This module
// owns notification-session admission, frame reassembly, parser dispatch, and
// the immediate effects of accepted parsed frames: profile synchronization,
// firmware-version reporting, V1 activity notification to power management,
// preview cancellation when an alert becomes live, and the parsed-work signal
// consumed by the display pipeline.
class BleQueueModule {
  public:
    struct Config {
        size_t queueDepth;
        size_t rxBufferCap;
        Config() : queueDepth(24), rxBufferCap(512) {}
    };

    bool begin(V1BLEClient* bleClient, PacketParser* parser, V1ProfileManager* profileMgr,
               DisplayPreviewModule* previewModule, PowerModule* powerModule, Config cfg = Config());

    // Release the FreeRTOS queue created by begin(). The firmware never tears
    // this module down (the queue lives for the life of the device), but
    // repeated begin() cycles — as native test fixtures do per test case —
    // must not orphan the previous queue allocation.
    void end();
    bool isReady() const { return queueHandle_ != nullptr && rxBufferReady_; }

    // Returns true if a packet was successfully parsed since last check (and clears flag)
    bool consumeParsedFlag() {
        bool had = hadSuccessfulParse_;
        hadSuccessfulParse_ = false;
        return had;
    }

    // Callback entry from BLE notifications.
    void onNotify(const uint8_t* data, size_t length, uint16_t charUUID, uint32_t sessionGeneration,
                  uint32_t callbackMillis, uint32_t ingressSequence);
    bool tryOnNotify(const uint8_t* data, size_t length, uint16_t charUUID, uint32_t sessionGeneration,
                     uint32_t callbackMillis, uint32_t ingressSequence);

    // Open/close the V1 notification boundary. closeSession() rejects new
    // notifications and discards every queued, buffered, and parsed signal
    // from the outgoing link; openSession(generation) admits the new link.
    void openSession(uint32_t sessionGeneration);
    void closeSession();

    // Drain and frame admitted notifications, parse complete packets, apply the
    // integration effects documented above, and signal display work.
    void process();


    unsigned long getLastRxMillis() const { return lastRxMillis_; }
    bool isBackpressured() const { return backpressureActive_; }

#ifdef UNIT_TEST
    bool enqueueStampedForTest(const uint8_t* data, size_t length, uint16_t charUUID, uint32_t sessionGeneration);
#endif

  private:
    static constexpr uint16_t LONG_NOTIFY_CHARACTERISTIC = 0xB4E0;
    static constexpr size_t MAX_LONG_CHUNKS = 15;
    static constexpr size_t MAX_LONG_CHUNK_PAYLOAD = 19;

    struct BLEDataPacket {
        uint8_t data[256];
        size_t length;
        uint16_t charUUID;
        uint32_t tsMs;
        uint32_t sessionGeneration;
        uint32_t ingressSequence;
    };

    // ESP Bluetooth Addendum rev. 4: B4E0 notifications prepend a one-byte
    // index/count field to at most 19 packet bytes. Chunks can be delivered
    // out of order, so they cannot be appended to the B2CE byte stream until
    // every indexed piece is present.
    struct LongRxAssembly {
        uint8_t count = 0;
        uint16_t presentMask = 0;
        std::array<std::array<uint8_t, MAX_LONG_CHUNK_PAYLOAD>, MAX_LONG_CHUNKS> payloads{};
        std::array<uint8_t, MAX_LONG_CHUNKS> lengths{};
        uint32_t firstIngressSequence = 0;
        uint32_t latestTimestampMs = 0;
        bool streamDiscontinuity = false;
    };

    V1BLEClient* ble_ = nullptr;
    PacketParser* parser_ = nullptr;
    V1ProfileManager* profiles_ = nullptr;
    DisplayPreviewModule* preview_ = nullptr;
    PowerModule* power_ = nullptr;
    QueueHandle_t queueHandle_ = nullptr;
    std::atomic<bool> acceptNotifications_{false};
    std::atomic<uint32_t> sessionGeneration_{0};
    std::vector<uint8_t> rxBuffer_;
    // Parallel first-arrival provenance for every staged byte. A frame uses
    // the sequence attached to its start byte so a response that began before
    // a command cannot become fresh merely by completing afterward.
    std::vector<uint32_t> rxIngressSequences_;
    // Marks the first staged byte after a known notification-stream gap. The
    // parser is invalidated only when consumption reaches that exact boundary.
    std::vector<uint8_t> rxDiscontinuities_;
    LongRxAssembly longRx_;
    bool rxBufferReady_ = false;
    size_t rxReadPos_ = 0; // Logical read pointer into rxBuffer (avoids front erases)
    unsigned long lastRxMillis_ = 0;
    uint32_t lastNotifyTsMs_ = 0;
    bool hadSuccessfulParse_ = false; // Flag: at least one packet parsed since last check
    bool backpressureActive_ = false;
    uint32_t lastDequeuedIngressSequence_ = 0;
    bool pendingStreamDiscontinuity_ = false;
    BleLogRateLimitState tooLargeWarningLog_;
    BleLogRateLimitState missingEndWarningLog_;

    Config config_;
    void refreshBackpressureState();
    bool appendRxBytes(const uint8_t* data, size_t length, uint32_t ingressSequence,
                       bool beginsAfterDiscontinuity = false);
    bool appendRxPacket(const BLEDataPacket& packet, bool beginsAfterDiscontinuity);
    bool appendLongRxChunk(const BLEDataPacket& packet, bool beginsAfterDiscontinuity);
    bool flushCompleteLongRx();
    bool longRxComplete() const;
    bool ingressWouldBeDiscontinuous(uint32_t ingressSequence) const;
    void acceptIngressSequence(uint32_t ingressSequence);
    void markAlertStreamDiscontinuous();
    void applyPendingDiscontinuityIfStreamDrained();
    void compactRxState();
    void clearRxState();
    void clearLongRxState();
};
