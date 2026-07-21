#include "wifi_diagnostics_api_service.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <cstring>

#if !defined(UNIT_TEST)
#include <cerrno>
#include <sys/socket.h>

#include <esp_task_wdt.h>
#endif

#include "wifi_api_response.h"
#include "wifi_diagnostics_stream.h"
#include "wifi_json_document.h"

namespace WifiDiagnosticsApiService {
namespace {

constexpr size_t MAX_PATH_BYTES = 120;
constexpr size_t MAX_LEAF_BYTES = 96;
constexpr size_t POWEROFF_TAIL_SCAN_BYTES = 2048;
constexpr size_t MAX_EVIDENCE_LINE_BYTES = 384;
constexpr size_t MAX_EVIDENCE_LINES = 8;
constexpr size_t DOWNLOAD_CHUNK_BYTES = 1024;

bool hasPrefix(const char* value, const char* prefix) {
    return value && prefix && std::strncmp(value, prefix, std::strlen(prefix)) == 0;
}

bool isSafeLeaf(const char* leaf) {
    if (!leaf || leaf[0] == '\0' || leaf[0] == '.') {
        return false;
    }

    const size_t length = std::strlen(leaf);
    if (length > MAX_LEAF_BYTES || std::strstr(leaf, "..") != nullptr) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        const char c = leaf[i];
        const bool alphaNumeric = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alphaNumeric && c != '_' && c != '-' && c != '.') {
            return false;
        }
    }
    return true;
}

const char* leafName(const char* path) {
    if (!path) {
        return "";
    }
    const char* lastSlash = std::strrchr(path, '/');
    return lastSlash ? lastSlash + 1 : path;
}

void markUiActivity(const Runtime& runtime) {
    if (runtime.markUiActivity) {
        runtime.markUiActivity(runtime.ctx);
    }
}

bool requireMaintenanceStorage(WebServer& server, const Runtime& runtime) {
    if (!runtime.maintenanceBootActive) {
        server.send(409, "application/json", "{\"success\":false,\"error\":\"maintenance_mode_required\"}");
        return false;
    }
    if (!runtime.storageReady || !runtime.sdCard || !runtime.filesystem) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"sd_card_unavailable\"}");
        return false;
    }
    return true;
}

void appendFile(JsonArray files, fs::FS& filesystem, const char* path, const char* category, size_t& fileCount) {
    File file = filesystem.open(path, FILE_READ);
    if (!file || file.isDirectory()) {
        file.close();
        return;
    }

    JsonObject item = files.add<JsonObject>();
    if (!item.isNull()) {
        item["path"] = path;
        item["category"] = category;
        item["sizeBytes"] = file.size();
        ++fileCount;
    }
    file.close();
}

