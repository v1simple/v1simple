#include "storage_json_rollback.h"

#include "storage_manager.h"

namespace {

bool loadJsonDocumentAtPath(fs::FS& fs,
                            const char* path,
                            size_t maxBytes,
                            JsonDocument& outDoc,
                            String* errorMessage) {
    if (errorMessage) {
        *errorMessage = "";
    }
    outDoc.clear();

    if (!path || path[0] == '\0' || !fs.exists(path)) {
        if (errorMessage) {
            *errorMessage = "missing";
        }
        return false;
    }

    File file = fs.open(path, FILE_READ);
    if (!file) {
        if (errorMessage) {
            *errorMessage = "open failed";
        }
        return false;
    }

    const size_t fileSize = file.size();
    if (fileSize == 0 || fileSize > maxBytes) {
        file.close();
        if (errorMessage) {
            *errorMessage = "size invalid";
        }
        return false;
    }

    const DeserializationError err = deserializeJson(outDoc, file);
    file.close();
    if (err) {
        if (errorMessage) {
            *errorMessage = err.c_str();
        }
        outDoc.clear();
        return false;
    }

    return true;
}

}  // namespace

JsonRollbackLoadResult loadJsonDocumentWithRollback(fs::FS& fs,
                                                    const char* livePath,
                                                    size_t maxBytes,
                                                    JsonDocument& outDoc,
                                                    String* errorMessage,
                                                    String* loadedPath) {
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
    const bool liveExists = fs.exists(livePath);
    const bool rollbackExists = rollbackPath.length() > 0 && fs.exists(rollbackPath.c_str());

    if (liveExists && loadJsonDocumentAtPath(fs, livePath, maxBytes, outDoc, errorMessage)) {
        if (loadedPath) {
            *loadedPath = livePath;
        }
        return JsonRollbackLoadResult::LoadedLive;
    }

    if (rollbackExists &&
        loadJsonDocumentAtPath(fs, rollbackPath.c_str(), maxBytes, outDoc, errorMessage)) {
        if (loadedPath) {
            *loadedPath = rollbackPath;
        }
        return JsonRollbackLoadResult::LoadedRollback;
    }

    return (liveExists || rollbackExists) ? JsonRollbackLoadResult::Invalid
                                          : JsonRollbackLoadResult::Missing;
}
