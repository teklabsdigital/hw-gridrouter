// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// bytes.hpp: little-endian decoding and a bounds-checked payload cursor.
//
// Every scalar is assembled from bytes, so neither the host byte order nor
// the alignment of the buffer matters (TRACE_FORMAT.md 1.1).  Every read is
// checked against the end of the buffer first; a read that would run past it
// throws CorruptChunk.  Sizes are computed in 64 bits before any comparison,
// so a corrupt count cannot overflow the arithmetic.
#ifndef RV_BYTES_HPP
#define RV_BYTES_HPP

#include "errors.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace rv {

static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == 8,
              "the trace format stores IEEE 754 binary64 values");

inline std::uint16_t get_u16(const unsigned char *p) noexcept {
    return static_cast<std::uint16_t>(static_cast<unsigned>(p[0]) |
                                      (static_cast<unsigned>(p[1]) << 8));
}

inline std::uint32_t get_u32(const unsigned char *p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

inline std::uint64_t get_u64(const unsigned char *p) noexcept {
    return static_cast<std::uint64_t>(get_u32(p)) |
           (static_cast<std::uint64_t>(get_u32(p + 4)) << 32);
}

inline std::int32_t get_i32(const unsigned char *p) noexcept {
    const std::uint32_t u = get_u32(p);
    std::int32_t v;
    std::memcpy(&v, &u, sizeof v); // exact-width types are two's complement
    return v;
}

inline std::int64_t get_i64(const unsigned char *p) noexcept {
    const std::uint64_t u = get_u64(p);
    std::int64_t v;
    std::memcpy(&v, &u, sizeof v);
    return v;
}

inline double get_f64(const unsigned char *p) noexcept {
    const std::uint64_t u = get_u64(p);
    double v;
    std::memcpy(&v, &u, sizeof v);
    return v;
}

// Converts a 64-bit size to size_t, or throws CorruptChunk if it cannot be
// held in memory on this platform.
inline std::size_t to_size(std::uint64_t n, const char *what) {
    if (n > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        n > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()))
        throw CorruptChunk(std::string(what) + ": size too large");
    return static_cast<std::size_t>(n);
}

// Reads fields in order from a payload held in memory.  Offsets are relative
// to the start of the buffer, which is the start of the chunk payload (or of
// a BRD record body), so they are the payload offsets of the spec.
class Cursor {
public:
    Cursor(const unsigned char *data, std::size_t size, const char *context) noexcept
        : data_(data), size_(size), context_(context) {}

    std::size_t offset() const noexcept { return off_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t remaining() const noexcept { return size_ - off_; }
    const unsigned char *here() const noexcept { return data_ + off_; }

    // Throws unless n more bytes are present.
    void need(std::uint64_t n, const char *what) const {
        if (n > remaining()) fail(what, "runs past the end of the payload");
    }

    // Throws unless count records of at least min_bytes each could fit in
    // what remains.  Call before reserving memory for count records.
    void need_records(std::uint64_t count, std::uint64_t min_bytes, const char *what) const {
        // count < 2^32 and min_bytes is small, so the product cannot overflow.
        if (count > 0xFFFFFFFFull || min_bytes > 0xFFFFull ||
            count * min_bytes > remaining())
            fail(what, "count is larger than the payload can hold");
    }

    void skip(std::uint64_t n, const char *what) {
        need(n, what);
        off_ += static_cast<std::size_t>(n);
    }

    // align8 of the spec: 0 to 7 bytes (in practice 0 or 4) so that the
    // offset becomes a multiple of 8.
    void align8(const char *what) {
        const std::size_t pad = (8 - off_ % 8) % 8;
        skip(pad, what);
    }

    std::uint32_t u32(const char *what) {
        need(4, what);
        const std::uint32_t v = get_u32(data_ + off_);
        off_ += 4;
        return v;
    }
    std::int32_t i32(const char *what) {
        need(4, what);
        const std::int32_t v = get_i32(data_ + off_);
        off_ += 4;
        return v;
    }
    std::int64_t i64(const char *what) {
        need(8, what);
        const std::int64_t v = get_i64(data_ + off_);
        off_ += 8;
        return v;
    }
    double f64(const char *what) {
        need(8, what);
        const double v = get_f64(data_ + off_);
        off_ += 8;
        return v;
    }

    // str: u32 n, n bytes, zero padding to a multiple of 4 (1.3).  The
    // padding must be present: it is part of the str's size.
    std::string str(const char *what) {
        const std::uint64_t n = u32(what);
        const std::uint64_t padded = (n + 3) & ~static_cast<std::uint64_t>(3);
        need(padded, what);
        std::string s(reinterpret_cast<const char *>(data_ + off_), static_cast<std::size_t>(n));
        off_ += static_cast<std::size_t>(padded);
        return s;
    }

    void u32_array(std::vector<std::uint32_t> &out, std::uint64_t count, const char *what) {
        need_records(count, 4, what);
        const std::size_t n = static_cast<std::size_t>(count);
        out.resize(n);
        for (std::size_t k = 0; k < n; ++k) out[k] = get_u32(data_ + off_ + 4 * k);
        off_ += 4 * n;
    }
    void i32_array(std::vector<std::int32_t> &out, std::uint64_t count, const char *what) {
        need_records(count, 4, what);
        const std::size_t n = static_cast<std::size_t>(count);
        out.resize(n);
        for (std::size_t k = 0; k < n; ++k) out[k] = get_i32(data_ + off_ + 4 * k);
        off_ += 4 * n;
    }

    [[noreturn]] void fail(const char *what, const char *problem) const {
        throw CorruptChunk(std::string(context_) + " " + what + ": " + problem);
    }

private:
    const unsigned char *data_;
    std::size_t size_;
    std::size_t off_ = 0;
    const char *context_;
};

} // namespace rv

#endif // RV_BYTES_HPP