void appendDirectory(JsonArray files, fs::FS& filesystem, const char* directory, const char* category,
                     size_t& fileCount, size_t& scannedEntryCount, bool& truncated) {
    File dir = filesystem.open(directory, FILE_READ);
    if (!dir || !dir.isDirectory()) {
        dir.close();
        return;
    }

    File entry = dir.openNextFile();
    while (entry) {
        if (fileCount >= MAX_LISTED_FILES || scannedEntryCount >= MAX_SCANNED_ENTRIES) {
            truncated = true;
            entry.close();
            break;
        }
        ++scannedEntryCount;

        if (!entry.isDirectory() && isSafeLeaf(leafName(entry.name()))) {
            String canonicalPath(directory);
            canonicalPath += "/";
            canonicalPath += leafName(entry.name());
            JsonObject item = files.add<JsonObject>();
            if (item.isNull()) {
                truncated = true;
                entry.close();
                break;
            }
            item["path"] = canonicalPath;
            item["category"] = category;
            item["sizeBytes"] = entry.size();
            ++fileCount;
        }

        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
}

String readLatestPoweroffEvidence(fs::FS& filesystem) {
    File file = filesystem.open("/poweroff.log", FILE_READ);
    if (!file || file.isDirectory()) {
        file.close();
        return String();
    }

    const size_t size = file.size();
    const size_t start = size > POWEROFF_TAIL_SCAN_BYTES ? size - POWEROFF_TAIL_SCAN_BYTES : 0;
    if (start > 0 && !file.seek(static_cast<uint32_t>(start))) {
        file.close();
        return String();
    }

    bool discardPartialLine = start > 0;
    String current;
    String latestLines[MAX_EVIDENCE_LINES];
    size_t lineCount = 0;
    auto retainLine = [&latestLines, &lineCount](const String& line) {
        if (line.length() == 0) {
            return;
        }
        if (lineCount < MAX_EVIDENCE_LINES) {
            latestLines[lineCount++] = line;
            return;
        }
        for (size_t i = 1; i < MAX_EVIDENCE_LINES; ++i) {
            latestLines[i - 1] = latestLines[i];
        }
        latestLines[MAX_EVIDENCE_LINES - 1] = line;
    };
    while (file.available() > 0) {
        const int next = file.read();
        if (next < 0) {
            break;
        }
        const char c = static_cast<char>(next);
        if (c == '\n' || c == '\r') {
            if (discardPartialLine) {
                discardPartialLine = false;
                current = String();
            } else if (current.length() > 0) {
                retainLine(current);
                current = String();
            }
            continue;
        }
        if (!discardPartialLine && current.length() < MAX_EVIDENCE_LINE_BYTES) {
            current += c;
        }
    }
    if (!discardPartialLine && current.length() > 0) {
        retainLine(current);
    }
    file.close();

    String evidence;
    for (size_t i = 0; i < lineCount; ++i) {
        if (evidence.length() > 0) {
            evidence += '\n';
        }
        evidence += latestLines[i];
    }
    return evidence;
}

const char* contentTypeFor(const char* path) {
    const size_t length = path ? std::strlen(path) : 0;
    return length >= 4 && std::strcmp(path + length - 4, ".csv") == 0 ? "text/csv" : "text/plain";
}

fs::FS* filesystemForPath(const Runtime& runtime, const char* path) {
    if (path && std::strcmp(path, "/panic.txt") == 0) {
        return runtime.panicFilesystem;
    }
    return runtime.filesystem;
}

bool streamDownloadBody(WebServer& server, File& file, size_t fileSize) {
    auto& client = server.client();
    uint8_t buffer[DOWNLOAD_CHUNK_BYTES];
    size_t remaining = fileSize;

#if defined(UNIT_TEST)
    uint32_t testClockMs = 0;
    auto now = [&testClockMs]() { return testClockMs; };
    auto wait = [&testClockMs](uint16_t delayMs) { testClockMs += delayMs; };
    auto feedWatchdog = []() {};
    auto attempt = [&client](const uint8_t* data, size_t length) {
        if (!client.connected()) {
            return WifiDiagnosticsStream::AttemptResult{WifiDiagnosticsStream::AttemptState::Disconnected, 0};
        }
        const size_t written = client.write(data, length);
        return written > 0
                   ? WifiDiagnosticsStream::AttemptResult{WifiDiagnosticsStream::AttemptState::Progress, written}
                   : WifiDiagnosticsStream::AttemptResult{WifiDiagnosticsStream::AttemptState::WouldBlock, 0};
    };
#else
    auto now = []() { return static_cast<uint32_t>(millis()); };
    auto wait = [](uint16_t delayMs) { delay(delayMs); };
    auto feedWatchdog = []() { (void)esp_task_wdt_reset(); };
    auto attempt = [&client](const uint8_t* data, size_t length) {
        if (!client.connected() || client.fd() < 0) {
            return WifiDiagnosticsStream::AttemptResult{WifiDiagnosticsStream::AttemptState::Disconnected, 0};
        }

        const ssize_t written = ::send(client.fd(), data, length, MSG_DONTWAIT);
        if (written > 0) {
            return WifiDiagnosticsStream::AttemptResult{WifiDiagnosticsStream::AttemptState::Progress,
                                                        static_cast<size_t>(written)};
        }
        if (written == 0) {
            return WifiDiagnosticsStream::AttemptResult{WifiDiagnosticsStream::AttemptState::Disconnected, 0};
        }

        const int sendError = errno;
        if (sendError == EAGAIN || sendError == EWOULDBLOCK || sendError == EINTR || sendError == ENOBUFS) {
            return WifiDiagnosticsStream::AttemptResult{WifiDiagnosticsStream::AttemptState::WouldBlock, 0};
        }
        return WifiDiagnosticsStream::AttemptResult{WifiDiagnosticsStream::AttemptState::Error, 0};
    };
#endif

    const uint32_t downloadStartedAtMs = static_cast<uint32_t>(now());
    const WifiDiagnosticsStream::Config streamConfig;

    while (remaining > 0) {
        if (static_cast<uint32_t>(now() - downloadStartedAtMs) >= streamConfig.totalTimeoutMs) {
            return false;
        }
        feedWatchdog();
        const size_t bytesRead = file.read(buffer, std::min(remaining, sizeof(buffer)));
        if (bytesRead == 0) {
            return false;
        }
        if (!WifiDiagnosticsStream::writeAll(buffer, bytesRead, attempt, now, wait, feedWatchdog, downloadStartedAtMs,
                                             streamConfig)) {
            return false;
        }
        remaining -= bytesRead;
    }
    return true;
}

} // namespace

