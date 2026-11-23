// file.cpp: FileSource, the only platform-specific code in the core.
//
// POSIX (macOS, Linux): open + fstat + pread, 64-bit offsets; replaced()
// compares stat(path) with fstat of the open descriptor (device and inode).
// Windows: CreateFileW (UTF-8 path converted to UTF-16) + GetFileSizeEx +
// ReadFile with an OVERLAPPED offset; replaced() compares the volume serial
// number and file index of the path with those of the open handle.
// A source opened to wait (M6) opens its path on the first size() call that
// finds a file there.
//
// This file holds the core's single `#ifdef _WIN32` (viewer/CLAUDE.md rule 3).
// The Windows branch has NOT been compiled or run: no Windows toolchain is
// available yet.  It is written against the documented Win32 API and must be
// built and tested when the Windows client starts.

// 64-bit off_t on 32-bit POSIX systems.  Must precede every system header.
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "source.hpp"

#include "errors.hpp"

#include <cstdint>
#include <limits>
#include <string>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rv {
namespace {

std::string win_error(const char *what, const std::string &path) {
    return std::string(what) + " " + path + ": Windows error " +
           std::to_string(static_cast<unsigned long>(GetLastError()));
}

// UTF-8 to UTF-16 for the wide file API.  Invalid UTF-8 is an error rather
// than being replaced, so a wrong path is never opened silently.
std::wstring widen(const std::string &utf8) {
    if (utf8.empty()) throw IoError("cannot open a file with an empty path");
    if (utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw IoError("path is too long");
    const int in_len = static_cast<int>(utf8.size());
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), in_len,
                                      nullptr, 0);
    if (n <= 0) throw IoError("path is not valid UTF-8: " + utf8);
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), in_len, &wide[0], n) != n)
        throw IoError("path is not valid UTF-8: " + utf8);
    return wide;
}

HANDLE as_handle(std::intptr_t h) { return reinterpret_cast<HANDLE>(h); }

} // namespace

FileSource::FileSource(const std::string &utf8_path, bool wait) : path_(utf8_path), handle_(0) {
    try_open(wait);
}

bool FileSource::try_open(bool missing_ok) {
    const std::wstring wide = widen(path_);
    // Share read, write and delete: the recorder may still be appending.
    HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (missing_ok && (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)) return false;
        throw IoError(win_error("cannot open", path_));
    }
    if (GetFileType(h) != FILE_TYPE_DISK) {
        CloseHandle(h);
        throw IoError("not a regular file: " + path_);
    }
    handle_ = reinterpret_cast<std::intptr_t>(h);
    open_ = true;
    return true;
}

bool FileSource::replaced() {
    if (!open_) return false;
    const std::wstring wide = widen(path_);
    HANDLE h = CreateFileW(wide.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return true;
        throw IoError(win_error("cannot open", path_));
    }
    BY_HANDLE_FILE_INFORMATION a, b;
    const bool ok = GetFileInformationByHandle(h, &a) && GetFileInformationByHandle(as_handle(handle_), &b);
    CloseHandle(h);
    if (!ok) throw IoError(win_error("cannot identify", path_));
    return a.dwVolumeSerialNumber != b.dwVolumeSerialNumber || a.nFileIndexHigh != b.nFileIndexHigh ||
           a.nFileIndexLow != b.nFileIndexLow;
}

FileSource::~FileSource() {
    if (open_) CloseHandle(as_handle(handle_));
}

std::uint64_t FileSource::size() {
    if (!open_ && !try_open(true)) return 0;
    LARGE_INTEGER s;
    if (!GetFileSizeEx(as_handle(handle_), &s)) throw IoError(win_error("cannot get the size of", path_));
    if (s.QuadPart < 0) throw IoError("negative file size: " + path_);
    return static_cast<std::uint64_t>(s.QuadPart);
}

std::size_t FileSource::read_at(std::uint64_t offset, void *buf, std::size_t n) {
    if (!open_) return 0;
    unsigned char *out = static_cast<unsigned char *>(buf);
    std::size_t done = 0;
    while (done < n) {
        const std::size_t want = n - done;
        const DWORD part = want > 0x40000000u ? 0x40000000u : static_cast<DWORD>(want);
        const std::uint64_t at = offset + done;
        OVERLAPPED ov = {};
        ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;
        if (!ReadFile(as_handle(handle_), out + done, part, &got, &ov)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            throw IoError(win_error("cannot read", path_));
        }
        if (got == 0) break;
        done += got;
    }
    return done;
}

} // namespace rv

#else // POSIX

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace rv {
namespace {

std::string posix_error(const char *what, const std::string &path, int err) {
    return std::string(what) + " " + path + ": " + std::strerror(err);
}

int as_fd(std::intptr_t h) { return static_cast<int>(h); }

} // namespace

FileSource::FileSource(const std::string &utf8_path, bool wait) : path_(utf8_path), handle_(-1) {
    if (utf8_path.empty()) throw IoError("cannot open a file with an empty path");
    try_open(wait);
}

bool FileSource::try_open(bool missing_ok) {
    int fd;
    do {
        fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        if (missing_ok && errno == ENOENT) return false;
        throw IoError(posix_error("cannot open", path_, errno));
    }
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        const int err = errno;
        ::close(fd);
        throw IoError(posix_error("cannot stat", path_, err));
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(fd);
        throw IoError("not a regular file: " + path_);
    }
    handle_ = fd;
    open_ = true;
    return true;
}

FileSource::~FileSource() {
    if (open_) ::close(as_fd(handle_));
}

std::uint64_t FileSource::size() {
    if (!open_ && !try_open(true)) return 0;
    struct stat st;
    if (::fstat(as_fd(handle_), &st) != 0) throw IoError(posix_error("cannot stat", path_, errno));
    if (st.st_size < 0) throw IoError("negative file size: " + path_);
    return static_cast<std::uint64_t>(st.st_size);
}

bool FileSource::replaced() {
    if (!open_) return false;
    struct stat now, mine;
    if (::stat(path_.c_str(), &now) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) return true;
        throw IoError(posix_error("cannot stat", path_, errno));
    }
    if (::fstat(as_fd(handle_), &mine) != 0) throw IoError(posix_error("cannot stat", path_, errno));
    return now.st_dev != mine.st_dev || now.st_ino != mine.st_ino;
}

std::size_t FileSource::read_at(std::uint64_t offset, void *buf, std::size_t n) {
    if (!open_) return 0;
    unsigned char *out = static_cast<unsigned char *>(buf);
    std::size_t done = 0;
    while (done < n) {
        const std::uint64_t at = offset + done;
        if (at > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
            throw IoError("read offset out of range in " + path_);
        std::size_t want = n - done;
        if (want > 0x40000000u) want = 0x40000000u; // keep each call well inside ssize_t
        const ssize_t got = ::pread(as_fd(handle_), out + done, want, static_cast<off_t>(at));
        if (got < 0) {
            if (errno == EINTR) continue;
            throw IoError(posix_error("cannot read", path_, errno));
        }
        if (got == 0) break;
        done += static_cast<std::size_t>(got);
    }
    return done;
}

} // namespace rv

#endif
