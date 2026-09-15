// build_metadata.cpp — Sole consumer of the GIT_SHA build flag.
// Isolating this macro here means only this tiny file recompiles on
// each new commit, keeping the rest of the incremental build cached.

#include "build_metadata.h"

#ifndef UNIT_TEST
#include <esp_app_desc.h>
#endif

#ifndef GIT_SHA
#define GIT_SHA "unknown"
#endif

const char* getBuildGitSha() {
    return GIT_SHA;
}

const char* getRuntimeImageId() {
#ifdef UNIT_TEST
    return "unit-test-image";
#else
    static char imageSha256[65] = {0};
    if (imageSha256[0] == '\0') {
        const int written = esp_app_get_elf_sha256(imageSha256, sizeof(imageSha256));
        if (written <= 1) {
            return "unknown";
        }
    }
    return imageSha256;
#endif
}
