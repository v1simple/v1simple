#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <Arduino.h>
#include "client_write_retry.h"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace {

class FakeClient {
public:
    explicit FakeClient(std::vector<size_t> scriptedWrites = {},
                        unsigned long advanceMsPerWrite = 0)
        : scriptedWrites_(std::move(scriptedWrites)),
          advanceMsPerWrite_(advanceMsPerWrite) {}

    size_t write(const uint8_t* data, size_t size) {
        ++attempts_;
        mockMillis += advanceMsPerWrite_;

        if (!scriptedWrites_.empty()) {
            const size_t scripted = scriptedWrites_.front();
            scriptedWrites_.erase(scriptedWrites_.begin());
            if (scripted == 0) {
                return 0;
            }
            const size_t accepted = std::min(scripted, size);
            written_.append(reinterpret_cast<const char*>(data), accepted);
            return accepted;
        }

        written_.append(reinterpret_cast<const char*>(data), size);
        return size;
    }

    int attempts() const { return attempts_; }
    const std::string& written() const { return written_; }

private:
    std::vector<size_t> scriptedWrites_;
    std::string written_;
    unsigned long advanceMsPerWrite_ = 0;
    int attempts_ = 0;
};

}  // namespace

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_write_client_bytes_retries_zero_writes() {
    FakeClient client({0, 0, 4});
    const uint8_t payload[] = {'T', 'E', 'S', 'T'};

    const client_write_retry::Config cfg{
        3,   // maxZeroWriteRetries
        500, // maxWriteWindowMs
        1,   // backoffStartMs
        8    // backoffMaxMs
    };

    const bool ok = client_write_retry::writeAll(client, payload, sizeof(payload), cfg);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(3, client.attempts());
    TEST_ASSERT_EQUAL_STRING_LEN("TEST", client.written().c_str(), 4);
}

void test_write_client_bytes_handles_partial_writes() {
    FakeClient client({2, 2, 1});
    const uint8_t payload[] = {'A', 'L', 'P', 'R', '!'};

    const bool ok = client_write_retry::writeAll(client, payload, sizeof(payload));

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(3, client.attempts());
    TEST_ASSERT_EQUAL_STRING_LEN("ALPR!", client.written().c_str(), 5);
}

void test_write_client_bytes_fails_after_retry_budget() {
    FakeClient client({0, 0, 0});
    const uint8_t payload[] = {'B', 'U', 'S'};

    const client_write_retry::Config cfg{
        2,   // maxZeroWriteRetries
        500, // maxWriteWindowMs
        1,   // backoffStartMs
        8    // backoffMaxMs
    };
    const bool ok = client_write_retry::writeAll(client, payload, sizeof(payload), cfg);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_INT(3, client.attempts());
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(client.written().size()));
}

void test_write_client_bytes_fails_after_write_window() {
    FakeClient client({0, 0, 0, 1}, 20);
    const uint8_t payload[] = {'S'};

    const client_write_retry::Config cfg{
        5,  // maxZeroWriteRetries
        30, // maxWriteWindowMs
        1,  // backoffStartMs
        8   // backoffMaxMs
    };
    const bool ok = client_write_retry::writeAll(client, payload, sizeof(payload), cfg);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_INT(2, client.attempts());
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(client.written().size()));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_write_client_bytes_retries_zero_writes);
    RUN_TEST(test_write_client_bytes_handles_partial_writes);
    RUN_TEST(test_write_client_bytes_fails_after_retry_budget);
    RUN_TEST(test_write_client_bytes_fails_after_write_window);
    return UNITY_END();
}
