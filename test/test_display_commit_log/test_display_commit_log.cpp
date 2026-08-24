#include <unity.h>

#include "../../src/modules/display/display_commit_log.h"
#include "../../src/modules/display/display_commit_log.cpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

// A commit that mirrors the shape of the retained replay defect: the code resolved a
// FRONT arrow with its flash bit set, and dispatched it through the partial-region
// path. Whether the panel followed is a question for camera evidence, not for the DUT.
V1DisplayCommitSnapshot makeCommit() {
    V1DisplayCommitSnapshot commit;
    commit.seq = 1;
    commit.millisTs = 84000;
    commit.path = V1DisplayCommitPath::Live;
    commit.dispatch = V1DisplayCommitDispatch::PartialRegion;
    commit.renderUs = 1234;
    commit.pushes = 1;
    commit.arrowsToShow = DIR_FRONT;
    commit.blinkPhase = 1;
    commit.arrowPainted = 1;
    commit.alertCount = 2;
    commit.regionX = 350;
    commit.regionY = 22;
    commit.regionW = 105;
    commit.regionH = 136;

    commit.state.activeBands = BAND_KA;
    commit.state.arrows = DIR_FRONT;
    commit.state.priorityArrow = DIR_FRONT;
    commit.state.signalBars = 6;
    commit.state.flashBits = DIR_FRONT;
    commit.state.bandFlashBits = 0;
    commit.state.muted = false;
    commit.state.softMuted = false;
    commit.state.displayOn = true;
    commit.state.systemStatus = true;
    commit.state.systemTest = false;
    commit.state.modeChar = 'A';
    commit.state.bogeyCounterChar = '2';
    commit.state.bogeyCounterDot = false;
    commit.state.bogeyCounterChar2 = '2';
    commit.state.mainVolume = 5;
    commit.state.muteVolume = 0;
    commit.state.hasJunkAlert = false;
    commit.state.hasPhotoAlert = false;
    commit.state.hasKuAlert = false;
    commit.priority = AlertData::create(BAND_KA, DIR_FRONT, 6, 2, 34700, true, true);
    commit.priority.v1Index = 3;
    commit.priority.frontRawStrength = 0xB2;
    commit.priority.rearRawStrength = 0x82;
    commit.alertTableDigest = 0x89ABCDEF;
    return commit;
}

std::string readProjectFile(const char* relativePath) {
    const std::filesystem::path path = std::filesystem::path(PROJECT_DIR) / relativePath;
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}
} // namespace

void setUp() {}
void tearDown() {}

void test_commit_log_records_resolved_state_and_dispatch() {
    V1DisplayCommitLog log;
    log.setBootId(62, 0x180a49da);
    log.begin(true);
    log.beginQualificationSession(0x0BADF00D);

    log.record(makeCommit());

    TEST_ASSERT_EQUAL_STRING("/display_commits/display_commits_62-180a49da.csv", log.csvPath());
    TEST_ASSERT_EQUAL_UINT32(1, log.testCommitCount());
    const char* legacyPrefix =
        "1,84000,LIVE,PARTIAL,1234,1,1,1,1,2,350,22,105,136,2,1,1,6,1,0,0,0,1,1,0,A,2,0,2,5,0,0,0,0,0,";
    TEST_ASSERT_EQUAL_STRING_LEN(legacyPrefix, log.testGetLastLine(), std::strlen(legacyPrefix));
    TEST_ASSERT_NOT_NULL(std::strstr(log.testGetLastLine(), ",0BADF00D,89ABCDEF,"));
    TEST_ASSERT_NOT_NULL(std::strstr(log.testGetLastLine(), ",1,3,2,34700,1,178,130,6,2,1,0,0,0,0,"));
    TEST_ASSERT_NOT_NULL(std::strstr(log.testGetLastLine(), ",0,0,0\n"));
}

