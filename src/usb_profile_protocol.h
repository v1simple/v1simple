#pragma once

#include <cstddef>
#include <cstdint>

// USB is a requested configuration transport, not a telemetry stream. Bulk
// work is admitted only in maintenance; the normal-loop parser stays fixed-size.
struct UsbProfileStatus {
    bool maintenance = false;
    bool busy = true;
    uint32_t boot = 0;
    const char* git = "unknown";
    const char* image = "unknown";
    unsigned slot = 0;
    unsigned persistence = 0;
    bool enabled = false;
};

class UsbProfileTransport {
  public:
    virtual ~UsbProfileTransport() = default;
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int availableForWrite() = 0;
    virtual size_t write(const uint8_t* data, size_t length) = 0;
};

class UsbProfileBackend {
  public:
    virtual ~UsbProfileBackend() = default;
    virtual UsbProfileStatus status() const = 0;
    virtual uint8_t* allocate(size_t bytes) = 0;
    virtual void release(uint8_t* data) = 0;
    virtual bool backup(uint8_t*& data, size_t& length, char* error, size_t errorSize) = 0;
    virtual bool apply(const uint8_t* data, size_t length, bool& backupPending, int& profiles,
                       char* error, size_t errorSize) = 0;
    virtual void enterMaintenance() = 0;
    virtual void restartNormal() = 0;
    virtual void activity(uint32_t nowMs) = 0;
};

class UsbProfileProtocol {
  public:
    static constexpr size_t MaxDocumentBytes = 128 * 1024;
    static constexpr size_t ChunkBytes = 64;
    static constexpr size_t FrameBytes = 256;
    static constexpr size_t RxBytesPerTick = 64;
    static constexpr uint32_t SessionIdleMs = 60000;
    static constexpr uint32_t FrameIdleMs = 2000;

    UsbProfileProtocol(UsbProfileTransport& transport, UsbProfileBackend& backend);
    ~UsbProfileProtocol();
    UsbProfileProtocol(const UsbProfileProtocol&) = delete;
    UsbProfileProtocol& operator=(const UsbProfileProtocol&) = delete;
    void tick(uint32_t nowMs);
    static uint32_t crc32(const uint8_t* data, size_t length);

  private:
    enum class Action { None, Maintenance, Normal };
    void dispatch(uint32_t nowMs);
    void reply(uint32_t id, const char* json);
    void fail(uint32_t id, const char* code, const char* detail = nullptr);
    void clearTransfer();
    void transmit(uint32_t nowMs);

    UsbProfileTransport& transport_;
    UsbProfileBackend& backend_;
    char input_[FrameBytes]{};
    char output_[FrameBytes]{};
    char lastRequest_[FrameBytes]{};
    char lastReply_[FrameBytes]{};
    size_t inputLength_ = 0;
    size_t outputLength_ = 0;
    uint32_t lastId_ = 0;
    uint32_t inputAt_ = 0;
    uint32_t outputAt_ = 0;
    uint32_t transferAt_ = 0;
    bool discarding_ = false;
    Action action_ = Action::None;
    Action cachedAction_ = Action::None;
    uint8_t* document_ = nullptr;
    size_t documentLength_ = 0;
    size_t received_ = 0;
    uint32_t expectedCrc_ = 0;
    bool receiving_ = false;
};
