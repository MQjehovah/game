#pragma once

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace test {
void ReportFailure(const char* file, int line, const char* expr);
} // namespace test

// Float tolerance check. Values are compared as doubles so int/float literals
// mix freely; fails with a readable diff when |a - b| exceeds eps.
#define CHECK_NEAR(a, b, eps)                                           \
    do {                                                                \
        double va = (a), vb = (b);                                      \
        if (std::fabs(va - vb) > (eps)) {                               \
            char buf[256];                                              \
            std::snprintf(buf, sizeof(buf), "%s ~= %s (%g vs %g)", #a, #b, va, vb); \
            test::ReportFailure(__FILE__, __LINE__, buf);               \
        }                                                               \
    } while (0)

// Asserts that the expression throws a std::exception-derived type. Any other
// throw escapes and crashes the process, which this harness treats as a
// controlled failure.
#define CHECK_THROW(...)                                                \
    do {                                                                \
        bool caught = false;                                            \
        try {                                                           \
            __VA_ARGS__;                                                \
        } catch (const std::exception&) {                               \
            caught = true;                                              \
        }                                                               \
        if (!caught) {                                                  \
            char buf[256];                                              \
            std::snprintf(buf, sizeof(buf), "expected exception: %s", #__VA_ARGS__); \
            test::ReportFailure(__FILE__, __LINE__, buf);               \
        }                                                               \
    } while (0)

namespace test {

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// RAII unique temporary directory, deleted (recursively) on destruction.
// Uses narrow Win32 APIs (GetTempPathA/CreateDirectoryA) because GCC 8's
// std::filesystem implementation is incomplete on MinGW.
class TempDir {
public:
    TempDir() {
        char base[MAX_PATH];
        DWORD n = GetTempPathA(MAX_PATH, base);
        if (n == 0 || n >= MAX_PATH)
            throw std::runtime_error("TempDir: GetTempPathA failed");
        for (int i = 0; i < 128; ++i) {
            char name[64];
            std::snprintf(name, sizeof(name), "neon_test_%lu_%lu",
                          static_cast<unsigned long>(GetCurrentProcessId()),
                          static_cast<unsigned long>(GetTickCount() + i));
            std::string dir = std::string(base) + name;
            if (CreateDirectoryA(dir.c_str(), nullptr)) {
                dir_ = dir;
                break;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS) break;
        }
        if (dir_.empty())
            throw std::runtime_error("TempDir: failed to create temporary directory");
    }

    ~TempDir() {
        if (!dir_.empty()) RemoveTree(dir_.c_str());
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& Str() const { return dir_; }

private:
    static void RemoveTree(const char* dir) {
        std::string pattern = std::string(dir) + "\\*";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] == '.' &&
                    (fd.cFileName[1] == '\0' ||
                     (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
                    continue;
                std::string full = std::string(dir) + "\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    RemoveTree(full.c_str());
                else
                    DeleteFileA(full.c_str());
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryA(dir);
    }

    std::string dir_;
};

#else // POSIX

#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// RAII unique temporary directory, deleted (recursively) on destruction.
class TempDir {
public:
    TempDir() {
        const char* base = std::getenv("TMPDIR");
        if (!base || !*base) base = "/tmp";
        for (int i = 0; i < 128; ++i) {
            std::string dir = std::string(base) + "/neon_test_" +
                              std::to_string(static_cast<long long>(::getpid())) +
                              "_" + std::to_string(i);
            if (::mkdir(dir.c_str(), 0700) == 0) {
                dir_ = dir;
                break;
            }
            if (errno != EEXIST) break;
        }
        if (dir_.empty())
            throw std::runtime_error("TempDir: failed to create temporary directory");
    }

    ~TempDir() {
        if (!dir_.empty()) RemoveTree(dir_.c_str());
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& Str() const { return dir_; }

private:
    static void RemoveTree(const char* dir) {
        DIR* d = ::opendir(dir);
        if (d) {
            struct dirent* e;
            while ((e = ::readdir(d)) != nullptr) {
                if (e->d_name[0] == '.' &&
                    (e->d_name[1] == '\0' ||
                     (e->d_name[1] == '.' && e->d_name[2] == '\0')))
                    continue;
                std::string full = std::string(dir) + "/" + e->d_name;
                struct stat st;
                if (::lstat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                    RemoveTree(full.c_str());
                else
                    ::unlink(full.c_str());
            }
            ::closedir(d);
        }
        ::rmdir(dir);
    }

    std::string dir_;
};

#endif

// In-memory file helpers: write a whole buffer / read a whole file.
// Paths are narrow strings to match the engine's const char* file APIs.

inline bool WriteFileAll(const std::string& path, const void* data, size_t size) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return out.good();
}

inline bool WriteFileAll(const std::string& path, const std::string& contents) {
    return WriteFileAll(path, contents.data(), contents.size());
}

inline bool ReadFileAll(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

inline bool ReadFileAll(const std::string& path, std::vector<char>& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    std::streamsize size = in.tellg();
    if (size < 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) in.read(out.data(), size);
    return !in.bad();
}

} // namespace test
