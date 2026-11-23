// errors.hpp: exception types used inside the core.
//
// They never cross the C boundary: api.cpp maps each one to an rv_status.
#ifndef RV_ERRORS_HPP
#define RV_ERRORS_HPP

#include <stdexcept>
#include <string>

namespace rv {

// The file could not be opened or read (maps to RV_E_IO).
struct IoError : std::runtime_error {
    explicit IoError(const std::string &what) : std::runtime_error(what) {}
};

// The bytes are not a trace file, or the file changed in a way an
// append-only trace cannot (maps to RV_E_FORMAT).
struct FormatError : std::runtime_error {
    explicit FormatError(const std::string &what) : std::runtime_error(what) {}
};

// A trace major version this reader does not know (maps to RV_E_UNSUPPORTED).
struct UnsupportedVersion : std::runtime_error {
    explicit UnsupportedVersion(const std::string &what) : std::runtime_error(what) {}
};

// One chunk's payload does not fit its own layout.  Caught per chunk by the
// reader, which records the chunk as corrupt and goes on; never escapes the
// reader.
struct CorruptChunk : std::runtime_error {
    explicit CorruptChunk(const std::string &what) : std::runtime_error(what) {}
};

} // namespace rv

#endif // RV_ERRORS_HPP
