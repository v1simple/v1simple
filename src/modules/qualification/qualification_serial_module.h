#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "modules/ble/ble_proxy_epoch_observer.h"

class QualificationSerialModule {
  public:
    enum class Suite : uint8_t {
        Core,
        Display,
    };

    enum class Mode : uint8_t {
        Current,
        Proxy,
        Obd,
        V1Only,
    };

    struct Providers {
        bool (*isPerfEnabled)(void* ctx) = nullptr;
        const char* (*perfCsvPath)(void* ctx) = nullptr;
        void (*startPerfSession)(void* ctx) = nullptr;
        bool (*enqueueSnapshotNow)(void* ctx) = nullptr;
        bool (*tryDrainPerf)(void* ctx) = nullptr;
        void (*setSdCapturePaused)(bool paused, void* ctx) = nullptr;
        void (*startDisplayPreview)(uint32_t durationMs, void* ctx) = nullptr;
        void (*cancelDisplayPreview)(void* ctx) = nullptr;
        bool (*applyQualificationMode)(uint8_t mode, void* ctx) = nullptr;
        void (*clearQualificationMode)(void* ctx) = nullptr;
        bool (*isStorageReady)(void* ctx) = nullptr;
        bool (*isSDCard)(void* ctx) = nullptr;
        fs::FS* (*filesystem)(void* ctx) = nullptr;
        SemaphoreHandle_t (*sdMutex)(void* ctx) = nullptr;
        bool (*tryProxyEpochSnapshot)(BleProxyEpochQualificationSnapshot& snapshot, void* ctx) = nullptr;
        uint32_t (*nowMs)(void* ctx) = nullptr;
        void* ctx = nullptr;
    };

    void begin(Stream* io, const Providers& providers);
    void process();

    bool isRunning() const { return state_ == State::Running || state_ == State::Finalizing; }
    bool isDone() const { return state_ == State::Done; }

  private:
    enum class State : uint8_t {
        Idle,
        Running,
        Finalizing,
        Done,
        Error,
    };

    static constexpr size_t kCommandBufferLen = 160;
    static constexpr size_t kPathBufferLen = 96;
    static constexpr size_t kExportChunkBytes = 64;
    static constexpr int kExportWritableHeadroom = static_cast<int>((kExportChunkBytes * 2) + 64);
    static constexpr uint32_t kFinalizeTimeoutMs = 2500;

    Stream* io_ = nullptr;
    Providers providers_{};
    State state_ = State::Idle;
    Suite suite_ = Suite::Core;
    Mode mode_ = Mode::Current;
    uint32_t durationMs_ = 0;
    uint32_t startedAtMs_ = 0;
    uint32_t finalizingAtMs_ = 0;
    bool finalSnapshotQueued_ = false;
    bool finalizedOk_ = false;
    char csvPath_[kPathBufferLen] = {0};
    char lastError_[64] = {0};

    char commandBuf_[kCommandBufferLen] = {0};
    size_t commandLen_ = 0;

    bool exportActive_ = false;
    File exportFile_{};
    uint32_t exportCrc_ = 0xFFFFFFFFUL;
    uint32_t exportBytes_ = 0;
    uint32_t exportSize_ = 0;
    uint32_t exportChunks_ = 0;

    uint32_t nowMs() const;
    const char* stateName() const;
    const char* suiteName() const;
    const char* modeName() const;

    void serviceInput();
    void serviceRun();
    void serviceExport();
    void handleCommand(char* line);
    void handleStart(char* args);
    void handleStatus();
    void handleGetCsv(char* args);
    void handleBsc08(char* args);
    void handleAbort();

    bool parseStartArgs(char* args, Suite& suite, uint32_t& durationSeconds, Mode& mode) const;
    bool startRun(Suite suite, uint32_t durationSeconds, Mode mode);
    void enterFinalizing(const char* reason);
    void finishRun(bool ok, const char* message);
    void clearQualificationModeOverride();
    bool openExport(const char* path);
    void closeExport();
    void setError(const char* message);

    void sendStatusLine(const char* prefix, bool ok, const char* message = nullptr);
    void sendErrorLine(const char* message);
    void printJsonString(const char* value);

    static uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len);
    static bool validNonce(const char* value);
    static char* trim(char* text);
};
