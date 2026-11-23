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
 *     next call that changes the same session, or until rv_destroy().  The
 *     calls that change a session are rv_open, rv_close, rv_refresh, and the
 *     calls that return a borrowed buffer (rv_chunk_text, rv_wavefront),
 *     each of which replaces the buffer it returned before.
 *
 * Milestone M1: the session lifecycle and the trace reader (open, close,
 * refresh and read-only facts about the file).  Later milestones add the
 * timeline, display list and wavefront images.
 *
 * The trace file format is docs/TRACE_FORMAT.md in the router repository.
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
#define RV_API_VERSION 2u

/* "No id": an absent search, route, copper or session index. */
#define RV_NONE 0xFFFFFFFFu

/* Result of a core call.  The numeric values are part of the ABI. */
typedef enum rv_status {
    RV_OK = 0,
    RV_E_INVALID_ARGUMENT = 1, /* a NULL or out-of-range argument, or no trace open */
    RV_E_NO_MEMORY = 2,        /* an allocation failed */
    RV_E_INTERNAL = 3,         /* any other failure inside the core */
    RV_E_IO = 4,               /* the file could not be opened or read */
    RV_E_FORMAT = 5,           /* not a trace file, or it shrank or was rewritten */
    RV_E_UNSUPPORTED = 6       /* a trace format major version this core cannot read */
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

/* ---- opening a trace -------------------------------------------------- */

/*
 * Open the trace file at path (UTF-8) in this session and read what it
 * holds now.  On success it replaces any trace already open in the session;
 * on failure the session is unchanged.  A file that is still being written
 * is fine: whatever complete chunks exist are read, and rv_refresh() picks
 * up the rest.  Fewer than 24 bytes that agree with the start of a header
 * open as an empty trace whose header is not written yet.
 * Errors: RV_E_IO, RV_E_FORMAT (not a trace), RV_E_UNSUPPORTED (version).
 */
rv_status rv_open(rv_session *session, const char *path);

/* Close the open trace, if any.  RV_OK also when none is open. */
rv_status rv_close(rv_session *session);

/*
 * Read chunks appended to the file since the last open or refresh.  Stores
 * the number of new chunks in *new_chunks (if not NULL).  If the file shrank
 * or its header changed, the trace enters a failed state: this and every
 * later refresh return RV_E_FORMAT, and the facts already read stay
 * available.  Close and reopen the file to read it again.
 */
rv_status rv_refresh(rv_session *session, uint32_t *new_chunks);

/* ---- file-level facts --------------------------------------------------- */

/* Chunk kinds.  RV_CHUNK_UNKNOWN is any tag this core does not know. */
typedef enum rv_chunk_kind {
    RV_CHUNK_UNKNOWN = 0,
    RV_CHUNK_RUN = 1,
    RV_CHUNK_NETS = 2,
    RV_CHUNK_PAD = 3,
    RV_CHUNK_RECT = 4,
    RV_CHUNK_DISC = 5,
    RV_CHUNK_VIA = 6,
    RV_CHUNK_SNAP = 7,
    RV_CHUNK_GRDS = 8,
    RV_CHUNK_RNB = 9,
    RV_CHUNK_RNE = 10,
    RV_CHUNK_SRCH = 11,
    RV_CHUNK_CMIT = 12,
    RV_CHUNK_RBLD = 13,
    RV_CHUNK_RIP = 14,
    RV_CHUNK_DROP = 15,
    RV_CHUNK_MARK = 16,
    RV_CHUNK_ROUT = 17,
    RV_CHUNK_BRD = 18,
    RV_CHUNK_END = 19,
    RV_CHUNK_KIND_COUNT = 20
} rv_chunk_kind;

/* The four-character tag of a kind ("RUN ", "SRCH", ...), "????" for
 * RV_CHUNK_UNKNOWN or an out-of-range value.  Static; never NULL. */
const char *rv_kind_tag(uint32_t kind);

typedef struct rv_trace_info {
    double open_time;        /* header open_time, Unix seconds */
    double last_time;        /* t of the last chunk read, seconds; 0 if none */
    uint64_t file_size;      /* bytes, at the last open or refresh */
    uint64_t indexed_end;    /* offset just past the last complete chunk read */
    int64_t expanded_total;  /* sum of SRCH expanded (>= 0 only) */
    int64_t trace_entries;   /* sum of SRCH ntrace (wavefront entries stored) */
    uint32_t header_complete;/* 1 once the 24-byte header exists */
    uint32_t version_major;  /* 0 until the header is complete */
    uint32_t version_minor;
    uint32_t complete;       /* 1 if the END chunk has been read */
    uint32_t stopped;        /* 1 if reading stopped at a corrupt chunk header */
    uint32_t failed;         /* 1 if the file shrank or was rewritten */
    uint32_t chunk_count;    /* chunks read, in file order */
    uint32_t issue_count;    /* problems found (see rv_issue) */
    uint32_t error_count;    /*   of which errors (corruption) */
    uint32_t warning_count;  /*   of which warnings */
    uint32_t run_count;      /* RUN chunks (router sessions) */
    uint32_t net_count;      /* distinct router nets named by NETS */
    uint32_t search_count;   /* SRCH chunks */
    uint32_t route_count;    /* RNB chunks */
    uint32_t copper_count;   /* copper ids: CMIT chunks and VIA chunks with added = 1 */
    uint32_t copper_removed; /* copper ids removed by RIP chunks */
    uint32_t failed_pads;    /* sum of RNE nfailed */
    uint32_t drops_failed;   /* DROP chunks with ok = 0 */
    uint32_t drops_raised;   /* DROP chunks with ok = 2 */
    uint32_t reserved;       /* 0 */
} rv_trace_info;

rv_status rv_info(rv_session *session, rv_trace_info *out);

/* Number of chunks of one kind (corrupt ones included). */
rv_status rv_kind_count(rv_session *session, uint32_t kind, uint32_t *out);

/* ---- the chunk index ------------------------------------------------------ */

typedef struct rv_chunk_info {
    uint64_t offset;  /* file offset of the chunk header */
    double time;      /* chunk time t, seconds since the file was opened */
    uint32_t length;  /* payload length in bytes (without padding) */
    uint32_t kind;    /* rv_chunk_kind */
    uint32_t corrupt; /* 1 if the payload did not fit its layout (see rv_issue) */
    uint32_t session; /* index of the RUN this chunk belongs to, or RV_NONE */
    uint32_t record;  /* for SRCH the search_id, for RNB the route_id, for RUN
                         the session index; otherwise an index among chunks of
                         its kind that parsed; RV_NONE if corrupt or unknown */
    char tag[5];      /* the four tag bytes and a NUL */
} rv_chunk_info;

rv_status rv_chunk(rv_session *session, uint32_t index, rv_chunk_info *out);

/* One line describing a chunk's content.  Borrowed until the next
 * rv_chunk_text call or any call that changes the session. */
rv_status rv_chunk_text(rv_session *session, uint32_t index, const char **text);

/* ---- problems found while reading ------------------------------------------- */

#define RV_ISSUE_WARNING 1u
#define RV_ISSUE_ERROR 2u

typedef struct rv_issue_info {
    uint64_t offset;     /* file offset the problem refers to */
    uint32_t severity;   /* RV_ISSUE_WARNING or RV_ISSUE_ERROR */
    uint32_t chunk;      /* chunk index, or RV_NONE */
    const char *message; /* borrowed */
} rv_issue_info;

rv_status rv_issue(rv_session *session, uint32_t index, rv_issue_info *out);

/* ---- router sessions (RUN chunks) ------------------------------------------- */

typedef struct rv_run_info {
    double cell;           /* cell pitch, mm */
    double origin_x;       /* board position of cell (0, 0), mm */
    double origin_y;
    double track;          /* thin track width, mm */
    double track_wide;     /* wide track width, mm */
    double via_dia;        /* via diameter for drawing, mm */
    double via_drill;
    const char *producer;  /* borrowed */
    const char *layer_name[2]; /* KiCad names of router layers 0 and 1; borrowed */
    uint32_t chunk;        /* index of the RUN chunk */
    uint32_t valid;        /* 0 if the RUN chunk was corrupt (other fields are 0) */
    int32_t nx;            /* grid columns */
    int32_t ny;            /* grid rows */
    int32_t nd;            /* directions */
    int32_t via_cost;
    int32_t rip_cost;
    int32_t max_rips;
    uint32_t flags;        /* bit 0 strict, bit 1 skip_joined_stubs, bit 2 has_edge_margin */
    uint32_t reserved;     /* 0 */
} rv_run_info;

rv_status rv_run(rv_session *session, uint32_t index, rv_run_info *out);

/* ---- router net names (NETS chunks) ------------------------------------------ */

/* The index-th router net, in ascending net number, with its latest name. */
rv_status rv_net(rv_session *session, uint32_t index, int32_t *code, const char **name);

/* ---- searches (SRCH chunks) --------------------------------------------------- */

/* SRCH flag bits (TRACE_FORMAT.md 7.11). */
#define RV_SRCH_WIDE (1u << 0)
#define RV_SRCH_HAS_GOAL (1u << 1)
#define RV_SRCH_HAS_MAX (1u << 2)
#define RV_SRCH_VIA_GOAL (1u << 3)
#define RV_SRCH_RELAX (1u << 4)
#define RV_SRCH_FOUND (1u << 5)
#define RV_SRCH_NEW_VIA (1u << 6)
#define RV_SRCH_HAS_COSTS (1u << 7)
#define RV_SRCH_ERROR (1u << 8)
#define RV_SRCH_HAS_TRACE (1u << 9)

typedef struct rv_search_info {
    double time;            /* chunk time: when the search returned, seconds */
    double duration_s;      /* wall time of the search */
    int64_t max_cost;       /* -1 if none */
    int64_t cost;           /* path cost, -1 if not found */
    int64_t expanded;       /* expanded states, -1 if unknown */
    uint32_t search_id;
    uint32_t chunk;         /* chunk index */
    uint32_t valid;         /* 0 if the SRCH chunk was corrupt (other fields are 0) */
    uint32_t session;       /* RUN session index */
    uint32_t route_id;      /* RV_NONE unless inside route_net */
    uint32_t ctx;           /* 0 direct, 1 route_net, 2 drop_to_plane */
    int32_t net;
    uint32_t flags;         /* RV_SRCH_* */
    uint32_t nsrc, ntgt, npath;
    uint32_t ntrace;        /* trace entries stored in this chunk */
    uint32_t ntrace_total;
    uint32_t repeat_of;     /* RV_NONE unless the trace is that of an earlier search */
    uint32_t wavefront_search; /* search whose arrays are this wavefront, RV_NONE if none */
    uint32_t wavefront_length; /* entries rv_wavefront returns; 0 if none */
} rv_search_info;

rv_status rv_search(rv_session *session, uint32_t search_id, rv_search_info *out);

/* The source (which = 0), target (1) or path (2) cells of a search, as
 * stored: cell = layer * nx * ny + j * nx + i.  Borrowed. */
rv_status rv_search_cells(rv_session *session, uint32_t search_id, uint32_t which,
                          const uint32_t **cells, uint32_t *count);

/*
 * The wavefront of a search: cells in heap pop order and, when the search
 * recorded costs, the cost so far of each (else *costs is NULL).  A search
 * without a wavefront gives *count = 0 and NULL arrays.  Read from the file
 * on demand through a small cache; borrowed until the next rv_wavefront call
 * or any call that changes the session.  RV_E_FORMAT if the stored arrays
 * hold a cell outside the grid.
 */
rv_status rv_wavefront(rv_session *session, uint32_t search_id, const uint32_t **cells,
                       const int32_t **costs, uint32_t *count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RV_H */
