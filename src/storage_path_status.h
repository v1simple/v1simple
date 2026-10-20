#pragma once

#include <FS.h>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <sys/stat.h>

enum class StoragePathStatus { Present, Missing, Unavailable };

inline StoragePathStatus storagePathStatus(fs::FS& filesystem, const String& path) {
    // Arduino FS::exists() opens the file, so false also means an I/O or
    // allocation failure. Only an explicit ENOENT can authorize recovery.
    const char* mountpoint = filesystem.mountpoint();
    if (!mountpoint || mountpoint[0] != '/' || path.length() == 0 || path[0] != '/' ||
        std::strlen(path.c_str()) != path.length()) return StoragePathStatus::Unavailable;
    const size_t mountLength = std::strlen(mountpoint);
    const size_t pathLength = path.length();
    if (mountLength > std::numeric_limits<size_t>::max() - pathLength - 1u) {
        return StoragePathStatus::Unavailable;
    }
    std::unique_ptr<char[]> mountedPath(new (std::nothrow) char[mountLength + pathLength + 1u]);
    if (!mountedPath) return StoragePathStatus::Unavailable;
    std::memcpy(mountedPath.get(), mountpoint, mountLength);
    std::memcpy(mountedPath.get() + mountLength, path.c_str(), pathLength + 1u);
    struct stat info {};
    if (::stat(mountedPath.get(), &info) == 0) return StoragePathStatus::Present;
    const int error = errno;
    return error == ENOENT ? StoragePathStatus::Missing : StoragePathStatus::Unavailable;
}

// Cleanup may report success only after absence is known. A failed read-open
// must not hide a journal or credential sidecar that still exists.
inline bool storageRemoveFileIfPresent(fs::FS& filesystem, const String& path) {
    const StoragePathStatus status = storagePathStatus(filesystem, path);
    if (status == StoragePathStatus::Missing) return true;
    return status == StoragePathStatus::Present && filesystem.remove(path.c_str()) &&
           storagePathStatus(filesystem, path) == StoragePathStatus::Missing;
}
