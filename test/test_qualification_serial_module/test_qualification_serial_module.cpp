#include <unity.h>

#include <ArduinoJson.h>

#include "../mocks/Arduino.h"

#include <cstdint>
#include <string>
#include <type_traits>

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

class Stream {
  public:
    virtual ~Stream() = default;

    virtual int available() = 0;
    virtual int read() = 0;
    virtual int availableForWrite() = 0;
    virtual size_t write(uint8_t value) = 0;

    size_t print(const char* value) {
        if (!value) {
            return 0;
        }
        size_t written = 0;
        while (*value != '\0') {
            written += write(static_cast<uint8_t>(*value++));
        }
        return written;
    }

    size_t print(char value) { return write(static_cast<uint8_t>(value)); }

    template <typename T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0> size_t print(T value) {
        const std::string text = std::to_string(value);
        return print(text.c_str());
    }

    size_t println(const char* value = "") {
        const size_t written = print(value);
        return written + print('\n');
    }
};

#include "../../src/modules/qualification/qualification_serial_module.cpp"

namespace {

class CaptureStream final : public Stream {
  public:
    explicit CaptureStream(const char* input) : input_(input ? input : "") {}

    int available() override { return inputOffset_ < input_.size() ? 1 : 0; }

    int read() override {
        if (inputOffset_ >= input_.size()) {
            return -1;
        }
        return static_cast<unsigned char>(input_[inputOffset_++]);
    }

    int availableForWrite() override { return 1024; }

    size_t write(uint8_t value) override {
        output_.push_back(static_cast<char>(value));
        return 1;
    }

    const std::string& output() const { return output_; }

  private:
    std::string input_;
    size_t inputOffset_ = 0;
    std::string output_;
};

struct DisplaySettingsProbe {
    uint8_t brightness = 173;
    uint16_t mutedColorRgb565 = 0x39E7;
    uint32_t brightnessReads = 0;
    uint32_t mutedColorReads = 0;
};

uint8_t provideBrightness(void* ctx) {
    auto& probe = *static_cast<DisplaySettingsProbe*>(ctx);
    probe.brightnessReads++;
    return probe.brightness;
}

uint16_t provideMutedColor(void* ctx) {
    auto& probe = *static_cast<DisplaySettingsProbe*>(ctx);
    probe.mutedColorReads++;
    return probe.mutedColorRgb565;
}

struct ClockProbe {
    uint64_t nextMicros = 0x123;
    uint32_t reads = 0;
    uint64_t segment = 0x1020304050607080ULL;
};

uint64_t provideMicros(void* ctx) {
    auto& probe = *static_cast<ClockProbe*>(ctx);
    const uint64_t value = probe.nextMicros;
    probe.nextMicros += 0x333;
    probe.reads++;
    return value;
}

uint64_t provideClockSegment(void* ctx) {
    return static_cast<ClockProbe*>(ctx)->segment;
}

struct EvidenceStartProbe {
    bool capturePaused = false;
    uint32_t perfStarts = 0;
};

bool provideTrue(void*) {
    return true;
}
const char* providePerfPath(void*) {
    return "/perf/test.csv";
}
void startPerf(void* ctx) {
    static_cast<EvidenceStartProbe*>(ctx)->perfStarts++;
}
bool enqueueSnapshot(void*) {
    return true;
}
void setCapturePaused(bool paused, void* ctx) {
    static_cast<EvidenceStartProbe*>(ctx)->capturePaused = paused;
}
bool rejectEvidenceStart(uint32_t, uint32_t, void*) {
    return false;
}
void endEvidence(uint32_t, uint32_t, void*) {}

} // namespace

void setUp() {}
void tearDown() {}

void test_qstatus_emits_provider_display_settings_as_json() {
    DisplaySettingsProbe probe;
    QualificationSerialModule::Providers providers;
    providers.displayBrightness = provideBrightness;
    providers.displayMutedColorRgb565 = provideMutedColor;
    providers.ctx = &probe;

    CaptureStream stream("QSTATUS\n");
    QualificationSerialModule module;
    module.begin(&stream, providers);
    module.process();

    constexpr char kResponsePrefix[] = "QRESP ";
    const std::string& response = stream.output();
    TEST_ASSERT_TRUE(response.rfind(kResponsePrefix, 0) == 0);
    TEST_ASSERT_FALSE(response.empty());
    TEST_ASSERT_EQUAL_CHAR('\n', response.back());

    const std::string payload =
        response.substr(sizeof(kResponsePrefix) - 1, response.size() - (sizeof(kResponsePrefix) - 1) - 1);
    JsonDocument status;
    const DeserializationError error = deserializeJson(status, payload);
    TEST_ASSERT_FALSE_MESSAGE(error, error.c_str());

    TEST_ASSERT_EQUAL_UINT8(probe.brightness, status["displaySettings"]["brightness"].as<uint8_t>());
    TEST_ASSERT_EQUAL_UINT16(probe.mutedColorRgb565, status["displaySettings"]["colorMutedRgb565"].as<uint16_t>());
    TEST_ASSERT_EQUAL_UINT32(1, probe.brightnessReads);
    TEST_ASSERT_EQUAL_UINT32(1, probe.mutedColorReads);
}

