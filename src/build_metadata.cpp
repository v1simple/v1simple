// build_metadata.cpp — Sole consumer of the GIT_SHA build flag.
// Isolating this macro here means only this tiny file recompiles on
// each new commit, keeping the rest of the incremental build cached.

#ifndef GIT_SHA
#define GIT_SHA "unknown"
#endif

const char* getBuildGitSha() {
    return GIT_SHA;
}
