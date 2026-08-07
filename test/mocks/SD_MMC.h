#pragma once

#include "FS.h"

class SDMMCClass : public fs::FS {
public:
    SDMMCClass()
        : fs::FS(std::filesystem::temp_directory_path() / "codex_sd_mmc_mock") {}

    bool setPins(int, int, int) {
        return pinsOk;
    }

    bool begin(const char* = "/sdcard", bool = true) {
        return beginOk;
    }

    uint64_t cardSize() const {
        return cardSizeBytes;
    }

    bool pinsOk = true;
    bool beginOk = false;
    uint64_t cardSizeBytes = 16ull * 1024ull * 1024ull;
};

inline SDMMCClass SD_MMC;
