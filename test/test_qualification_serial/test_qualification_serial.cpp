#include <unity.h>

#include "../mocks/Arduino.h"

#include <string>

unsigned long mockMillis = 0;

class Stream {
public:
    void feed(const char* text) {
        input_ += text ? text : "";
    }

    int available() const {
        return readPos_ < input_.size() ? static_cast<int>(input_.size() - readPos_) : 0;
    }

    int read() {
        if (readPos_ >= input_.size()) {
            return -1;
        }
        return static_cast<unsigned char>(input_[readPos_++]);
    }

    int availableForWrite() const { return writable_; }
    void setAvailableForWrite(int writable) { writable_ = writable; }

    void print(const char* value) { output_ += value ? value : ""; }
    void print(char value) { output_.push_back(value); }
    void print(uint32_t value) { output_ += std::to_string(value); }
    void print(unsigned long value) { output_ += std::to_string(value); }
    void print(int value) { output_ += std::to_string(value); }

    void println(const char* value = "") {
        print(value);
        output_.push_back('\n');
    }

    const std::string& output() const { return output_; }
    bool outputContains(const char* needle) const {
        return output_.find(needle ? needle : "") != std::string::npos;
    }
    void clearOutput() { output_.clear(); }

private:
    std::string input_;
    size_t readPos_ = 0;
    std::string output_;
    int writable_ = 512;
};

#include "../../src/modules/qualification/qualification_serial_module.cpp"

struct Harness {
    bool perfEnabled = true;
    bool drainReturn = true;
    bool applyModeReturn = true;
    bool storageReady = false;
    bool sdCard = false;
    bool proxySnapshotReady = true;
    BleProxyEpochQualificationSnapshot proxySnapshot{};
    const char* path = "/perf/qualification.csv";
    uint32_t now = 0;
    uint8_t lastMode = 0;
    uint32_t previewDurationMs = 0;
    int startPerfCalls = 0;
    int enqueueSnapshotCalls = 0;
    int tryDrainCalls = 0;
    int pauseTrueCalls = 0;
    int pauseFalseCalls = 0;
    int startPreviewCalls = 0;
    int cancelPreviewCalls = 0;
    int applyModeCalls = 0;
    int clearModeCalls = 0;
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();

    void reset() {
        perfEnabled = true;
        drainReturn = true;
        applyModeReturn = true;
        storageReady = false;
        sdCard = false;
        proxySnapshotReady = true;
        proxySnapshot = BleProxyEpochQualificationSnapshot{};
        path = "/perf/qualification.csv";
        now = 0;
        lastMode = 0;
        previewDurationMs = 0;
        startPerfCalls = 0;
        enqueueSnapshotCalls = 0;
        tryDrainCalls = 0;
        pauseTrueCalls = 0;
        pauseFalseCalls = 0;
        startPreviewCalls = 0;
        cancelPreviewCalls = 0;
        applyModeCalls = 0;
        clearModeCalls = 0;
        mutex = xSemaphoreCreateMutex();
    }

    static bool isPerfEnabled(void* ctx) { return static_cast<Harness*>(ctx)->perfEnabled; }
    static const char* perfCsvPath(void* ctx) { return static_cast<Harness*>(ctx)->path; }
    static void startPerfSession(void* ctx) { static_cast<Harness*>(ctx)->startPerfCalls++; }
    static bool enqueueSnapshotNow(void* ctx) {
        static_cast<Harness*>(ctx)->enqueueSnapshotCalls++;
        return true;
    }
    static bool tryDrainPerf(void* ctx) {
        Harness* h = static_cast<Harness*>(ctx);
        h->tryDrainCalls++;
        return h->drainReturn;
    }
    static void setSdCapturePaused(bool paused, void* ctx) {
        Harness* h = static_cast<Harness*>(ctx);
        if (paused) {
            h->pauseTrueCalls++;
        } else {
            h->pauseFalseCalls++;
        }
    }
    static void startDisplayPreview(uint32_t durationMs, void* ctx) {
        Harness* h = static_cast<Harness*>(ctx);
        h->startPreviewCalls++;
        h->previewDurationMs = durationMs;
    }
    static void cancelDisplayPreview(void* ctx) { static_cast<Harness*>(ctx)->cancelPreviewCalls++; }
    static bool applyQualificationMode(uint8_t mode, void* ctx) {
        Harness* h = static_cast<Harness*>(ctx);
        h->applyModeCalls++;
        h->lastMode = mode;
        return h->applyModeReturn;
    }
    static void clearQualificationMode(void* ctx) { static_cast<Harness*>(ctx)->clearModeCalls++; }
    static bool isStorageReady(void* ctx) { return static_cast<Harness*>(ctx)->storageReady; }
    static bool isSDCard(void* ctx) { return static_cast<Harness*>(ctx)->sdCard; }
    static fs::FS* filesystem(void* /*ctx*/) { return nullptr; }
    static SemaphoreHandle_t sdMutex(void* ctx) { return static_cast<Harness*>(ctx)->mutex; }
    static bool tryProxyEpochSnapshot(BleProxyEpochQualificationSnapshot& snapshot, void* ctx) {
        Harness* h = static_cast<Harness*>(ctx);
        if (!h->proxySnapshotReady) {
            return false;
        }
        snapshot = h->proxySnapshot;
        return true;
    }
    static uint32_t nowMs(void* ctx) { return static_cast<Harness*>(ctx)->now; }

