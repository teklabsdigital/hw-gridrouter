// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// source.hpp: where the reader gets its bytes from.
//
// A Source is a growing, append-only sequence of bytes read by position: a
// trace file on disk (FileSource, file.cpp) or a buffer in memory
// (MemorySource, used by tests and by tools that build traces in memory).
// The reader never loads a whole file; it asks for the ranges it needs.
#ifndef RV_SOURCE_HPP
#define RV_SOURCE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace rv {

class Source {
public:
    virtual ~Source() = default;

    // Current size in bytes.  Throws IoError if it cannot be determined.
    virtual std::uint64_t size() = 0;

    // Reads up to n bytes at offset into buf.  Returns the number read,
    // which is less than n only at the end of the data.  Throws IoError on a
    // read error.
    virtual std::size_t read_at(std::uint64_t offset, void *buf, std::size_t n) = 0;

    // M6.  Is the source still the one that was opened?  A file is not when
    // its path now names another file, or no file (it was renamed away or
    // deleted).  Throws IoError if that cannot be determined.
    virtual bool replaced() { return false; }
    // M6.  True while a source opened to wait for its file has none yet
    // (size() is then 0).
    virtual bool waiting() const { return false; }
};

// A trace file on disk, read with 64-bit positioned reads.  The file is
// opened read-only and shared, so a writer can keep appending to it.
class FileSource final : public Source {
public:
    // Opens the file at path (UTF-8).  Throws IoError.  With wait = true a
    // path that does not exist yet is not an error: the source is empty and
    // waiting() until a file appears there (size() opens it).
    explicit FileSource(const std::string &utf8_path, bool wait = false);
    ~FileSource() override;
    FileSource(const FileSource &) = delete;
    FileSource &operator=(const FileSource &) = delete;

    std::uint64_t size() override;
    std::size_t read_at(std::uint64_t offset, void *buf, std::size_t n) override;
    bool replaced() override;
    bool waiting() const override { return !open_; }

private:
    bool try_open(bool missing_ok); // false: the path does not exist (missing_ok only)

    std::string path_;      // for messages
    std::intptr_t handle_;  // POSIX file descriptor or Windows HANDLE
    bool open_ = false;
};

// Bytes in memory.  The buffer is shared so that a test can append to it
// after handing the source to a reader, imitating a file being written.
class MemorySource final : public Source {
public:
    explicit MemorySource(std::shared_ptr<std::vector<unsigned char>> data)
        : data_(std::move(data)) {}

    std::uint64_t size() override { return data_ ? data_->size() : 0; }

    std::size_t read_at(std::uint64_t offset, void *buf, std::size_t n) override {
        if (!data_ || offset >= data_->size()) return 0;
        const std::size_t avail = data_->size() - static_cast<std::size_t>(offset);
        const std::size_t k = std::min(n, avail);
        if (k > 0) std::memcpy(buf, data_->data() + offset, k);
        return k;
    }

private:
    std::shared_ptr<std::vector<unsigned char>> data_;
};

} // namespace rv

#endif // RV_SOURCE_HPP