bool isAllowedLogPath(const char* path) {
    if (!path) {
        return false;
    }
    const size_t length = std::strlen(path);
    if (length == 0 || length > MAX_PATH_BYTES) {
        return false;
    }
    if (std::strcmp(path, "/poweroff.log") == 0 || std::strcmp(path, "/panic.txt") == 0) {
        return true;
    }

    constexpr const char* PERF_PREFIX = "/perf/";
    constexpr const char* ALP_PREFIX = "/alp/";
    if (hasPrefix(path, PERF_PREFIX)) {
        return isSafeLeaf(path + std::strlen(PERF_PREFIX));
    }
    if (hasPrefix(path, ALP_PREFIX)) {
        return isSafeLeaf(path + std::strlen(ALP_PREFIX));
    }
    return false;
}

void handleApiList(WebServer& server, const Runtime& runtime) {
    markUiActivity(runtime);
    if (!requireMaintenanceStorage(server, runtime)) {
        return;
    }

    WifiJson::Document doc;
    doc["success"] = true;
    doc["maxListedFiles"] = MAX_LISTED_FILES;
    doc["maxScannedEntries"] = MAX_SCANNED_ENTRIES;
    doc["maxDownloadBytes"] = MAX_DOWNLOAD_BYTES;
    JsonArray files = doc["files"].to<JsonArray>();
    size_t fileCount = 0;
    size_t scannedEntryCount = 0;
    bool truncated = false;

    appendFile(files, *runtime.filesystem, "/poweroff.log", "power", fileCount);
    if (fileCount < MAX_LISTED_FILES) {
        fs::FS* panicFilesystem = filesystemForPath(runtime, "/panic.txt");
        if (panicFilesystem) {
            appendFile(files, *panicFilesystem, "/panic.txt", "panic", fileCount);
        }
    }
    if (fileCount < MAX_LISTED_FILES) {
        appendDirectory(files, *runtime.filesystem, "/perf", "performance", fileCount, scannedEntryCount, truncated);
    }
    if (fileCount < MAX_LISTED_FILES) {
        appendDirectory(files, *runtime.filesystem, "/alp", "alp", fileCount, scannedEntryCount, truncated);
    } else {
        truncated = true;
    }

    doc["truncated"] = truncated;
    const String evidence = readLatestPoweroffEvidence(*runtime.filesystem);
    if (evidence.length() > 0) {
        doc["lastPoweroffEvidence"] = evidence;
    }
    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiDownload(WebServer& server, const Runtime& runtime) {
    markUiActivity(runtime);
    if (!requireMaintenanceStorage(server, runtime)) {
        return;
    }
    if (!server.hasArg("path")) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"path_required\"}");
        return;
    }

    const String requestedPath = server.arg("path");
    if (!isAllowedLogPath(requestedPath.c_str())) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"invalid_path\"}");
        return;
    }

    fs::FS* filesystem = filesystemForPath(runtime, requestedPath.c_str());
    if (!filesystem) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"storage_unavailable\"}");
        return;
    }

    File file = filesystem->open(requestedPath.c_str(), FILE_READ);
    if (!file || file.isDirectory()) {
        file.close();
        server.send(404, "application/json", "{\"success\":false,\"error\":\"log_not_found\"}");
        return;
    }
    const size_t fileSize = file.size();
    if (fileSize > MAX_DOWNLOAD_BYTES) {
        file.close();
        server.send(413, "application/json", "{\"success\":false,\"error\":\"log_too_large\"}");
        return;
    }

    String disposition = "attachment; filename=\"";
    disposition += leafName(requestedPath.c_str());
    disposition += "\"";
    server.sendHeader("Content-Disposition", disposition);
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(fileSize);
#if !defined(UNIT_TEST)
    (void)esp_task_wdt_reset();
#endif
    server.send(200, contentTypeFor(requestedPath.c_str()), "");
    // Match WebServer::streamFile(): the exact length applies only to this
    // response and must not leak into the next request on the same server.
    server.setContentLength(CONTENT_LENGTH_NOT_SET);
    if (!streamDownloadBody(server, file, fileSize)) {
        // The 200 header is already on the wire. Closing the socket leaves an
        // unmistakably incomplete body instead of pretending a truncated file
        // satisfied the declared Content-Length.
        server.client().stop();
    }
    file.close();
}

} // namespace WifiDiagnosticsApiService
