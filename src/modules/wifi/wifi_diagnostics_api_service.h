#pragma once

#include <FS.h>
#include <WebServer.h>

namespace WifiDiagnosticsApiService {

// Listing is deliberately bounded so a card with years of CSV files cannot
// turn one maintenance request into an unbounded heap allocation.
inline constexpr size_t MAX_LISTED_FILES = 64;
inline constexpr size_t MAX_SCANNED_ENTRIES = 128;
inline constexpr size_t MAX_DOWNLOAD_BYTES = 16u * 1024u * 1024u;

struct Runtime {
    fs::FS* filesystem = nullptr;
    // Crash breadcrumbs are written to LittleFS before primary storage mounts.
    // Keep that source distinct when SD is the primary diagnostics filesystem.
    fs::FS* panicFilesystem = nullptr;
    bool storageReady = false;
    bool sdCard = false;
    bool maintenanceBootActive = false;
    void (*markUiActivity)(void* ctx) = nullptr;
    void* ctx = nullptr;
};

// Public for native contract tests. Only canonical root diagnostics and a
// single safe filename below /perf or /alp are accepted.
bool isAllowedLogPath(const char* path);

// Callers must hold the StorageManager SD lock for the full handler call.
void handleApiList(WebServer& server, const Runtime& runtime);
void handleApiDownload(WebServer& server, const Runtime& runtime);

} // namespace WifiDiagnosticsApiService
