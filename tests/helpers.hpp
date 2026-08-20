#pragma once

// All system includes live at the top of the file, before any namespace, so
// this header is safe to include from any translation unit on any platform.

#include <cerrno>
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

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// Minimal dependency-free test framework. Definitions are `inline` so every TU
// that includes this header shares the same registry/counter (C++17).
namespace test {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

inline int gFailures = 0;

inline void ReportFailure(const char* file, int line, const char* expr) {
    std::printf("  FAIL %s:%d: %s\n", file, line, expr);
    ++gFailures;
}

} // namespace test

// Register a test: TEST(name) { ... } defines the function and registers it.
#define TEST(name)                                                      \
    static void Test_##name();                                          \
    static test::Registrar Reg_##name(#name, &Test_##name);             \
    static void Test_##name()

// Boolean check: fails when cond is false.
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) test::ReportFailure(__FILE__, __LINE__, #cond);    \
    } while (0)

// Equality check: fails when the values differ.
#define CHECK_EQ(a, b)                                                  \
    do {                                                                \
        auto va = (a);                                                  \
        auto vb = (b);                                                  \
        if (!(va == vb)) {                                              \
            char buf[256];                                              \
            std::snprintf(buf, sizeof(buf), "%s == %s", #a, #b);        \
            test::ReportFailure(__FILE__, __LINE__, buf);               \
        }                                                               \
    } while (0)

// Float tolerance check. Values are compared as doubles so int/float literals
// mix freely; fails when |a - b| exceeds eps (|eps| is used, and NaN operands
// always fail).
#define CHECK_NEAR(a, b, eps)                                           \
    do {                                                                \
        double va = (a), vb = (b);                                      \
        double tol = std::fabs(eps);                                    \
        if (std::isnan(va) || std::isnan(vb) || std::fabs(va - vb) > tol) { \
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

// RAII unique temporary directory, deleted (recursively) on destruction.
// Uses platform APIs (narrow Win32 calls / POSIX mkdir) because GCC 8's
// std::filesystem implementation is incomplete on MinGW.

#if defined(_WIN32)

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
                DWORD attrs = fd.dwFileAttributes;
                // Read-only entries cannot be deleted; clear the attribute.
                if (attrs & FILE_ATTRIBUTE_READONLY)
                    SetFileAttributesA(full.c_str(),
                                       attrs & ~FILE_ATTRIBUTE_READONLY);
                if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
                    RemoveTree(full.c_str());
                } else if (!DeleteFileA(full.c_str())) {
                    std::fprintf(stderr,
                                 "TempDir cleanup warning: failed to delete %s "
                                 "(err=%lu)\n",
                                 full.c_str(), (unsigned long)GetLastError());
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        if (!RemoveDirectoryA(dir)) {
            std::fprintf(stderr,
                         "TempDir cleanup warning: failed to remove dir %s "
                         "(err=%lu)\n",
                         dir, (unsigned long)GetLastError());
        }
    }

    std::string dir_;
};

#else // POSIX

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
//
// NOTE: Paths are narrow strings. On Windows they are interpreted in the ANSI
// codepage, so UTF-8 file names may not round-trip. This matches the engine's
// const char* file API convention.

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
