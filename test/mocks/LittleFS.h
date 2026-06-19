#pragma once

#include "FS.h"

#include <string>

class LittleFSClass : public fs::FS {
public:
    static inline int beginCallCount = 0;
    static inline bool lastFormatOnFail = true;
    static inline std::string lastBasePath;
    static inline uint8_t lastMaxOpenFiles = 0;
    static inline std::string lastPartitionLabel;
    static inline bool beginReturnValue = true;

    LittleFSClass()
        : fs::FS(std::filesystem::temp_directory_path() / "codex_littlefs_mock") {}

    static void resetBeginRecording() {
        beginCallCount = 0;
        lastFormatOnFail = true;
        lastBasePath.clear();
        lastMaxOpenFiles = 0;
        lastPartitionLabel.clear();
        beginReturnValue = true;
    }

    bool begin(bool formatOnFail = false,
               const char* basePath = "/littlefs",
               uint8_t maxOpenFiles = 10,
               const char* partitionLabel = "storage") {
        beginCallCount++;
        lastFormatOnFail = formatOnFail;
        lastBasePath = basePath ? basePath : "";
        lastMaxOpenFiles = maxOpenFiles;
        lastPartitionLabel = partitionLabel ? partitionLabel : "";
        return beginReturnValue;
    }

    void end() {}
};

inline LittleFSClass LittleFS;