void test_commit_log_keeps_flash_bits_and_dispatch_independent() {
    // The whole point of the record: the flash bit the code resolved and the transfer
    // it chose are separate facts. Collapsing them would hide exactly the case where a
    // flashing frame is dispatched through a path the panel does not latch.
    V1DisplayCommitLog log;
    log.begin(true);

    V1DisplayCommitSnapshot commit = makeCommit();
    commit.dispatch = V1DisplayCommitDispatch::FullFlush;
    log.record(commit);
    TEST_ASSERT_NOT_NULL(std::strstr(log.testGetLastLine(), ",FULL,"));

    commit.seq = 2;
    commit.dispatch = V1DisplayCommitDispatch::PartialRegion;
    commit.state.flashBits = 0;
    log.record(commit);
    TEST_ASSERT_NOT_NULL(std::strstr(log.testGetLastLine(), ",PARTIAL,"));
    // flash_bits is column 19; with no flash the field must read 0 while the rest of
    // the resolved state is unchanged.
    TEST_ASSERT_NOT_NULL(std::strstr(log.testGetLastLine(), ",350,22,105,136,2,1,1,6,0,0,"));
}

void test_commit_log_records_every_dispatch_kind_including_no_push() {
    V1DisplayCommitLog log;
    log.begin(true);

    V1DisplayCommitSnapshot commit = makeCommit();
    commit.dispatch = V1DisplayCommitDispatch::None;
    commit.pushes = 0;
    log.record(commit);
    TEST_ASSERT_NOT_NULL(std::strstr(log.testGetLastLine(), ",NONE,"));

    commit.dispatch = V1DisplayCommitDispatch::MultiRect;
    commit.pushes = 3;
    log.record(commit);
    TEST_ASSERT_NOT_NULL(std::strstr(log.testGetLastLine(), ",MULTIRECT,"));
    TEST_ASSERT_EQUAL_UINT32(2, log.testCommitCount());
}

void test_commit_log_reports_its_own_losses_inside_the_record() {
    // A dropped commit must never look like a commit that did not happen. The count
    // rides along in the next record so the host can see the gap.
    V1DisplayCommitLog log;
    log.begin(true);
    log.testForceDrop();
    log.testForceDrop();

    V1DisplayCommitSnapshot commit = makeCommit();
    commit.droppedSnapshots = log.testDroppedCount();
    log.record(commit);

    TEST_ASSERT_EQUAL_UINT32(2, log.testDroppedCount());
    const std::string line = log.testGetLastLine();
    TEST_ASSERT_TRUE(line.size() > 2);
    TEST_ASSERT_NOT_NULL(std::strstr(line.c_str(), ",0,2,0,0,"));
}

void test_commit_log_is_inert_without_storage() {
    V1DisplayCommitLog log;
    log.begin(false);
    log.record(makeCommit());

    TEST_ASSERT_FALSE(log.isEnabled());
    TEST_ASSERT_FALSE(log.beginQualificationSession(0x12345678));
    TEST_ASSERT_EQUAL_UINT32(0, log.testCommitCount());
}

void test_commit_log_hands_out_monotonic_sequence_ids() {
    V1DisplayCommitLog log;
    log.begin(true);
    TEST_ASSERT_EQUAL_UINT32(1, log.nextSeq());
    TEST_ASSERT_EQUAL_UINT32(2, log.nextSeq());
    TEST_ASSERT_EQUAL_UINT32(3, log.nextSeq());
    // begin() restarts the run, so sequence ids restart with the file.
    log.begin(true);
    TEST_ASSERT_EQUAL_UINT32(1, log.nextSeq());
}

void test_commit_log_never_blocks_the_render_path() {
    // Structural guard. The render path calls record() directly, so the enqueue must
    // use a zero tick wait and drop-with-count on pressure. A blocking send here would
    // put SD latency inside display timing, which is the one thing this must not do.
    const std::string source = readProjectFile("src/modules/display/display_commit_log.cpp");
    TEST_ASSERT_FALSE(source.empty());
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("xQueueSend(queue_, &snapshot, 0)"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("portMAX_DELAY)  != pdTRUE"));
    const size_t enqueue = source.find("bool V1DisplayCommitLog::enqueueSnapshot");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, enqueue);
    const size_t format = source.find("bool V1DisplayCommitLog::formatCsvLine", enqueue);
    const std::string body = source.substr(enqueue, format - enqueue);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("droppedSnapshots_.fetch_add"));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("SDLock"));
}

