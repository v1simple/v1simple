#pragma once

const char* getBuildGitSha();

// SHA-256 of the ELF embedded in and reported by the currently running image.
// Unlike the Git build flag, this identifies the resident binary even for a
// dirty build or when a bench run skips upload.
const char* getRuntimeImageId();
