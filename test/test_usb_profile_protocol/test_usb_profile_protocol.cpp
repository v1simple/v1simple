#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "../../src/usb_profile_protocol.cpp"

namespace {

struct Transport final : UsbProfileTransport {
    std::string input;
    size_t cursor = 0;
    std::string output;
    std::vector<std::string> attempts;
    int space = 256;  // Actual HWCDC default TX ring capacity.
    size_t nextWriteLimit = std::numeric_limits<size_t>::max();
    size_t reads = 0;

    int available() override { return static_cast<int>(input.size() - cursor); }
    int read() override {
        if (!available()) return -1;
        ++reads;
        return static_cast<unsigned char>(input[cursor++]);
    }
    int availableForWrite() override { return space; }
    size_t write(const uint8_t* bytes, size_t length) override {
        attempts.emplace_back(reinterpret_cast<const char*>(bytes), length);
        const size_t accepted = std::min(length, nextWriteLimit);
        output.append(reinterpret_cast<const char*>(bytes), accepted);
        nextWriteLimit = std::numeric_limits<size_t>::max();
        return accepted;
    }
};

struct Backend final : UsbProfileBackend {
    UsbProfileStatus state;
    mutable size_t statusCalls = 0;
    size_t allocations = 0;
    size_t allocatedBytes = 0;
    size_t releases = 0;
    size_t activities = 0;
    size_t backups = 0;
    size_t applies = 0;
    size_t entries = 0;
    size_t exits = 0;
    bool allocationFails = false;
    bool applySucceeds = true;
    std::string source = "retained profile bundle";
    std::string applied;

    Backend() { state.busy = false; }
    UsbProfileStatus status() const override { ++statusCalls; return state; }
    uint8_t* allocate(size_t bytes) override {
        ++allocations;
        allocatedBytes = bytes;
        return allocationFails ? nullptr : static_cast<uint8_t*>(std::malloc(bytes));
    }
    void release(uint8_t* bytes) override { ++releases; std::free(bytes); }
    bool backup(uint8_t*& bytes, size_t& length, char*, size_t) override {
        ++backups;
        length = source.size();
        bytes = allocate(length);
        if (!bytes) return false;
        std::memcpy(bytes, source.data(), length);
        return true;
    }
    bool apply(const uint8_t* bytes, size_t length, bool& pending, int& profiles,
               char* error, size_t errorSize) override {
        ++applies;
        applied.assign(reinterpret_cast<const char*>(bytes), length);
        pending = true;
        profiles = 3;
        if (!applySucceeds) std::snprintf(error, errorSize, "storage failure");
        return applySucceeds;
    }
    void enterMaintenance() override { ++entries; state.maintenance = true; }
    void restartNormal() override { ++exits; state.maintenance = false; }
    void activity(uint32_t) override { ++activities; }
};

std::string hex(const std::string& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned char byte : bytes) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

std::string checksum(const std::string& bytes) {
    char result[9];
    std::snprintf(result, sizeof(result), "%08lx", static_cast<unsigned long>(
        UsbProfileProtocol::crc32(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())));
    return result;
}

struct Fixture {
    Transport transport;
    Backend backend;
    UsbProfileProtocol protocol{transport, backend};
    uint32_t now = 1;
    uint32_t id = 1;

