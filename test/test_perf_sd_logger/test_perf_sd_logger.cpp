/**
 * Perf SD Logger — storage lifecycle contracts.
 *
 * The perf logger's SD path is production-only (native builds use mocks), so
 * these tests pin the source contract textually, following the pattern
 * established for the ALP logger's handle-reuse contract.
 *
 * Contract under test: warm-at-boot. The perf file's first writes in a boot (/perf
 * mkdir, CSV create, header, session marker) carry FAT-allocation cost that
 * was observed at 10-25 ms per write on a worn card. begin() must pay that
 * cost during setup — before BLE connects and before enabled_ flips true — so
 * successful warm-up keeps it off flush boundaries while alerts are in flight.
 */

#include <unity.h>

#include <FS.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
std::string readProjectFile(const char* relativePath) {
    const std::filesystem::path path = std::filesystem::path(PROJECT_DIR) / relativePath;
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

std::filesystem::path resetTestRoot(const char* name) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

std::string parserSafePadding(size_t bytes) {
    constexpr size_t kRecordBytes = 512;
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(bytes % kRecordBytes));
    std::string block(kRecordBytes, ' ');
    block.front() = '#';
    block.back() = '\n';
    std::string result;
    result.reserve(bytes);
    while (result.size() < bytes) {
        result += block;
    }
    return result;
}

std::string sectorAlignedTransaction(const std::string& data) {
    constexpr size_t kSectorBytes = 512;
    TEST_ASSERT_FALSE(data.empty());
    TEST_ASSERT_EQUAL_CHAR('\n', data.back());
    std::string result = data;
    const size_t remainder = result.size() % kSectorBytes;
    if (remainder == 0) {
        return result;
    }
    const size_t padding = kSectorBytes - remainder;
    if (padding == 1) {
        result.push_back('\n');
    } else {
        result.push_back('#');
        result.append(padding - 2, ' ');
        result.push_back('\n');
    }
    return result;
}

void writeAll(File& file, const std::string& text) {
    TEST_ASSERT_EQUAL_UINT32(
        static_cast<uint32_t>(text.size()),
        static_cast<uint32_t>(file.write(reinterpret_cast<const uint8_t*>(text.data()), text.size())));
}

std::string trimSpaces(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}
} // namespace

void setUp() {}
void tearDown() {}

void test_perf_sd_logger_begin_warms_storage_at_boot() {
    const std::string source = readProjectFile("src/perf_sd_logger.cpp");
    TEST_ASSERT_FALSE(source.empty());

    const size_t beginStart = source.find("void PerfSdLogger::begin(");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, beginStart);
    const size_t enabledTrue = source.find("enabled_ = true;", beginStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, enabledTrue);
    const std::string beginBody = source.substr(beginStart, enabledTrue - beginStart);

    // Warm-up runs under the SD lock, opens the persistent handle through the
    // same helper the writer task uses, and writes header + session marker.
    TEST_ASSERT_NOT_EQUAL(std::string::npos, beginBody.find("StorageManager::SDLockBlocking"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, beginBody.find("ensurePersistentFileLocked(*fs)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, beginBody.find("ensureCsvHeaderAndSessionMarker(persistentFile_)"));
}

void test_perf_sd_logger_writer_and_warmup_share_one_open_path() {
    // The open/retry sequence must exist exactly once, in the shared helper —
    // not duplicated between the writer task and the boot warm-up.
    const std::string source = readProjectFile("src/perf_sd_logger.cpp");
    TEST_ASSERT_FALSE(source.empty());

    const std::string openLine = "persistentFile_ = fs.open(csvPath, FILE_APPEND, true);";
    const size_t first = source.find(openLine);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, first);
    const size_t second = source.find(openLine, first + openLine.size());
    TEST_ASSERT_NOT_EQUAL(std::string::npos, second);
    TEST_ASSERT_EQUAL(std::string::npos, source.find(openLine, second + openLine.size()));

    const size_t helperStart = source.find("bool PerfSdLogger::ensurePersistentFileLocked(fs::FS& fs)");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, helperStart);
    const size_t helperEnd = source.find("\nbool PerfSdLogger::", helperStart + 1);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, helperEnd);
    TEST_ASSERT_TRUE(first > helperStart && second < helperEnd);

    // The writer task path delegates to the helper instead of reopening.
    const size_t appendStart = source.find("bool PerfSdLogger::appendSnapshotLine(");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, appendStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("ensurePersistentFileLocked(*fs)", appendStart));
}