void test_commit_log_export_drain_is_nonblocking_and_writes_a_terminal_fence() {
    const std::string source = readProjectFile("src/modules/display/display_commit_log.cpp");
    TEST_ASSERT_FALSE(source.empty());
    const size_t drain = source.find("bool V1DisplayCommitLog::tryDrainAndClose()");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, drain);
    const std::string body = source.substr(drain);
    const size_t pending = body.find("pendingWrites_.load");
    const size_t queued = body.find("uxQueueMessagesWaiting(queue_)");
    const size_t tryLock = body.find("StorageManager::SDTryLock");
    const size_t marker = body.find("COMMIT_EXPORT_MARKER_FORMAT");
    const size_t flush = body.find("persistentFile_.flush()", marker);
    const size_t close = body.find("persistentFile_.close()", flush);
    TEST_ASSERT_TRUE(pending < tryLock && queued < tryLock);
    TEST_ASSERT_TRUE(tryLock < marker && marker < flush && flush < close);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("display_commit_export_schema=1"));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("SDLockBlocking"));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("vTaskDelay"));
}

void test_render_commit_digest_joins_the_parser_published_alert_table() {
    // The encounter rows retain the parser-published table. The display evidence
    // must name that same table, even when the composer filters or reorders cards.
    const std::string source = readProjectFile("src/display_update.cpp");
    TEST_ASSERT_FALSE(source.empty());
    TEST_ASSERT_EQUAL(std::string::npos, source.find("v1AlertTableFnv1a32(allAlerts, alertCount)"));
}

void test_render_request_is_stamped_at_the_public_frame_boundary() {
    const std::string source = readProjectFile("src/display_update.cpp");
    TEST_ASSERT_FALSE(source.empty());
    const size_t entry = source.find("void V1Display::renderFrame(const RenderFrame& frame)");
    const size_t stamp = source.find("activeRenderRequestDutMicros_ = QualificationClock::nowMicros();", entry);
    const size_t dispatch = source.find("switch (frame.primaryKind)", entry);
    TEST_ASSERT_TRUE(entry < stamp && stamp < dispatch);
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("activeRenderRequestDutMicros_ != 0 ? activeRenderRequestDutMicros_", dispatch));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("lastPhysicalCommitDutMicros_", dispatch));
}

void test_qualification_only_display_paths_are_complete_and_gated() {
    const std::string updateSource = readProjectFile("src/display_update.cpp");
    const std::string screenSource = readProjectFile("src/display_screens.cpp");
    TEST_ASSERT_FALSE(updateSource.empty());
    TEST_ASSERT_FALSE(screenSource.empty());
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          updateSource.find("qualificationOnly && !v1DisplayCommitLog.isQualificationSessionActive()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, updateSource.find("V1DisplayCommitPath::Stealth"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, screenSource.find("V1DisplayCommitPath::Scanning"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, screenSource.find("V1DisplayCommitPath::Resting"));

    const size_t scanning = screenSource.find("void V1Display::showScanning()");
    const size_t scanningFlush = screenSource.find("DISPLAY_FLUSH();", scanning);
    const size_t scanningRecord = screenSource.find("recordDisplayCommit(V1DisplayCommitPath::Scanning", scanning);
    TEST_ASSERT_TRUE(scanning < scanningFlush && scanningFlush < scanningRecord);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_commit_log_records_resolved_state_and_dispatch);
    RUN_TEST(test_commit_log_keeps_flash_bits_and_dispatch_independent);
    RUN_TEST(test_commit_log_records_every_dispatch_kind_including_no_push);
    RUN_TEST(test_commit_log_reports_its_own_losses_inside_the_record);
    RUN_TEST(test_commit_log_is_inert_without_storage);
    RUN_TEST(test_commit_log_hands_out_monotonic_sequence_ids);
    RUN_TEST(test_commit_log_never_blocks_the_render_path);
    RUN_TEST(test_commit_log_export_drain_is_nonblocking_and_writes_a_terminal_fence);
    RUN_TEST(test_render_commit_digest_joins_the_parser_published_alert_table);
    RUN_TEST(test_render_request_is_stamped_at_the_public_frame_boundary);
    RUN_TEST(test_qualification_only_display_paths_are_complete_and_gated);
    return UNITY_END();
}