    void tick(uint32_t elapsed = 1) {
        now += elapsed;
        const size_t before = transport.reads;
        const size_t writes = transport.attempts.size();
        protocol.tick(now);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(UsbProfileProtocol::RxBytesPerTick, transport.reads - before);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(1, transport.attempts.size() - writes);
    }
    void append(uint32_t requestId, const std::string& command) {
        transport.input += "@V1USB1 " + std::to_string(requestId) + " " + command + "\n";
    }
    void drainInput() {
        for (unsigned count = 0; transport.available() && count < 4096; ++count) tick();
        TEST_ASSERT_EQUAL_INT(0, transport.available());
    }
    std::string request(const std::string& command, uint32_t requestId = 0) {
        const size_t start = transport.output.size();
        append(requestId ? requestId : id++, command);
        drainInput();
        return transport.output.substr(start);
    }
    void begin(const std::string& bytes, const std::string& crc = "") {
        const auto response = request("begin " + std::to_string(bytes.size()) + " " +
                                      (crc.empty() ? checksum(bytes) : crc));
        TEST_ASSERT_NOT_NULL(std::strstr(response.c_str(), "\"offset\":0"));
    }
    void stage(const std::string& bytes) {
        begin(bytes);
        for (size_t offset = 0; offset < bytes.size(); offset += UsbProfileProtocol::ChunkBytes) {
            request("write " + std::to_string(offset) + " " + hex(bytes.substr(offset, UsbProfileProtocol::ChunkBytes)));
            TEST_ASSERT_EQUAL_UINT32(0, backend.applies);
        }
    }
};

void contains(const std::string& text, const char* expected) {
    TEST_ASSERT_NOT_NULL_MESSAGE(std::strstr(text.c_str(), expected), text.c_str());
}

void test_idle_is_silent_and_does_not_allocate_or_call_backend() {
    Fixture f;
    for (unsigned i = 0; i < 100; ++i) f.tick(1000);
    TEST_ASSERT_TRUE(f.transport.output.empty());
    TEST_ASSERT_EQUAL_UINT32(0, f.transport.reads);
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.allocations);
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.statusCalls);
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.activities);
}

void test_rx_budget_and_one_dispatch_per_tick() {
    Fixture f;
    f.transport.input = std::string(500, 'x') + "\n";
    f.tick();
    TEST_ASSERT_EQUAL_UINT32(64, f.transport.reads);
    f.drainInput();
    TEST_ASSERT_TRUE(f.transport.output.empty());
    f.append(1, "status");
    f.append(2, "status");
    f.tick();
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.activities);
    TEST_ASSERT_GREATER_THAN_INT(0, f.transport.available());
    f.tick();
    TEST_ASSERT_EQUAL_UINT32(2, f.backend.activities);
    TEST_ASSERT_EQUAL_INT(0, f.transport.available());
}

void test_noise_oversize_invalid_ids_and_control_bytes_are_quiet() {
    Fixture f;
    f.transport.input = "human text\n@V1USB1 0 status\n@V1USB1 4294967296 status\n";
    f.transport.input += "@V1USB1 1 sta\x01tus\n";
    f.transport.input += "@V1USB1 1 " + std::string(300, 's') + "\n";
    f.drainInput();
    TEST_ASSERT_TRUE(f.transport.output.empty());
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.allocations);
    contains(f.request("status"), "\"mode\":\"normal\"");
}

void test_timed_out_partial_frame_discards_suffix_until_newline() {
    Fixture f;
    f.transport.input = "@V1USB1 1 maint";
    f.drainInput();
    f.tick(UsbProfileProtocol::FrameIdleMs);
    f.transport.input += "enance\n";
    f.drainInput();
    TEST_ASSERT_TRUE(f.transport.output.empty());
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.entries);
    contains(f.request("status"), "\"ok\":true");
}

void test_normal_denies_every_bulk_operation_before_backend_work() {
    Fixture f;
    for (const char* command : {"backup", "begin 4 12345678", "write 0 61626364", "read 0", "commit", "abort"}) {
        contains(f.request(command), "maintenance_required");
    }
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.allocations);
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.backups);
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.applies);
}

void test_busy_maintenance_request_is_not_queued_for_later() {
    Fixture f;
    f.backend.state.busy = true;
    contains(f.request("maintenance", 10), "\"error\":\"busy\"");
    f.backend.state.busy = false;
    f.tick();
    contains(f.request("maintenance", 10), "\"error\":\"busy\"");
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.entries);
    contains(f.request("maintenance", 11), "restarting");
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.entries);
    f.request("maintenance", 11);
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.entries);
    f.request("normal", 12);
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.exits);
}

