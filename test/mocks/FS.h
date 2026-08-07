#pragma once

#include "Arduino.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#ifndef FILE_READ
#define FILE_READ "r"
#endif

#ifndef FILE_WRITE
#define FILE_WRITE "w"
#endif

#ifndef FILE_APPEND
#define FILE_APPEND "a"
#endif

namespace fs {
class FS;
}

namespace mock_fs_detail {

inline constexpr size_t kUnlimitedWriteBudget = std::numeric_limits<size_t>::max();
inline size_t g_new_file_write_budget = kUnlimitedWriteBudget;

}  // namespace mock_fs_detail

enum SeekMode {
    SeekSet = 0,
    SeekCur = 1,
    SeekEnd = 2,
};

class File {
public:
    File() = default;

    explicit operator bool() const {
        return state_ && (state_->directory || state_->stream.is_open());
    }

    bool isDirectory() const {
        return state_ && state_->directory;
    }

    const char* name() const {
        return state_ ? state_->name.c_str() : "";
    }

    size_t size() const {
        if (!state_ || state_->directory) {
            return 0;
        }
        std::error_code ec;
        return std::filesystem::exists(state_->path, ec)
                   ? static_cast<size_t>(std::filesystem::file_size(state_->path, ec))
                   : 0u;
    }

    size_t write(const uint8_t* data, size_t length) {
        if (!state_ || state_->directory || !state_->writable || !state_->stream.is_open()) {
            return 0;
        }
        size_t bytesToWrite = length;
        if (state_->writeBudgetRemaining != mock_fs_detail::kUnlimitedWriteBudget) {
            if (state_->writeBudgetRemaining == 0) {
                return 0;
            }
            bytesToWrite = std::min(bytesToWrite, state_->writeBudgetRemaining);
        }
        state_->stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytesToWrite));
        if (!state_->stream.good()) {
            return 0;
        }
        if (state_->writeBudgetRemaining != mock_fs_detail::kUnlimitedWriteBudget) {
            state_->writeBudgetRemaining -= bytesToWrite;
        }
        return bytesToWrite;
    }

    size_t write(uint8_t byte) {
        return write(&byte, 1);
    }

    size_t read(uint8_t* buffer, size_t length) {
        if (!state_ || state_->directory || !state_->readable || !state_->stream.is_open()) {
            return 0;
        }
        state_->stream.read(reinterpret_cast<char*>(buffer), static_cast<std::streamsize>(length));
        return static_cast<size_t>(state_->stream.gcount());
    }

    int read() {
        uint8_t byte = 0;
        return read(&byte, 1) == 1 ? static_cast<int>(byte) : -1;
    }

    int peek() {
        if (!state_ || state_->directory || !state_->readable || !state_->stream.is_open()) {
            return -1;
        }
        return state_->stream.peek();
    }

    int available() {
        if (!state_ || state_->directory || !state_->readable || !state_->stream.is_open()) {
            return 0;
        }
        const std::streampos current = state_->stream.tellg();
        if (current < 0) {
            return 0;
        }
        state_->stream.seekg(0, std::ios::end);
        const std::streampos end = state_->stream.tellg();
        state_->stream.seekg(current);
        if (end < current) {
            return 0;
        }
        return static_cast<int>(end - current);
    }

    String readStringUntil(char terminator) {
        String result;
        while (available() > 0) {
            const int next = read();
            if (next < 0 || next == terminator) {
                break;
            }
            result += static_cast<char>(next);
        }
        return result;
    }

    bool seek(uint32_t pos, SeekMode mode = SeekSet) {
        if (!state_ || state_->directory || !state_->stream.is_open()) {
            return false;
        }

        std::ios_base::seekdir dir = std::ios::beg;
        if (mode == SeekCur) {
            dir = std::ios::cur;
        } else if (mode == SeekEnd) {
            dir = std::ios::end;
        }

        if (state_->readable) {
            state_->stream.seekg(static_cast<std::streamoff>(pos), dir);
        }
        if (state_->writable) {
            state_->stream.seekp(static_cast<std::streamoff>(pos), dir);
        }
        return state_->stream.good();
    }

    void flush() {
        if (state_ && !state_->directory && state_->stream.is_open()) {
            state_->stream.flush();
        }
    }

    size_t print(const char* str) {
        if (!str) return 0;
        const size_t len = std::strlen(str);
        return write(reinterpret_cast<const uint8_t*>(str), len);
    }

    size_t println(const char* str = "") {
        size_t n = print(str);
        n += print("\n");
        return n;
    }

    template<typename... Args>
    size_t printf(const char* fmt, Args... args) {
        char buf[256];
        int n = std::snprintf(buf, sizeof(buf), fmt, args...);
        if (n <= 0) return 0;
        return write(reinterpret_cast<const uint8_t*>(buf),
                     static_cast<size_t>(std::min(n, static_cast<int>(sizeof(buf) - 1))));
    }

    void close() {
        if (state_ && !state_->directory && state_->stream.is_open()) {
            state_->stream.close();
        }
    }

    File openNextFile() {
        if (!state_ || !state_->directory || state_->dirIndex >= state_->dirEntries.size()) {
            return File();
        }
        const std::filesystem::path next = state_->dirEntries[state_->dirIndex++];
        return openPath(next, FILE_READ);
    }

