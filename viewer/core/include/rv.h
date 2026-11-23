/*
 * rv.h: the C interface of the Visual Gridrouter core.
 *
 * Every client (Swift/AppKit now, C++/Win32 and C++/Qt6 later) talks to the
 * core through this header only.  It is plain C99 and also compiles as C++17.
 *
 * Rules of the interface:
 *   - One opaque session per open trace.  A session is single-threaded: call
 *     it from one thread at a time.  There are no callbacks.
 *   - Functions that can fail return an rv_status.  The message for the most
 *     recent failure on a session is available from rv_last_error().
 *   - No function aborts or lets a C++ exception escape.
 *   - Pointers returned by the core are borrowed: they stay valid until the
 *     next call that changes the same session, or until rv_destroy().
 *
 * This is milestone M0: only the session lifecycle exists.  Later milestones
 * add the trace reader, timeline, display list and wavefront images.
 */
#ifndef RV_H
#define RV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Version of this interface.  Bumped on every change to it.  A client
 * compares this compile-time value with rv_api_version() at run time to make
 * sure it was linked against the core it was compiled for.
 */
#define RV_API_VERSION 1u

/* Result of a core call.  The numeric values are part of the ABI. */
typedef enum rv_status {
    RV_OK = 0,
    RV_E_INVALID_ARGUMENT = 1, /* a NULL or out-of-range argument */
    RV_E_NO_MEMORY = 2,        /* an allocation failed */
    RV_E_INTERNAL = 3          /* any other failure inside the core */
} rv_status;

/* Opaque session handle. */
typedef struct rv_session rv_session;

/* The interface version the core library was built with (RV_API_VERSION). */
uint32_t rv_api_version(void);

/*
 * Create a session.  On success stores it in *out and returns RV_OK.  On
 * failure stores NULL in *out (when out is not NULL) and returns the reason.
 */
rv_status rv_create(rv_session **out);

/* Destroy a session.  NULL is accepted and ignored. */
void rv_destroy(rv_session *session);

/*
 * Message describing the most recent failure on this session, or "" if the
 * last call succeeded.  Never returns NULL; for a NULL session it returns a
 * static message.  The string is borrowed (see the rules above).
 */
const char *rv_last_error(const rv_session *session);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RV_H */