void test_busy_at_transmit_cancels_action_including_cached_retry() {
    Fixture f;
    f.transport.space = 0;
    f.request("maintenance", 10);
    f.backend.state.busy = true;
    f.transport.space = 256;
    f.tick();
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.entries);
    f.backend.state.busy = false;
    f.request("maintenance", 10);
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.entries);
}

void test_complete_backup_chunks_preserve_binary_bytes_and_fit_tx_ring() {
    Fixture f;
    f.backend.state.maintenance = true;
    f.backend.source.clear();
    for (unsigned i = 0; i < 256; ++i) f.backend.source += static_cast<char>(i);
    const std::string response = f.request("backup");
    contains(response, "\"bytes\":256");
    contains(response, checksum(f.backend.source).c_str());
    for (unsigned offset = 0; offset < 256; offset += 64) {
        contains(f.request("read " + std::to_string(offset)), hex(f.backend.source.substr(offset, 64)).c_str());
    }
    contains(f.request("read 256"), "bad_offset");
    for (const auto& frame : f.transport.attempts) {
        TEST_ASSERT_LESS_THAN_UINT32(256, frame.size());
        TEST_ASSERT_EQUAL_CHAR('\n', frame.front());
        TEST_ASSERT_EQUAL_CHAR('\n', frame.back());
        TEST_ASSERT_EQUAL_UINT32(1, frame.find("@V1USB1 "));
    }
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.backups);
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.applies);
}

void test_crc_matches_independent_standard_vector() {
    TEST_ASSERT_EQUAL_HEX32(0xcbf43926u, UsbProfileProtocol::crc32(
        reinterpret_cast<const uint8_t*>("123456789"), 9));
    TEST_ASSERT_EQUAL_HEX32(0u, UsbProfileProtocol::crc32(nullptr, 0));
}

void test_incomplete_wrong_offset_and_bad_hex_never_apply_or_advance() {
    Fixture f;
    f.backend.state.maintenance = true;
    f.begin("abcdef");
    contains(f.request("write 1 6162"), "bad_chunk");
    contains(f.request("write 0 6162xz"), "bad_chunk");
    contains(f.request("write 0 616"), "bad_chunk");
    contains(f.request("write 0 61626364656667"), "bad_chunk");
    contains(f.request("commit"), "incomplete_document");
    contains(f.request("write 0 616263"), "\"offset\":3");
    contains(f.request("commit"), "incomplete_document");
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.applies);
    contains(f.request("write 3 646566"), "\"offset\":6");
    contains(f.request("commit"), "\"stored\":true");
    TEST_ASSERT_EQUAL_STRING("abcdef", f.backend.applied.c_str());
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.applies);
}

void test_crc_failure_releases_document_and_never_applies() {
    Fixture f;
    f.backend.state.maintenance = true;
    f.begin("abcdef", "00000000");
    f.request("write 0 616263646566");
    contains(f.request("commit"), "checksum_mismatch");
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.applies);
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.releases);
    contains(f.request("commit"), "incomplete_document");
}

void test_only_complete_valid_commit_dispatches_and_duplicate_never_reapplies() {
    Fixture f;
    f.backend.state.maintenance = true;
    std::string bytes;
    for (unsigned i = 0; i < 130; ++i) bytes += static_cast<char>(i);
    f.stage(bytes);
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.applies);
    const std::string committed = f.request("commit", 100);
    contains(committed, "\"stored\":true");
    contains(committed, "\"backup_pending\":true");
    TEST_ASSERT_EQUAL_MEMORY(bytes.data(), f.backend.applied.data(), bytes.size());
    TEST_ASSERT_EQUAL_UINT32(bytes.size(), f.backend.applied.size());
    const std::string repeated = f.request("commit", 100);
    TEST_ASSERT_EQUAL_STRING(committed.c_str(), repeated.c_str());
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.applies);
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.releases);
    contains(f.request("commit", 101), "incomplete_document");
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.applies);
}