void test_mock_r_plus_overwrites_reserve_then_grows_at_exact_capacity() {
    const std::filesystem::path root = resetTestRoot("v1_perf_sd_r_plus_boundary");
    fs::FS fs(root);

    File created = fs.open("/perf/reserved.csv", FILE_WRITE, true);
    TEST_ASSERT_TRUE(created);
    writeAll(created, parserSafePadding(1024));
    created.flush();
    created.close();

    File verified = fs.open("/perf/reserved.csv", FILE_READ, false);
    TEST_ASSERT_TRUE(verified);
    TEST_ASSERT_EQUAL_UINT32(1024, static_cast<uint32_t>(verified.size()));
    verified.close();

    File update = fs.open("/perf/reserved.csv", "r+", false);
    TEST_ASSERT_TRUE(update);
    TEST_ASSERT_TRUE(update.seek(1023, SeekSet));
    writeAll(update, "X");
    TEST_ASSERT_EQUAL_UINT32(1024, static_cast<uint32_t>(update.position()));
    TEST_ASSERT_EQUAL_UINT32(1024, static_cast<uint32_t>(update.size()));

    writeAll(update, "Y");
    TEST_ASSERT_EQUAL_UINT32(1025, static_cast<uint32_t>(update.position()));
    update.flush();
    update.close();

    File grown = fs.open("/perf/reserved.csv", FILE_READ, false);
    TEST_ASSERT_TRUE(grown);
    TEST_ASSERT_EQUAL_UINT32(1025, static_cast<uint32_t>(grown.size()));
    grown.close();

    File missing = fs.open("/perf/missing.csv", "r+", false);
    TEST_ASSERT_FALSE(missing);
}

void test_reserved_padding_remains_csv_safe_across_sessions_and_append_fallback() {
    const std::filesystem::path root = resetTestRoot("v1_perf_sd_parser_safe_padding");
    fs::FS fs(root);
    const std::string path = "/perf/reserved.csv";

    File created = fs.open(path.c_str(), FILE_WRITE, true);
    TEST_ASSERT_TRUE(created);
    writeAll(created, parserSafePadding(4096));
    created.flush();
    created.close();

    const std::vector<std::string> records = {
        "millis,rx\n#session_start,seq=1,bootId=7,uptime_ms=100,token=AAAA0001,schema=47\n",
        "100,1\n",
        "millis,rx\n#session_start,seq=2,bootId=7,uptime_ms=200,token=BBBB0002,schema=47\n",
        "200,2\n",
    };
    File update = fs.open(path.c_str(), "r+", false);
    TEST_ASSERT_TRUE(update);
    for (const std::string& record : records) {
        writeAll(update, sectorAlignedTransaction(record));
    }
    const size_t logicalEnd = update.position();
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(logicalEnd % 512));
    TEST_ASSERT_TRUE(logicalEnd < 4096);
    update.flush();
    update.close();

    // Reserved reopen/seek failure falls back to FILE_APPEND at physical EOF.
    // The untouched gap must be only blank/comment records.
    File append = fs.open(path.c_str(), FILE_APPEND, true);
    TEST_ASSERT_TRUE(append);
    writeAll(append, "205,3\n");
    append.flush();
    append.close();

    std::ifstream input(root / "perf" / "reserved.csv", std::ios::binary);
    TEST_ASSERT_TRUE(input.is_open());
    std::vector<std::string> meaningful;
    std::string line;
    bool headerNeedsMarker = false;
    uint32_t adjacentMarkers = 0;
    while (std::getline(input, line)) {
        const std::string trimmed = trimSpaces(line);
        if (headerNeedsMarker) {
            TEST_ASSERT_TRUE(trimmed.rfind("#session_start", 0) == 0);
            headerNeedsMarker = false;
            adjacentMarkers++;
            continue;
        }
        if (trimmed == "millis,rx") {
            meaningful.push_back(trimmed);
            headerNeedsMarker = true;
            continue;
        }
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        meaningful.push_back(trimmed);
    }
    TEST_ASSERT_FALSE(headerNeedsMarker);
    TEST_ASSERT_EQUAL_UINT32(2, adjacentMarkers);
    TEST_ASSERT_EQUAL_UINT32(5, static_cast<uint32_t>(meaningful.size()));
    TEST_ASSERT_EQUAL_STRING("millis,rx", meaningful[0].c_str());
    TEST_ASSERT_EQUAL_STRING("100,1", meaningful[1].c_str());
    TEST_ASSERT_EQUAL_STRING("millis,rx", meaningful[2].c_str());
    TEST_ASSERT_EQUAL_STRING("200,2", meaningful[3].c_str());
    TEST_ASSERT_EQUAL_STRING("205,3", meaningful[4].c_str());
}

