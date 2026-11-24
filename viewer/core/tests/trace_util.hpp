// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// trace_util.hpp: helpers shared by the reader tests.
#ifndef RV_TESTS_TRACE_UTIL_HPP
#define RV_TESTS_TRACE_UTIL_HPP

#include "trace.hpp"

#include <memory>
#include <string>
#include <vector>

namespace rvtest {

// A reader over a copy of bytes, held in memory.
std::unique_ptr<rv::Trace> open_bytes(const std::vector<unsigned char> &bytes);

// A reader over a shared buffer that the caller may grow later.
std::unique_ptr<rv::Trace> open_shared(const std::shared_ptr<std::vector<unsigned char>> &bytes);

// Everything the reader derived, as text: chunks with their descriptions,
// issues, every table and derived table.  Two readers that read the same
// bytes, in one go or piece by piece, must give the same fingerprint.
std::string fingerprint(const rv::Trace &t);

// Structural invariants that must hold for any trace the reader accepted,
// however corrupt the bytes: every index in range, offsets increasing and
// inside the file.  Returns "" if they hold, else what failed.
std::string invariant_failure(const rv::Trace &t);

// Load every on-demand item (wavefronts, grids, boards) and describe every
// chunk.  Returns the number of loads that failed with FormatError (a clean
// error).  Any other exception propagates.
int exercise(rv::Trace &t);

// A fresh directory under the system temp directory, removed by the
// destructor.
class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
    std::string path(const std::string &name) const;
    bool ok() const { return !dir_.empty(); }

private:
    std::string dir_;
};

// Appends bytes to a file (creating it).  Returns false on failure.
bool append_file(const std::string &path, const unsigned char *data, std::size_t n);

} // namespace rvtest

#endif // RV_TESTS_TRACE_UTIL_HPP