void test_busy_commit_cannot_mutate_staged_document() {
    Fixture f;
    f.backend.state.maintenance = true;
    f.stage("abcdef");
    f.backend.state.busy = true;
    contains(f.request("commit", 100), "\"error\":\"busy\"");
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.applies);
    f.backend.state.busy = false;
    contains(f.request("commit", 100), "\"error\":\"busy\"");
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.applies);
    contains(f.request("commit", 101), "\"stored\":true");
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.applies);
}

void test_failed_apply_is_never_retried_as_a_mutation() {
    Fixture f;
    f.backend.state.maintenance = true;
    f.backend.applySucceeds = false;
    f.stage("abcdef");
    const std::string failed = f.request("commit", 100);
    contains(failed, "apply_failed");
    TEST_ASSERT_EQUAL_STRING(failed.c_str(), f.request("commit", 100).c_str());
    contains(f.request("commit", 101), "incomplete_document");
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.applies);
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.releases);
}

void test_request_id_conflict_preserves_original_committed_reply() {
    Fixture f;
    f.backend.state.maintenance = true;
    f.stage("abcdef");
    const std::string committed = f.request("commit", 100);
    contains(f.request("status", 100), "request_id_conflict");
    TEST_ASSERT_EQUAL_STRING(committed.c_str(), f.request("commit", 100).c_str());
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.applies);
}

void test_abort_expiry_and_destruction_release_staged_buffers() {
    Fixture f;
    f.backend.state.maintenance = true;
    f.begin("abcdef");
    f.request("abort");
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.releases);
    f.begin("abcdef");
    f.tick(UsbProfileProtocol::SessionIdleMs);
    TEST_ASSERT_EQUAL_UINT32(2, f.backend.releases);
    contains(f.request("commit"), "incomplete_document");
    f.request("backup");
    f.tick(UsbProfileProtocol::SessionIdleMs);
    TEST_ASSERT_EQUAL_UINT32(3, f.backend.releases);
    contains(f.request("read 0"), "bad_offset");
    Backend backend;
    Transport transport;
    backend.state.maintenance = true;
    {
        UsbProfileProtocol protocol(transport, backend);
        transport.input = "@V1USB1 1 begin 6 4b8e39ef\n";
        protocol.tick(1);
        TEST_ASSERT_EQUAL_UINT32(1, backend.allocations);
    }
    TEST_ASSERT_EQUAL_UINT32(1, backend.releases);
}

void test_invalid_sizes_and_allocation_failure_do_not_mutate() {
    Fixture f;
    f.backend.state.maintenance = true;
    contains(f.request("begin 0 00000000"), "document_too_large");
    contains(f.request("begin 131073 00000000"), "document_too_large");
    contains(f.request("begin 4294967296 00000000"), "bad_request");
    contains(f.request("begin 6 zzzzzzzz"), "bad_request");
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.allocations);
    f.backend.allocationFails = true;
    contains(f.request("begin 6 4b8e39ef"), "out_of_memory");
    contains(f.request("commit"), "incomplete_document");
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.applies);
}

void test_maximum_document_round_trip_includes_last_byte_without_overrun() {
    Fixture f;
    f.backend.state.maintenance = true;
    std::string bytes(UsbProfileProtocol::MaxDocumentBytes, '\0');
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(i % 251);
    f.stage(bytes);
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.allocations);
    TEST_ASSERT_EQUAL_UINT32(UsbProfileProtocol::MaxDocumentBytes + 1, f.backend.allocatedBytes);
    contains(f.request("commit"), "\"stored\":true");
    TEST_ASSERT_EQUAL_UINT32(bytes.size(), f.backend.applied.size());
    TEST_ASSERT_EQUAL_MEMORY(bytes.data(), f.backend.applied.data(), bytes.size());
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.applies);
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.releases);
}

