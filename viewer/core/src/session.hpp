// session.hpp: the C++ object behind an rv_session handle.
//
// Portable C++17, standard library only.  Later milestones add the
// timeline, camera and display list here; the C boundary in api.cpp stays a
// thin translation layer.
#ifndef RV_SESSION_HPP
#define RV_SESSION_HPP

#include "trace.hpp"

#include <memory>
#include <stdexcept>
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

    // --- the open trace -------------------------------------------------

    // Replace the open trace (on success only: a failed open throws before
    // anything changes).
    void open(const std::string &path) {
        std::unique_ptr<Trace> t = Trace::open_file(path);
        drop_borrowed();
        trace_ = std::move(t);
    }

    void close() noexcept {
        drop_borrowed();
        trace_.reset();
    }

    bool has_trace() const noexcept { return trace_ != nullptr; }

    // The open trace; throws std::invalid_argument if there is none.
    Trace &trace() {
        if (!trace_) throw std::invalid_argument("no trace is open in this session");
        return *trace_;
    }

    // Buffers that back borrowed pointers handed out through rv.h.
    std::string text;                              // rv_chunk_text
    std::shared_ptr<const Wavefront> wavefront;    // rv_wavefront

    void drop_borrowed() noexcept {
        text.clear();
        wavefront.reset();
    }

private:
    std::string last_error_;
    const char *fallback_ = nullptr;
    std::unique_ptr<Trace> trace_;
};

} // namespace rv

#endif // RV_SESSION_HPP