private:
    struct State {
        std::filesystem::path path;
        std::fstream stream;
        bool readable = false;
        bool writable = false;
        bool directory = false;
        std::string name;
        std::vector<std::filesystem::path> dirEntries;
        size_t dirIndex = 0;
        size_t writeBudgetRemaining = mock_fs_detail::kUnlimitedWriteBudget;
    };

    explicit File(std::shared_ptr<State> state) : state_(std::move(state)) {}

    static File openPath(const std::filesystem::path& path, const char* mode) {
        std::error_code ec;
        auto state = std::make_shared<State>();
        state->path = path;
        state->name = path.filename().string();

        if (std::filesystem::is_directory(path, ec)) {
            state->directory = true;
            for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
                state->dirEntries.push_back(entry.path());
            }
            std::sort(state->dirEntries.begin(), state->dirEntries.end());
            return File(state);
        }

        const std::string openMode = mode ? std::string(mode) : std::string(FILE_READ);
        const bool append = openMode.find('a') != std::string::npos;
        const bool truncate = openMode.find('w') != std::string::npos;
        state->readable = (openMode.find('r') != std::string::npos);
        state->writable = append || truncate;
        if (state->writable) {
            state->writeBudgetRemaining = mock_fs_detail::g_new_file_write_budget;
        }

        std::ios::openmode iosMode = std::ios::binary;
        if (state->readable) {
            iosMode |= std::ios::in;
        }
        if (state->writable) {
            iosMode |= std::ios::out;
        }
        if (truncate) {
            iosMode |= std::ios::trunc;
        }
        if (append) {
            iosMode |= std::ios::app;
        }

        std::filesystem::create_directories(path.parent_path(), ec);
        state->stream.open(path, iosMode);
        if (!state->stream.is_open() && state->writable && !truncate && !append) {
            state->stream.clear();
            state->stream.open(path, iosMode | std::ios::trunc);
        }
        if (!state->stream.is_open()) {
            return File();
        }
        return File(state);
    }

    std::shared_ptr<State> state_;

    friend class fs::FS;
};

namespace fs {

struct MockRenameState {
    size_t renameCalls = 0;
    size_t failOnCall = 0;
};

inline MockRenameState g_mock_fs_rename_state{};

inline void mock_reset_fs_rename_state() {
    g_mock_fs_rename_state = MockRenameState{};
}

inline void mock_fail_next_rename() {
    g_mock_fs_rename_state.failOnCall = g_mock_fs_rename_state.renameCalls + 1;
}

inline void mock_fail_rename_on_call(size_t callNumber) {
    g_mock_fs_rename_state.failOnCall = callNumber;
}

inline void mock_reset_fs_write_budget() {
    mock_fs_detail::g_new_file_write_budget = mock_fs_detail::kUnlimitedWriteBudget;
}

inline void mock_set_fs_write_budget(size_t bytes) {
    mock_fs_detail::g_new_file_write_budget = bytes;
}

class FS {
public:
    FS()
        : root_(std::filesystem::temp_directory_path() / "codex_fs_mock") {
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);
    }

    explicit FS(const std::filesystem::path& root)
        : root_(root) {
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);
    }

    File open(const char* path, const char* mode = FILE_READ, bool /*create*/ = false) {
        return File::openPath(resolve(path), mode);
    }

    File open(const String& path, const char* mode = FILE_READ, bool create = false) {
        return open(path.c_str(), mode, create);
    }

    bool exists(const char* path) {
        std::error_code ec;
        return std::filesystem::exists(resolve(path), ec);
    }

    bool exists(const String& path) {
        return exists(path.c_str());
    }

    bool remove(const char* path) {
        std::error_code ec;
        return std::filesystem::remove_all(resolve(path), ec) > 0;
    }

    bool remove(const String& path) {
        return remove(path.c_str());
    }

    bool rename(const char* from, const char* to) {
        g_mock_fs_rename_state.renameCalls++;
        if (g_mock_fs_rename_state.failOnCall != 0 &&
            g_mock_fs_rename_state.renameCalls == g_mock_fs_rename_state.failOnCall) {
            g_mock_fs_rename_state.failOnCall = 0;
            return false;
        }
        std::error_code ec;
        const std::filesystem::path target = resolve(to);
        std::filesystem::create_directories(target.parent_path(), ec);
        std::filesystem::rename(resolve(from), target, ec);
        return !ec;
    }

    bool rename(const String& from, const String& to) {
        return rename(from.c_str(), to.c_str());
    }

    bool mkdir(const char* path) {
        std::error_code ec;
        return std::filesystem::create_directories(resolve(path), ec) || !ec;
    }

    bool mkdir(const String& path) {
        return mkdir(path.c_str());
    }

private:
    std::filesystem::path resolve(const char* path) const {
        if (!path || path[0] == '\0') {
            return root_;
        }
        std::filesystem::path relative(path);
        if (relative.is_absolute()) {
            relative = relative.relative_path();
        }
        return root_ / relative;
    }

    std::filesystem::path root_;
};

}  // namespace fs
