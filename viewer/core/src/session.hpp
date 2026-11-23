// session.hpp: the C++ object behind an rv_session handle.
//
// Portable C++17, standard library only.  Later milestones add the trace
// reader, timeline, camera and display list here; the C boundary in api.cpp
// stays a thin translation layer.
#ifndef RV_SESSION_HPP
#define RV_SESSION_HPP

#include <string>

namespace rv {

class Session {
public:
    Session() = default;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // Message for the most recent failure, "" after a successful call.
    // Never null.
    const char *last_error() const noexcept {
        return fallback_ != nullptr ? fallback_ : last_error_.c_str();
    }

    // Record a failure.  Never throws: if the message cannot be stored, a
    // static message is reported instead.
    void set_error(const char *message) noexcept {
        try {
            last_error_.assign(message != nullptr ? message : "unknown error");
            fallback_ = nullptr;
        } catch (...) {
            last_error_.clear();
            fallback_ = "out of memory while recording an error";
        }
    }

    void clear_error() noexcept {
        last_error_.clear();
        fallback_ = nullptr;
    }

private:
    std::string last_error_;
    const char *fallback_ = nullptr;
};

} // namespace rv

#endif // RV_SESSION_HPP