void test_short_tx_resynchronizes_whole_frame_without_reapplying() {
    Fixture f;
    f.backend.state.maintenance = true;
    f.stage("abcdef");
    const size_t start = f.transport.output.size();
    f.transport.nextWriteLimit = 15;
    f.request("commit", 100);
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.applies);
    TEST_ASSERT_EQUAL_UINT32(start + 15, f.transport.output.size());
    const std::string complete = f.transport.attempts.back();
    f.tick();
    TEST_ASSERT_EQUAL_STRING(complete.c_str(), f.transport.output.substr(start + 15).c_str());
    TEST_ASSERT_EQUAL_CHAR('\n', f.transport.output[start + 15]);
    TEST_ASSERT_EQUAL_STRING(complete.c_str(), f.request("commit", 100).c_str());
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.applies);
}

void test_blocked_tx_is_bounded_and_expired_status_can_be_retried() {
    Fixture f;
    f.transport.space = 0;
    f.request("status", 10);
    TEST_ASSERT_TRUE(f.transport.output.empty());
    TEST_ASSERT_TRUE(f.transport.attempts.empty());
    f.tick(UsbProfileProtocol::FrameIdleMs);
    f.transport.space = 256;
    contains(f.request("status", 10), "\"mode\":\"normal\"");
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.applies);
}

void test_unsent_expired_restart_is_not_executed_on_cached_retry() {
    Fixture f;
    f.transport.space = 0;
    f.request("maintenance", 10);
    f.tick(UsbProfileProtocol::FrameIdleMs);
    f.transport.space = 256;
    f.request("maintenance", 10);
    TEST_ASSERT_EQUAL_UINT32(0, f.backend.entries);
    f.request("maintenance", 11);
    TEST_ASSERT_EQUAL_UINT32(1, f.backend.entries);
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_idle_is_silent_and_does_not_allocate_or_call_backend);
    RUN_TEST(test_rx_budget_and_one_dispatch_per_tick);
    RUN_TEST(test_noise_oversize_invalid_ids_and_control_bytes_are_quiet);
    RUN_TEST(test_timed_out_partial_frame_discards_suffix_until_newline);
    RUN_TEST(test_normal_denies_every_bulk_operation_before_backend_work);
    RUN_TEST(test_busy_maintenance_request_is_not_queued_for_later);
    RUN_TEST(test_busy_at_transmit_cancels_action_including_cached_retry);
    RUN_TEST(test_complete_backup_chunks_preserve_binary_bytes_and_fit_tx_ring);
    RUN_TEST(test_crc_matches_independent_standard_vector);
    RUN_TEST(test_incomplete_wrong_offset_and_bad_hex_never_apply_or_advance);
    RUN_TEST(test_crc_failure_releases_document_and_never_applies);
    RUN_TEST(test_only_complete_valid_commit_dispatches_and_duplicate_never_reapplies);
    RUN_TEST(test_busy_commit_cannot_mutate_staged_document);
    RUN_TEST(test_failed_apply_is_never_retried_as_a_mutation);
    RUN_TEST(test_request_id_conflict_preserves_original_committed_reply);
    RUN_TEST(test_abort_expiry_and_destruction_release_staged_buffers);
    RUN_TEST(test_invalid_sizes_and_allocation_failure_do_not_mutate);
    RUN_TEST(test_maximum_document_round_trip_includes_last_byte_without_overrun);
    RUN_TEST(test_short_tx_resynchronizes_whole_frame_without_reapplying);
    RUN_TEST(test_blocked_tx_is_bounded_and_expired_status_can_be_retried);
    RUN_TEST(test_unsent_expired_restart_is_not_executed_on_cached_retry);
    return UNITY_END();
}