void test_sector_aligned_transaction_exact_tail_boundaries_are_parser_safe() {
    const std::string exact = std::string(511, 'A') + "\n";
    const std::string onePadByte = std::string(510, 'B') + "\n";
    const std::string crossSector = std::string(512, 'C') + "\n";

    const std::string exactResult = sectorAlignedTransaction(exact);
    const std::string onePadResult = sectorAlignedTransaction(onePadByte);
    const std::string crossResult = sectorAlignedTransaction(crossSector);
    TEST_ASSERT_EQUAL_UINT32(512, static_cast<uint32_t>(exactResult.size()));
    TEST_ASSERT_EQUAL_UINT32(512, static_cast<uint32_t>(onePadResult.size()));
    TEST_ASSERT_EQUAL_CHAR('\n', onePadResult[510]);
    TEST_ASSERT_EQUAL_CHAR('\n', onePadResult[511]);
    TEST_ASSERT_EQUAL_UINT32(1024, static_cast<uint32_t>(crossResult.size()));
    TEST_ASSERT_EQUAL_CHAR('\n', crossResult[512]);
    TEST_ASSERT_EQUAL_CHAR('#', crossResult[513]);
    TEST_ASSERT_EQUAL_CHAR('\n', crossResult.back());
}

void test_perf_sd_logger_reserve_source_contract_is_fail_safe_and_measured() {
    const std::string source = readProjectFile("src/perf_sd_logger.cpp");
    TEST_ASSERT_FALSE(source.empty());

    // 51: schema 50 dropped 19 structurally-dead columns (15 wifi* + 4 cached-DMA);
    // 51 dropped the largest-block watermark, which sampled MALLOC_CAP_DEFAULT
    // (PSRAM on this board) and read 7,077,876 in every row ever collected.
    // Existing CSVs are not column-comparable across either bump.
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("PERF_CSV_SCHEMA_VERSION = 51"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("minLargestBlock"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("wifiStartApBringupMax_us"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("snapshot.freeDmaMin"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("dutMicros,clockSegment"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("snapshot.dutMicros"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("snapshot.clockSegment"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("notifyToDisplayPipelineCompleteMax_ms,notifyToDisplayPipelineCompleteTotalCount"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("snapshot.notifyToDisplayPipelineCompleteMaxMs"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("snapshot.notifyToDisplayPipelineCompleteTotalCount"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("notifyToDisplayMax_ms"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("snapshot.notifyToDisplayMaxMs"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("PERF_SD_CONTIGUOUS_RESERVE_SIZE = 1024 * 1024"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("PERF_CSV_LINE_BUFFER_SIZE = 6656"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("(sizeof(PERF_CSV_HEADER) - 1) + PERF_SD_SESSION_MARKER_BUFFER_SIZE <= "
                                      "PERF_CSV_LINE_BUFFER_SIZE"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("/perf/.perf_reserve.tmp"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("esp_vfs_fat_create_contiguous_file(fs.mountpoint(), fullPath, size, false)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("int contiguousWord = 0;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("reinterpret_cast<bool*>(&contiguousWord)"));

    const size_t prepStart = source.find("bool PerfSdLogger::prepareReservedFileLocked(");
    const size_t prepEnd = source.find("\nbool PerfSdLogger::", prepStart + 1);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, prepStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, prepEnd);
    const std::string prep = source.substr(prepStart, prepEnd - prepStart);
    const size_t padding = prep.find("writeParserSafeReservePadding(reserve)");
    const size_t flush = prep.find("reserve.flush()", padding);
    const size_t close = prep.find("reserve.close()", flush);
    const size_t reopen = prep.find("fs.open(PERF_SD_RESERVE_TEMP_PATH, FILE_READ, false)", close);
    const size_t exactSize = prep.find("physicalBytes != PERF_SD_CONTIGUOUS_RESERVE_SIZE", reopen);
    const size_t verify = prep.find("verifyContiguousReserve", exactSize);
    const size_t rename = prep.find("fs.rename(PERF_SD_RESERVE_TEMP_PATH, csvPath)", verify);
    TEST_ASSERT_TRUE(padding < flush && flush < close && close < reopen && reopen < exactSize && exactSize < verify &&
                     verify < rename);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, prep.find("fallback_create"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, prep.find("fallback_padding"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, prep.find("fallback_size_reopen"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, prep.find("fallback_size_mismatch"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, prep.find("fallback_test"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, prep.find("fallback_rename"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, prep.find("logPrep(\"active\""));

    const size_t paddingStart = source.find("bool PerfSdLogger::writeParserSafeReservePadding(");
    const size_t paddingEnd = source.find("\nbool PerfSdLogger::", paddingStart + 1);
    const std::string paddingBody = source.substr(paddingStart, paddingEnd - paddingStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, paddingBody.find("writeStagingBuffer_[0] = '#'"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          paddingBody.find("writeStagingBuffer_[PERF_SD_WRITE_STAGING_SIZE - 1] = '\\n'"));

    const size_t metadataStart = source.find("bool PerfSdLogger::ensureCsvHeaderAndSessionMarker(");
    const size_t metadataEnd = source.find("\nbool PerfSdLogger::", metadataStart + 1);
    const std::string metadataBody = source.substr(metadataStart, metadataEnd - metadataStart);
    const size_t bothPending = metadataBody.find("!csvHeaderReady_ && sessionMarkerPending_");
    const size_t copyHeader = metadataBody.find("memcpy(csvLineBuffer_, PERF_CSV_HEADER, headerLen)", bothPending);
    const size_t formatMarker = metadataBody.find("formatSessionMarker(csvLineBuffer_ + headerLen", copyHeader);
    const size_t combinedWrite = metadataBody.find("headerLen + markerLen", formatMarker);
    TEST_ASSERT_TRUE(bothPending < copyHeader && copyHeader < formatMarker && formatMarker < combinedWrite);
}

void test_perf_sd_logger_cursor_durability_and_logical_export_contracts() {
    const std::string source = readProjectFile("src/perf_sd_logger.cpp");
    TEST_ASSERT_FALSE(source.empty());
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("PERF_SD_FLUSH_EVERY_ROWS = 1"));

    const size_t openStart = source.find("bool PerfSdLogger::ensurePersistentFileLocked(");
    const size_t openEnd = source.find("\nbool PerfSdLogger::", openStart + 1);
    const std::string openBody = source.substr(openStart, openEnd - openStart);
    const size_t rPlus = openBody.find("fs.open(csvPath, PERF_SD_READ_WRITE_MODE, false)");
    const size_t seek = openBody.find("persistentFile_.seek(static_cast<uint32_t>(reservedLogicalEnd_), SeekSet)");
    const size_t fallback = openBody.find("persistentFile_ = fs.open(csvPath, FILE_APPEND, true)", seek);
    TEST_ASSERT_TRUE(rPlus < seek && seek < fallback);

    const size_t writeStart = source.find("bool PerfSdLogger::writeStaged(");
    const size_t writeEnd = source.find("\nbool PerfSdLogger::", writeStart + 1);
    const std::string writeBody = source.substr(writeStart, writeEnd - writeStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, writeBody.find("f.position() != writeStart"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, writeBody.find("data[len - 1] != '\\n'"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          writeBody.find("f.write(writeStagingBuffer_, PERF_SD_WRITE_STAGING_SIZE)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, writeBody.find("paddingLen == 1"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, writeBody.find("writeEnd - writeStart != paddedLen"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, writeBody.find("reservedLogicalEnd_ = writeEnd"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, writeBody.find("writeEnd > PERF_SD_CONTIGUOUS_RESERVE_SIZE"));

    const size_t appendStart = source.find("bool PerfSdLogger::appendSnapshotLine(");
    const size_t appendEnd = source.find("\nvoid PerfSdLogger::drainAndClose", appendStart);
    const std::string appendBody = source.substr(appendStart, appendEnd - appendStart);
    const size_t timer = appendBody.find("uint32_t startUs = PERF_TIMESTAMP_US()");
    const size_t rowWrite = appendBody.find("writeStaged(persistentFile_", timer);
    const size_t rowFlush = appendBody.find("flushPersistentFileIfDue(persistentFile_)", rowWrite);
    const size_t record = appendBody.find("perfRecordSdFlushUs(PERF_TIMESTAMP_US() - startUs)", rowFlush);
    TEST_ASSERT_TRUE(timer < rowWrite && rowWrite < rowFlush && rowFlush < record);

    const size_t drainStart = source.find("void PerfSdLogger::drainAndClose(");
    const size_t drainEnd = source.find("\nbool PerfSdLogger::tryDrainAndClose", drainStart);
    const std::string drainBody = source.substr(drainStart, drainEnd - drainStart);
    TEST_ASSERT_TRUE(drainBody.find("flushPersistentFile(persistentFile_)") <
                     drainBody.find("persistentFile_.close()"));

    const size_t tryStart = source.find("bool PerfSdLogger::tryDrainAndClose(");
    const size_t tryEnd = source.find("\nbool PerfSdLogger::tryResolveExportSize", tryStart);
    const std::string tryBody = source.substr(tryStart, tryEnd - tryStart);
    TEST_ASSERT_EQUAL(std::string::npos, tryBody.find("reservedLogicalEnd_"));

    const size_t exportStart = tryEnd + 1;
    const std::string exportBody = source.substr(exportStart);
    const size_t queueIdle = exportBody.find("uxQueueMessagesWaiting(queue_)");
    const size_t pendingIdle = exportBody.find("pendingWrites_.load(std::memory_order_relaxed)");
    const size_t handleIdle = exportBody.find("persistentFile_");
    const size_t chooseLayout = exportBody.find("if (!reservedLayoutActive_)");
    TEST_ASSERT_TRUE(queueIdle < chooseLayout && pendingIdle < chooseLayout && handleIdle < chooseLayout);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, exportBody.find("selectedSize = physicalSize"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, exportBody.find("reservedLogicalEnd_ > physicalSize"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, exportBody.find("selectedSize = reservedLogicalEnd_"));
    TEST_ASSERT_EQUAL(std::string::npos, exportBody.find("truncate"));

    const size_t exhaustion = appendBody.find("result=extent_exhausted", record);
    TEST_ASSERT_TRUE(record < exhaustion);
}

void test_qualification_export_uses_current_perf_logical_prefix_without_changing_qstart() {
    const std::string qualification = readProjectFile("src/modules/qualification/qualification_serial_module.cpp");
    const std::string wiring = readProjectFile("src/main_runtime_wiring.cpp");
    TEST_ASSERT_FALSE(qualification.empty());
    TEST_ASSERT_FALSE(wiring.empty());

    const size_t startRun = qualification.find("bool QualificationSerialModule::startRun(");
    const size_t startRunEnd = qualification.find("\nvoid QualificationSerialModule::enterFinalizing", startRun);
    const std::string startBody = qualification.substr(startRun, startRunEnd - startRun);
    const size_t drain = startBody.find("providers_.tryDrainPerf(providers_.ctx)");
    const size_t newSession = startBody.find("providers_.startPerfSession(providers_.ctx)");
    TEST_ASSERT_TRUE(drain < newSession);
    TEST_ASSERT_EQUAL(std::string::npos, startBody.find("tryResolvePerfExportSize("));

    const size_t handleGet = qualification.find("void QualificationSerialModule::handleGetCsv(");
    const size_t handleGetEnd = qualification.find("\nvoid QualificationSerialModule::handleBsc08", handleGet);
    const std::string handleGetBody = qualification.substr(handleGet, handleGetEnd - handleGet);
    const size_t currentExport = handleGetBody.find("strcmp(requested, currentPerfPath) == 0");
    const size_t exportDrain = handleGetBody.find("providers_.tryDrainPerf(providers_.ctx)", currentExport);
    const size_t openAfterDrain = handleGetBody.find("openExport(requested)", exportDrain);
    TEST_ASSERT_TRUE(currentExport < exportDrain && exportDrain < openAfterDrain);
    // The display-commit export/drain gate is gone with the commit log itself.
    TEST_ASSERT_EQUAL(std::string::npos, handleGetBody.find("currentDisplayCommitPath"));
    TEST_ASSERT_EQUAL(std::string::npos, handleGetBody.find("tryDrainDisplayCommit"));

    const size_t serviceRun = qualification.find("void QualificationSerialModule::serviceRun(");
    const size_t serviceRunEnd = qualification.find("\nvoid QualificationSerialModule::serviceExport", serviceRun);
    const std::string serviceRunBody = qualification.substr(serviceRun, serviceRunEnd - serviceRun);
    const size_t terminalError = serviceRunBody.find("state_ == State::Error");
    const size_t terminalDrain = serviceRunBody.find("providers_.tryDrainPerf(providers_.ctx)", terminalError);
    TEST_ASSERT_TRUE(terminalError < terminalDrain);
    TEST_ASSERT_EQUAL(std::string::npos, serviceRunBody.find("tryDrainDisplayCommit"));

    const size_t openExport = qualification.find("bool QualificationSerialModule::openExport(");
    const size_t openExportEnd = qualification.find("\nvoid QualificationSerialModule::closeExport", openExport);
    const std::string openBody = qualification.substr(openExport, openExportEnd - openExport);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, openBody.find("validCanonicalExportPath(path)"));
    const size_t pathPolicy = qualification.find("bool validCanonicalExportPath(");
    const size_t pathPolicyEnd = qualification.find("\n} // namespace", pathPolicy);
    const std::string pathPolicyBody = qualification.substr(pathPolicy, pathPolicyEnd - pathPolicy);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, pathPolicyBody.find("segmentStart && c == '.'"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, pathPolicyBody.find("segmentStart || cursor[-1] == '.'"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, pathPolicyBody.find("c >= 'a' && c <= 'z'"));
    TEST_ASSERT_EQUAL(std::string::npos, pathPolicyBody.find("c >= 'A' && c <= 'Z'"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, pathPolicyBody.find("path[pathLen - 1] != '.'"));
    const size_t physical = openBody.find("const size_t physicalSize = exportFile_.size()");
    const size_t currentPath = openBody.find("strcmp(path, currentPerfPath) == 0", physical);
    const size_t resolve = openBody.find("providers_.tryResolvePerfExportSize(physicalSize, selectedSize", currentPath);
    const size_t unavailable = openBody.find("setError(\"export_size_unavailable\")", resolve);
    const size_t bounds = openBody.find("selectedSize > physicalSize", unavailable);
    const size_t active = openBody.find("exportActive_ = true", bounds);
    TEST_ASSERT_TRUE(physical < currentPath && currentPath < resolve && resolve < unavailable && unavailable < bounds &&
                     bounds < active);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, openBody.find("physicalSize"));

    const size_t serviceExport = qualification.find("void QualificationSerialModule::serviceExport(");
    const size_t serviceExportEnd =
        qualification.find("\nvoid QualificationSerialModule::handleCommand", serviceExport);
    const std::string serviceBody = qualification.substr(serviceExport, serviceExportEnd - serviceExport);
    const size_t remaining = serviceBody.find("exportSize_ - exportBytes_");
    const size_t boundedRead = serviceBody.find("remaining < sizeof(bytes) ? remaining : sizeof(bytes)", remaining);
    const size_t truncated = serviceBody.find("sendErrorLine(\"export_truncated\")", boundedRead);
    const size_t qendBytes = serviceBody.find("io_->print(exportBytes_)", truncated);
    TEST_ASSERT_TRUE(remaining < boundedRead && boundedRead < truncated && truncated < qendBytes);

    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        wiring.find("providers.tryResolvePerfExportSize = [](size_t physicalBytes, size_t& selectedBytes"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          wiring.find("perfSdLogger.tryResolveExportSize(physicalBytes, selectedBytes)"));
    // The display commit log is gone; its export/drain wiring must not return.
    TEST_ASSERT_EQUAL(std::string::npos, wiring.find("displayCommitCsvPath"));
    TEST_ASSERT_EQUAL(std::string::npos, wiring.find("v1DisplayCommitLog"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_perf_sd_logger_begin_warms_storage_at_boot);
    RUN_TEST(test_perf_sd_logger_writer_and_warmup_share_one_open_path);
    RUN_TEST(test_mock_r_plus_overwrites_reserve_then_grows_at_exact_capacity);
    RUN_TEST(test_reserved_padding_remains_csv_safe_across_sessions_and_append_fallback);
    RUN_TEST(test_sector_aligned_transaction_exact_tail_boundaries_are_parser_safe);
    RUN_TEST(test_perf_sd_logger_reserve_source_contract_is_fail_safe_and_measured);
    RUN_TEST(test_perf_sd_logger_cursor_durability_and_logical_export_contracts);
    RUN_TEST(test_qualification_export_uses_current_perf_logical_prefix_without_changing_qstart);
    return UNITY_END();
}