void test_qsync_captures_parse_and_prewrite_timestamps_in_fixed_width_reply() {
    ClockProbe probe;
    QualificationSerialModule::Providers providers;
    providers.nowUs = provideMicros;
    providers.clockSegment = provideClockSegment;
    providers.ctx = &probe;

    CaptureStream stream("QSYNC 0123456789abcdef\n");
    QualificationSerialModule module;
    module.begin(&stream, providers);
    module.process();

    TEST_ASSERT_EQUAL_STRING("QSYNC 0123456789ABCDEF 1020304050607080 0000000000000123 0000000000000456\n",
                             stream.output().c_str());
    TEST_ASSERT_EQUAL_UINT32(2, probe.reads);
}

void test_qsync_rejects_non_fixed_nonce() {
    ClockProbe probe;
    QualificationSerialModule::Providers providers;
    providers.nowUs = provideMicros;
    providers.clockSegment = provideClockSegment;
    providers.ctx = &probe;

    CaptureStream stream("QSYNC 1234\n");
    QualificationSerialModule module;
    module.begin(&stream, providers);
    module.process();

    TEST_ASSERT_TRUE(stream.output().rfind("QERR ", 0) == 0);
    TEST_ASSERT_TRUE(stream.output().find("\"error\":\"invalid_sync_nonce\"") != std::string::npos);
    TEST_ASSERT_EQUAL_UINT32(2, probe.reads);
}

void test_qstart_treats_causal_evidence_as_best_effort() {
    EvidenceStartProbe probe;
    QualificationSerialModule::Providers providers;
    providers.isPerfEnabled = provideTrue;
    providers.perfCsvPath = providePerfPath;
    providers.startPerfSession = startPerf;
    providers.enqueueSnapshotNow = enqueueSnapshot;
    providers.tryDrainPerf = provideTrue;
    providers.tryDrainEvidence = provideTrue;
    providers.beginEvidenceSession = rejectEvidenceStart;
    providers.endEvidenceSession = endEvidence;
    providers.setSdCapturePaused = setCapturePaused;
    providers.ctx = &probe;

    CaptureStream stream("QSTART core 1\n");
    QualificationSerialModule module;
    module.begin(&stream, providers);
    module.process();

    TEST_ASSERT_TRUE(stream.output().rfind("QRESP ", 0) == 0);
    TEST_ASSERT_FALSE(probe.capturePaused);
    TEST_ASSERT_EQUAL_UINT32(1, probe.perfStarts);
    TEST_ASSERT_TRUE(module.isRunning());

    EvidenceStartProbe unavailableProbe;
    providers.beginEvidenceSession = nullptr;
    providers.endEvidenceSession = nullptr;
    providers.tryDrainEvidence = nullptr;
    providers.ctx = &unavailableProbe;
    CaptureStream unavailableStream("QSTART core 1\n");
    QualificationSerialModule unavailableModule;
    unavailableModule.begin(&unavailableStream, providers);
    unavailableModule.process();

    TEST_ASSERT_TRUE(unavailableStream.output().rfind("QRESP ", 0) == 0);
    TEST_ASSERT_FALSE(unavailableProbe.capturePaused);
    TEST_ASSERT_EQUAL_UINT32(1, unavailableProbe.perfStarts);
    TEST_ASSERT_TRUE(unavailableModule.isRunning());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_qstatus_emits_provider_display_settings_as_json);
    RUN_TEST(test_qsync_captures_parse_and_prewrite_timestamps_in_fixed_width_reply);
    RUN_TEST(test_qsync_rejects_non_fixed_nonce);
    RUN_TEST(test_qstart_treats_causal_evidence_as_best_effort);
    return UNITY_END();
}
