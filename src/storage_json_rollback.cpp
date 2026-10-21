#include "storage_json_rollback.h"

#include "json_exact_input.h"
#include "psram_json_document.h"
#include "storage_manager.h"
#include "storage_path_status.h"

namespace {

enum class JsonPathLoadResult : uint8_t {
    Loaded,
    Invalid,
    Unavailable,
};

JsonPathLoadResult loadJsonDocumentAtPath(fs::FS& fs, const char* path, size_t maxBytes,
                                          const struct stat& details, JsonDocument& outDoc,
                                          String* errorMessage) {
    if (errorMessage) {
        *errorMessage = "";
    }
    outDoc.clear();

    if (!path || path[0] == '\0') {
        if (errorMessage) {
            *errorMessage = "missing";
        }
        return JsonPathLoadResult::Invalid;
    }

    File file = fs.open(path, FILE_READ);
    if (!file) {
        if (errorMessage) {
            *errorMessage = "open failed";
        }
        return JsonPathLoadResult::Unavailable;
    }

    // VFS may leave File::size() at zero when its internal fstat fails.
    // The successful path-status probe supplies checked metadata instead.
    if (details.st_size <= 0 || static_cast<uint64_t>(details.st_size) > maxBytes) {
        file.close();
        if (errorMessage) {
            *errorMessage = "size invalid";
        }
        return JsonPathLoadResult::Invalid;
    }
    const size_t fileSize = static_cast<size_t>(details.st_size);

    PsramJson::Buffer bytes(fileSize);
    if (!bytes || file.read(bytes.data(), fileSize) != fileSize) {
        file.close();
        if (errorMessage) *errorMessage = "memory/read unavailable";
        return JsonPathLoadResult::Unavailable;
    }
    file.close();
    const ExactJsonInput::Status exact = ExactJsonInput::validate(bytes.data(), fileSize);
    if (exact != ExactJsonInput::Status::Ok) {
        if (errorMessage) *errorMessage = exact == ExactJsonInput::Status::MemoryUnavailable
                                              ? "exact validation memory unavailable"
                                              : "not one exact unique-key UTF-8 document";
        return exact == ExactJsonInput::Status::MemoryUnavailable ? JsonPathLoadResult::Unavailable
                                                                  : JsonPathLoadResult::Invalid;
    }
    const DeserializationError err = deserializeJson(outDoc, bytes.data(), fileSize);
    if (err) {
        if (errorMessage) {
            *errorMessage = err.c_str();
        }
        outDoc.clear();
        return err == DeserializationError::NoMemory ? JsonPathLoadResult::Unavailable
                                                     : JsonPathLoadResult::Invalid;
    }

    return outDoc.overflowed() ? JsonPathLoadResult::Unavailable : JsonPathLoadResult::Loaded;
}

} // namespace

JsonRollbackLoadResult loadJsonDocumentWithRollback(fs::FS& fs, const char* livePath, size_t maxBytes,
                                                    JsonDocument& outDoc, String* errorMessage, String* loadedPath) {
    if (loadedPath) {
        *loadedPath = "";
    }
    if (errorMessage) {
        *errorMessage = "";
    }

    if (!livePath || livePath[0] == '\0') {
        return JsonRollbackLoadResult::Invalid;
    }

    const String rollbackPath = StorageManager::rollbackPathFor(livePath);
    struct stat liveDetails {};
    const StoragePathStatus liveStatus = storagePathStatus(fs, livePath, &liveDetails);
    if (liveStatus == StoragePathStatus::Unavailable) return JsonRollbackLoadResult::Unavailable;
    const bool liveExists = liveStatus == StoragePathStatus::Present;

    const JsonPathLoadResult liveResult = liveExists
                                              ? loadJsonDocumentAtPath(fs, livePath, maxBytes, liveDetails,
                                                                       outDoc, errorMessage)
                                              : JsonPathLoadResult::Invalid;
    if (liveResult == JsonPathLoadResult::Loaded) {
        if (loadedPath) {
            *loadedPath = livePath;
        }
        return JsonRollbackLoadResult::LoadedLive;
    }
    // A transient inability to read the authoritative live candidate is not
    // evidence that an older rollback is preferable. Abort before inspecting
    // or promoting stale state.
    if (liveResult == JsonPathLoadResult::Unavailable) {
        outDoc.clear();
        return JsonRollbackLoadResult::Unavailable;
    }

    struct stat rollbackDetails {};
    const StoragePathStatus rollbackStatus = storagePathStatus(fs, rollbackPath, &rollbackDetails);
    if (rollbackStatus == StoragePathStatus::Unavailable) return JsonRollbackLoadResult::Unavailable;
    const bool rollbackExists = rollbackStatus == StoragePathStatus::Present;
    const JsonPathLoadResult rollbackResult = rollbackExists
                                                  ? loadJsonDocumentAtPath(fs, rollbackPath.c_str(), maxBytes, rollbackDetails,
                                                                           outDoc, errorMessage)
                                                  : JsonPathLoadResult::Invalid;
    if (rollbackResult == JsonPathLoadResult::Unavailable) {
        outDoc.clear();
        return JsonRollbackLoadResult::Unavailable;
    }
    if (rollbackResult == JsonPathLoadResult::Loaded) {
        if (loadedPath) {
            *loadedPath = rollbackPath;
        }
        return JsonRollbackLoadResult::LoadedRollback;
    }

    return (liveExists || rollbackExists) ? JsonRollbackLoadResult::Invalid : JsonRollbackLoadResult::Missing;
}