    QualificationSerialModule::Providers providers() {
        QualificationSerialModule::Providers p{};
        p.isPerfEnabled = isPerfEnabled;
        p.perfCsvPath = perfCsvPath;
        p.startPerfSession = startPerfSession;
        p.enqueueSnapshotNow = enqueueSnapshotNow;
        p.tryDrainPerf = tryDrainPerf;
        p.setSdCapturePaused = setSdCapturePaused;
        p.startDisplayPreview = startDisplayPreview;
        p.cancelDisplayPreview = cancelDisplayPreview;
        p.applyQualificationMode = applyQualificationMode;
        p.clearQualificationMode = clearQualificationMode;
        p.isStorageReady = isStorageReady;
        p.isSDCard = isSDCard;
        p.filesystem = filesystem;
        p.sdMutex = sdMutex;
        p.tryProxyEpochSnapshot = tryProxyEpochSnapshot;
        p.nowMs = nowMs;
        p.ctx = this;
        return p;
    }
};

static Stream io;
static Harness harness;
static QualificationSerialModule module;

void setUp() {
    io = Stream();
    harness.reset();
    module.begin(&io, harness.providers());
}

void tearDown() {}

void test_status_reports_idle() {
    io.feed("QSTATUS\n");
    module.process();

    TEST_ASSERT_TRUE(io.outputContains("QRESP "));
    TEST_ASSERT_TRUE(io.outputContains("\"state\":\"idle\""));
    TEST_ASSERT_TRUE(io.outputContains("\"ok\":true"));
}

void test_invalid_start_args_returns_error() {
    io.feed("QSTART core nope\n");
    module.process();

    TEST_ASSERT_TRUE(io.outputContains("QERR "));
    TEST_ASSERT_TRUE(io.outputContains("invalid_start_args"));
    TEST_ASSERT_FALSE(module.isRunning());
}

void test_core_start_runs_finalizes_and_clears_mode() {
    harness.now = 100;
    io.feed("QSTART core 1 proxy\n");
    module.process();

    TEST_ASSERT_TRUE(module.isRunning());
    TEST_ASSERT_TRUE(io.outputContains("started"));
    TEST_ASSERT_EQUAL(1, harness.applyModeCalls);
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(QualificationSerialModule::Mode::Proxy), harness.lastMode);
    TEST_ASSERT_EQUAL(1, harness.startPerfCalls);
    TEST_ASSERT_EQUAL(1, harness.pauseTrueCalls);
    TEST_ASSERT_EQUAL(1, harness.pauseFalseCalls);
    TEST_ASSERT_EQUAL(1, harness.enqueueSnapshotCalls);

    io.clearOutput();
    harness.now = 1100;
    module.process();
    TEST_ASSERT_TRUE(module.isRunning());
    TEST_ASSERT_TRUE(io.outputContains("duration_elapsed"));
    TEST_ASSERT_EQUAL(2, harness.enqueueSnapshotCalls);
    TEST_ASSERT_EQUAL(2, harness.pauseTrueCalls);

    io.clearOutput();
    module.process();
    TEST_ASSERT_TRUE(module.isDone());
    TEST_ASSERT_TRUE(io.outputContains("QEVENT "));
    TEST_ASSERT_TRUE(io.outputContains("done"));
    TEST_ASSERT_EQUAL(1, harness.clearModeCalls);
}

void test_display_start_and_abort_cancel_preview() {
    io.feed("QSTART display 2 v1\n");
    module.process();

    TEST_ASSERT_TRUE(module.isRunning());
    TEST_ASSERT_EQUAL(1, harness.startPreviewCalls);
    TEST_ASSERT_EQUAL(2000UL, harness.previewDurationMs);
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(QualificationSerialModule::Mode::V1Only), harness.lastMode);

    io.clearOutput();
    io.feed("QABORT\n");
    module.process();

    TEST_ASSERT_FALSE(module.isDone());
    TEST_ASSERT_FALSE(module.isRunning());
    TEST_ASSERT_TRUE(io.outputContains("aborted"));
    TEST_ASSERT_EQUAL(1, harness.cancelPreviewCalls);
    TEST_ASSERT_EQUAL(1, harness.clearModeCalls);
}

void test_getcsv_rejects_when_storage_unavailable() {
    harness.storageReady = false;
    harness.sdCard = false;

    io.feed("QGETCSV /perf/qualification.csv\n");
    module.process();

    TEST_ASSERT_TRUE(io.outputContains("QERR "));
    TEST_ASSERT_TRUE_MESSAGE(io.outputContains("sd_unavailable"), io.output().c_str());
    TEST_ASSERT_TRUE_MESSAGE(io.outputContains("\"message\":\"sd_unavailable\""), io.output().c_str());
    TEST_ASSERT_TRUE_MESSAGE(io.outputContains("\"suite\":\"core\""), io.output().c_str());
    TEST_ASSERT_TRUE_MESSAGE(io.outputContains("\"durationMs\":0"), io.output().c_str());
}

