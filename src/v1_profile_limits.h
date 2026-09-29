#pragma once

#include <cstddef>

inline constexpr size_t V1_PROFILE_DESCRIPTION_MAX_BYTES = 4096;
// A complete profile catalog is a transport-level object: USB export, HTTP/SD
// backup, and the restore rollback journal all promise that no profile is
// silently omitted. Ten maximal schema-v4 profiles leave measured headroom
// inside the 128 KiB USB/HTTP/journal envelope; every catalog mutation and
// import boundary enforces the same finite contract.
inline constexpr size_t V1_PROFILE_CATALOG_MAX_COUNT = 10;
// Round the measured worst supported pretty file (17,479 bytes, including the
// whole-profile integrity checksum) to the next 4 KiB storage quantum, then
// retain one complete quantum of schema headroom.
inline constexpr size_t V1_PROFILE_FILE_MAX_BYTES = 24u * 1024u;
// Native ArduinoJson coverage pins both maximum supported schema-v4 shapes:
// the canonical API profile is 12,768 bytes and a maximal accepted frontend
// save request is 12,772 bytes. The existing 16 KiB request limit leaves
// 3,612 bytes of headroom without admitting the global 128 KiB restore limit.
inline constexpr size_t V1_PROFILE_HTTP_SAVE_MAX_BYTES = 16u * 1024u;
inline constexpr size_t V1_PROFILE_HTTP_DELETE_MAX_BYTES = 2u * 1024u;
inline constexpr size_t V1_PROFILE_CATALOG_DOCUMENT_MAX_BYTES = 128u * 1024u;
inline constexpr const char* V1_PROFILE_FILE_LIMIT_ERROR =
    "Profile exceeds 24576-byte storage limit";
inline constexpr const char* V1_PROFILE_CATALOG_LIMIT_ERROR =
    "Profile catalog supports at most 10 profiles";
