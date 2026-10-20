#pragma once

#include <FS.h>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <memory>
#include <new>

#ifdef UNIT_TEST
namespace StorageDirectoryTest {
inline size_t failAfterEntries = std::numeric_limits<size_t>::max();
inline size_t failOnOpen = 0;
inline void failRead(size_t entries, size_t openOccurrence = 1) {
    failAfterEntries = entries;
    failOnOpen = openOccurrence;
}
inline void reset() {
    failAfterEntries = std::numeric_limits<size_t>::max();
    failOnOpen = 0;
}
}  // namespace StorageDirectoryTest
#endif

// Arduino File::openNextFile() opens each child and returns the same false File
// for child-open failure and EOF. A catalog must only become authoritative
// after readdir reaches clean EOF, without opening children to obtain names.
class StorageDirectory {
public:
    StorageDirectory(fs::FS& filesystem, const String& path) {
        const char* mountpoint = filesystem.mountpoint();
        if (!mountpoint || mountpoint[0] != '/' || path.length() == 0 || path[0] != '/' ||
            std::strlen(path.c_str()) != path.length()) return;
        const size_t mountLength = std::strlen(mountpoint);
        const size_t pathLength = path.length();
        if (mountLength > std::numeric_limits<size_t>::max() - pathLength - 1u) return;
        std::unique_ptr<char[]> mountedPath(new (std::nothrow) char[mountLength + pathLength + 1u]);
        if (!mountedPath) return;
        std::memcpy(mountedPath.get(), mountpoint, mountLength);
        std::memcpy(mountedPath.get() + mountLength, path.c_str(), pathLength + 1u);
        directory_ = ::opendir(mountedPath.get());
#ifdef UNIT_TEST
        if (directory_ && StorageDirectoryTest::failOnOpen != 0 &&
            --StorageDirectoryTest::failOnOpen == 0) {
            failAfterEntries_ = StorageDirectoryTest::failAfterEntries;
        }
#endif
    }

    ~StorageDirectory() { close(); }
    StorageDirectory(const StorageDirectory&) = delete;
    StorageDirectory& operator=(const StorageDirectory&) = delete;

    explicit operator bool() const { return directory_ != nullptr; }

    const char* next() {
        if (!directory_ || finished_) return nullptr;
        while (true) {
            errno = 0;
            struct dirent* entry = nullptr;
#ifdef UNIT_TEST
            if (returnedEntries_ == failAfterEntries_) errno = EIO;
            else
#endif
                entry = ::readdir(directory_);
            if (!entry) {
                complete_ = errno == 0;
                finished_ = true;
                return nullptr;
            }
            if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
#ifdef UNIT_TEST
            ++returnedEntries_;
#endif
            return entry->d_name;
        }
    }

    bool complete() const { return complete_; }
    void close() {
        if (directory_) ::closedir(directory_);
        directory_ = nullptr;
    }

private:
    DIR* directory_ = nullptr;
    bool finished_ = false;
    bool complete_ = false;
#ifdef UNIT_TEST
    size_t returnedEntries_ = 0;
    size_t failAfterEntries_ = std::numeric_limits<size_t>::max();
#endif
};
