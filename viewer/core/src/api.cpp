// api.cpp: the C boundary of the core.
//
// Each entry point translates between the C types of rv.h and the C++
// objects behind them.  No C++ exception crosses this boundary: every entry
// point catches everything and maps it to an rv_status, recording the
// message on the session when there is one.

#include "rv.h"

#include "session.hpp"

#include <exception>
#include <new>
#include <stdexcept>

struct rv_session {
    rv::Session impl;
};

namespace {

// Map the exception being handled to a status and record its message.
// Call only from inside a catch block.
rv_status status_from_current_exception(rv::Session *session) noexcept {
    rv_status status = RV_E_INTERNAL;
    const char *message = "unknown internal error";
    try {
        throw;
    } catch (const std::bad_alloc &) {
        status = RV_E_NO_MEMORY;
        message = "out of memory";
    } catch (const std::invalid_argument &e) {
        status = RV_E_INVALID_ARGUMENT;
        message = e.what();
    } catch (const std::exception &e) {
        message = e.what();
    } catch (...) {
    }
    if (session != nullptr) session->set_error(message);
    return status;
}

const char kNullSession[] = "invalid argument: session is NULL";

} // namespace

extern "C" {

uint32_t rv_api_version(void) {
    return RV_API_VERSION;
}

rv_status rv_create(rv_session **out) {
    if (out == nullptr) return RV_E_INVALID_ARGUMENT;
    *out = nullptr;
    try {
        *out = new rv_session();
        return RV_OK;
    } catch (...) {
        // No session exists yet, so the status is the only report.
        return status_from_current_exception(nullptr);
    }
}

void rv_destroy(rv_session *session) {
    try {
        delete session;
    } catch (...) {
        // Destructors do not throw; this only keeps the boundary rule.
    }
}

const char *rv_last_error(const rv_session *session) {
    if (session == nullptr) return kNullSession;
    return session->impl.last_error();
}

} // extern "C"