void test_bsc08_snapshot_echoes_exact_nonce_and_only_privacy_safe_scalars() {
    harness.proxySnapshot.epoch.currentEpoch = 42;
    harness.proxySnapshot.epoch.admittedEpoch = 42;
    harness.proxySnapshot.epoch.v1ToProxyCallbackEntries = 3;
    harness.proxySnapshot.epoch.proxyToV1CallbackEntries = 4;
    harness.proxySnapshot.epoch.v1ToProxyAdmissions = 2;
    harness.proxySnapshot.epoch.proxyToV1Admissions = 1;
    harness.proxySnapshot.epoch.staleV1ToProxyRejections = 1;
    harness.proxySnapshot.epoch.staleProxyToV1Rejections = 2;
    harness.proxySnapshot.epoch.allocationCount = 2;
    harness.proxySnapshot.epoch.disableCount = 1;
    harness.proxySnapshot.epoch.releaseCount = 1;
    harness.proxySnapshot.epoch.reenableCount = 1;
    harness.proxySnapshot.epoch.activeCallbackObserved = true;
    harness.proxySnapshot.epoch.releaseOpportunityObserved = true;
    harness.proxySnapshot.proxyQueueCapacity = 8;
    harness.proxySnapshot.phoneQueueCapacity = 16;
    harness.proxySnapshot.freeInternalBytes = 123456;
    harness.proxySnapshot.largestInternalBlockBytes = 65432;

    io.feed("QBSC08 0123456789abcdef0123456789abcdef\n");
    module.process();

    TEST_ASSERT_TRUE(io.outputContains("QBSC08 {\"schema\":1"));
    TEST_ASSERT_TRUE(io.outputContains("\"nonce\":\"0123456789abcdef0123456789abcdef\""));
    TEST_ASSERT_TRUE(io.outputContains("\"status\":\"ready\""));
    TEST_ASSERT_TRUE(io.outputContains("\"callbackEntries\":[3,4]"));
    TEST_ASSERT_TRUE(io.outputContains("\"lifecycle\":[2,1,1,1]"));
    TEST_ASSERT_TRUE(io.outputContains("\"heap\":[123456,65432]"));
    TEST_ASSERT_FALSE(io.outputContains("address"));
    TEST_ASSERT_FALSE(io.outputContains("payload"));
    TEST_ASSERT_FALSE(io.outputContains("handle"));
    TEST_ASSERT_FALSE(io.outputContains("path"));
    TEST_ASSERT_FALSE(io.outputContains("uuid"));
}

void test_bsc08_snapshot_returns_busy_without_partial_state() {
    harness.proxySnapshotReady = false;
    io.feed("QBSC08 fedcba9876543210fedcba9876543210\n");
    module.process();

    TEST_ASSERT_TRUE(io.outputContains("\"status\":\"busy\""));
    TEST_ASSERT_TRUE(io.outputContains("fedcba9876543210fedcba9876543210"));
    TEST_ASSERT_FALSE(io.outputContains("\"epoch\""));
}

void test_bsc08_snapshot_rejects_missing_uppercase_or_extended_nonce() {
    io.feed("QBSC08\nQBSC08 0123456789ABCDEF0123456789ABCDEF\n");
    io.feed("QBSC08 0123456789abcdef0123456789abcdef extra\n");
    module.process();

    const std::string output = io.output();
    size_t errors = 0;
    size_t offset = 0;
    while ((offset = output.find("QERR ", offset)) != std::string::npos) {
        ++errors;
        offset += 1;
    }
    TEST_ASSERT_EQUAL_UINT32(3, errors);
    TEST_ASSERT_FALSE(io.outputContains("QBSC08 {"));
}

void runAllTests() {
    RUN_TEST(test_status_reports_idle);
    RUN_TEST(test_invalid_start_args_returns_error);
    RUN_TEST(test_core_start_runs_finalizes_and_clears_mode);
    RUN_TEST(test_display_start_and_abort_cancel_preview);
    RUN_TEST(test_getcsv_rejects_when_storage_unavailable);
    RUN_TEST(test_bsc08_snapshot_echoes_exact_nonce_and_only_privacy_safe_scalars);
    RUN_TEST(test_bsc08_snapshot_returns_busy_without_partial_state);
    RUN_TEST(test_bsc08_snapshot_rejects_missing_uppercase_or_extended_nonce);
}

#ifdef ARDUINO
void setup() {
    delay(2000);
    UNITY_BEGIN();
    runAllTests();
    UNITY_END();
}
void loop() {}
#else
int main(int argc, char** argv) {
    UNITY_BEGIN();
    runAllTests();
    return UNITY_END();
}
#endif
