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
 *     calls that change a session are rv_open, rv_open_ex, rv_reload,
 *     rv_close, rv_refresh, rv_poll, rv_set_following, rv_live_text,
 *     rv_board_select, rv_layer_set_visible, rv_class_set_visible, the
 *     playback calls (rv_advance, rv_play, rv_set_direction, rv_set_speed,
 *     rv_speed_step, rv_seek_*, rv_step, rv_jump, rv_set_pacing), the
 *     selection calls (rv_select_net, rv_select_name, rv_select_clear,
 *     rv_click), rv_nets_set_view, and the calls that return a borrowed
 *     buffer (rv_chunk_text, rv_wavefront, rv_view_status, rv_text,
 *     rv_get_display_list, rv_get_overlay, rv_nets_get, rv_nets_cell,
 *     rv_hit_test, rv_hover_status, rv_net_history, rv_net_events), each of
 *     which replaces only the buffer it returned before (rv_nets_get and
 *     rv_nets_set_view replace the table's rows and cell texts; rv_hit_test
 *     the hits);
 *     rv_get_wavefront_image may change the pixels of every wavefront image
 *     (not their addresses within one image size).  The camera calls
 *     (rv_view_*) never invalidate a display list.
 *
 * Milestone M1: the session lifecycle and the trace reader (open, close,
 * refresh and read-only facts about the file).  Milestone M2: the board
 * display list in KiCad's default colours, layer and object visibility, and
 * the camera.  Milestone M3: the timeline, the playback clock, and the
 * overlay of router copper, rip-up ghosts, failure markers and the current
 * event.  Milestone M4: the wavefront of each search as images, and pacing
 * modes.  Milestone M5: the nets table, the net selection and its
 * highlight, hit testing, and the attempt history of a net.  Milestone M6:
 * following a trace while it is written (rv_open_ex, rv_poll, rv_live_*,
 * rv_set_following), with every table extended incrementally.
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
#define RV_API_VERSION 7u

/* "No id": an absent search, route, copper or session index. */
#define RV_NONE 0xFFFFFFFFu

/* Result of a core call.  The numeric values are part of the ABI. */
typedef enum rv_status {
    RV_OK = 0,
    RV_E_INVALID_ARGUMENT = 1, /* a NULL or out-of-range argument, or no trace open */
    RV_E_NO_MEMORY = 2,        /* an allocation failed */
    RV_E_INTERNAL = 3,         /* any other failure inside the core */
    RV_E_IO = 4,               /* the file could not be opened or read */
    RV_E_FORMAT = 5,           /* not a trace file */
    RV_E_UNSUPPORTED = 6,      /* a trace format major version this core cannot read */
    RV_E_REPLACED = 7          /* M6: the file is no longer the trace that was opened: reload it */
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
 * the number of new chunks in *new_chunks (if not NULL).  Everything derived
 * (timeline, nets table, histories, overlay, hit testing, the board list) is
 * extended incrementally, in time proportional to what was appended, and the
 * session then equals one opened afresh on the same bytes (the position,
 * the view and the selection aside).  With RV_OPEN_LIVE the current event
 * never changes because of a refresh: events that existed keep their times
 * while the trace grows, and when it completes (refit) the position keeps
 * its event and the fraction of it played; following moves the position
 * only in rv_advance.  Without RV_OPEN_LIVE (M5 behaviour) every refresh
 * refits; a position at the end goes to the new end, any other keeps its
 * event and fraction.
 *
 * Replacement (M6): if the file is no longer the one opened (it shrank, its
 * header or the last indexed chunk header changed, or its path now names
 * another file or none), the trace enters a failed state: this and every
 * later refresh return RV_E_REPLACED, and the facts already read stay
 * available and unchanged (two runs are never mixed).  Reload it
 * (rv_reload).  A header that is not a trace's gives RV_E_FORMAT or
 * RV_E_UNSUPPORTED, also for good.  RV_E_IO (the file cannot be read now)
 * is not a failed state.
 */
rv_status rv_refresh(rv_session *session, uint32_t *new_chunks);

/* ---- M6: following a trace while it is written ----------------------------- */

/* rv_open_ex flags. */
#define RV_OPEN_WAIT 1u /* a path with no file yet opens an empty trace that waits for it
                           (RV_LIVE_WAITING; rv_poll opens the file once it appears) */
#define RV_OPEN_LIVE 2u /* follow the trace while it grows: prefix pacing (see rv_pacing)
                           and following on at open if the trace is not complete */

/*
 * rv_open with flags (RV_OPEN_*; rv_open is rv_open_ex with 0).  Opening a
 * complete trace with RV_OPEN_LIVE is the same as without it.
 */
rv_status rv_open_ex(rv_session *session, const char *path, uint32_t flags);

/*
 * Open the path of the last rv_open or rv_open_ex again, with its flags,
 * after RV_E_REPLACED.  Keeps the camera, layer and object visibility, the
 * pacing, the speed and the selected router net; everything else is as
 * after an open.  RV_E_INVALID_ARGUMENT if nothing was opened.
 */
rv_status rv_reload(rv_session *session);

/*
 * rv_refresh for a client that follows a file: call it on a timer (the Mac
 * client: every 0.25 s while the trace is not complete).  `now` is the
 * client's clock in seconds (any origin, non-decreasing); the core has no
 * clock of its own and uses it only to tell how long the file has not grown
 * (rv_live.idle_seconds).  `changed` (may be NULL) gets RV_CHANGED_* bits:
 * RV_CHANGED_TIMELINE when events were added or retimed, the others as for
 * the playback calls.  Errors as rv_refresh.
 */
rv_status rv_poll(rv_session *session, double now, uint32_t *new_chunks, uint32_t *changed);

/* What the file is doing (rv_live.state). */
typedef enum rv_live_state {
    RV_LIVE_NONE = 0,     /* no trace open */
    RV_LIVE_WAITING = 1,  /* no file yet (RV_OPEN_WAIT), or its header is not written yet */
    RV_LIVE_GROWING = 2,  /* no END yet, and it grew within RV_LIVE_STALL_SECONDS */
    RV_LIVE_STALLED = 3,  /* no END, and it has not grown for RV_LIVE_STALL_SECONDS or more
                             (a run that died without END stays here: the core has no timeout) */
    RV_LIVE_COMPLETE = 4, /* END read */
    RV_LIVE_STOPPED = 5,  /* reading stopped at a corrupt chunk header */
    RV_LIVE_REPLACED = 6, /* RV_E_REPLACED: reload */
    RV_LIVE_FAILED = 7    /* the header is not a trace's */
} rv_live_state;

#define RV_LIVE_STALL_SECONDS 5.0

typedef struct rv_live {
    double idle_seconds;   /* now of the last rv_poll minus now of the rv_poll that last saw
                              the file grow (0 before the first rv_poll) */
    double lag_seconds;    /* router seconds from the current event's last chunk to the
                              newest chunk */
    uint64_t file_size;    /* bytes seen */
    uint64_t indexed_end;  /* bytes indexed (a chunk being written is not) */
    uint32_t state;        /* rv_live_state */
    uint32_t following;    /* 1 while following (see rv_set_following) */
    uint32_t live;         /* 1 if opened with RV_OPEN_LIVE */
    uint32_t chunks;       /* chunks indexed */
    uint32_t events;
    uint32_t skips;        /* times following jumped ahead (more than RV_FOLLOW_LAG behind) */
} rv_live;

rv_status rv_live_get(rv_session *session, rv_live *out);

/* One line for a live indicator: "Live: growing, 12.3 MB, 4 012 events",
 * "Live: stalled, no new data for 12 s, ...", "Complete: ...", "Waiting for
 * the file", "Replaced: ...".  "" with no trace.  Borrowed until the next
 * rv_live_text or any call that changes the session. */
rv_status rv_live_text(rv_session *session, const char **text);

/*
 * Following.  On (rv_set_following 1, rv_jump to the end, rv_play at the
 * end, playing forward into the end, or rv_open_ex with RV_OPEN_LIVE of a
 * growing trace): the position goes to the end (rv_play: stays there) and
 * the session plays forward toward the newest event; each rv_advance(dt):
 *   - if the current event's last chunk is more than RV_FOLLOW_LAG router
 *     seconds older than the newest chunk, jumps to the start of the first
 *     event within RV_FOLLOW_LAG of it (rv_live.skips counts these);
 *   - else moves forward at max(speed, gap / RV_FOLLOW_CATCH_UP) playback
 *     seconds per second, gap being the playback time left to the end, but
 *     stops at RV_FOLLOW_SETTLED of the first search with time (the current
 *     event included) that has not reached that point: at most one new
 *     search per call, and no search is skipped (each is shown with its
 *     wavefront settled);
 *   - at the end, stays there (playing) while the trace grows; when the
 *     trace no longer grows (END), following ends there and playback pauses.
 * Off: pausing (rv_play 0), any seek, rv_step, rv_set_direction(-1) or
 * rv_set_following 0 (which pauses).  rv_set_following 1 on a trace that is
 * not growing, or not opened with RV_OPEN_LIVE, only jumps to the end.
 * `changed` gets RV_CHANGED_LIVE when following or playing changed.
 */
#define RV_FOLLOW_LAG 2.0
#define RV_FOLLOW_CATCH_UP 0.5
#define RV_FOLLOW_SETTLED 0.9
rv_status rv_set_following(rv_session *session, uint32_t on, uint32_t *changed);

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

/* ==== M2: the board ========================================================
 *
 * The board drawn is a BRD chunk (TRACE_FORMAT.md 7.18): by default the
 * latest one at or before the playback position (see M3 below; after
 * rv_open the position is the end, so this is the latest BRD in the file,
 * and a refresh while at the end switches to a newer one), or one chosen
 * with rv_board_select.  When the position has no BRD at or before it, the
 * display list is the router outline of the position's router session: the
 * grid extent on "Edge.Cuts" and the session's PAD, RECT and DISC chunks as
 * outlines on the two router layers (layer indices are then positions in
 * that small layer table: the router layers, then Edge.Cuts).  The calls
 * below also work with no trace open or a trace without a board or RUN:
 * there is then no board, no layers and an empty display list, and the
 * camera still works.
 */

/* ---- boards (BRD chunks) -------------------------------------------------- */

typedef struct rv_board_info {
    double thickness_mm;
    double origin_x_mm;      /* BRD origin_x_nm, in mm */
    double origin_y_mm;
    const char *label;       /* "placed", "final", ...; borrowed */
    uint32_t chunk;          /* index of the BRD chunk */
    uint32_t layer_count;
    uint32_t net_count;
    uint32_t footprint_count;
    uint32_t record_count;
    uint32_t reserved;       /* 0 */
} rv_board_info;

/* Number of readable BRD chunks (0 with no trace open). */
rv_status rv_board_count(rv_session *session, uint32_t *count);
rv_status rv_board(rv_session *session, uint32_t index, rv_board_info *out);

/* Draw BRD number index, or with RV_NONE the latest one at or before the
 * playback position (the default, and the state after every rv_open).  An
 * index beyond the boards read so far is accepted and shows no board until
 * a refresh reads it. */
rv_status rv_board_select(rv_session *session, uint32_t index);

/* The BRD index being drawn, RV_NONE if there is none. */
rv_status rv_board_selected(rv_session *session, uint32_t *index);

/* ---- layers of the board drawn ------------------------------------------------ */

/* Layer kinds (rv_layer_info.kind). */
#define RV_LAYER_OTHER 0u
#define RV_LAYER_COPPER 1u
#define RV_LAYER_ADHESIVE 2u
#define RV_LAYER_PASTE 3u
#define RV_LAYER_SILK 4u
#define RV_LAYER_MASK 5u
#define RV_LAYER_COURTYARD 6u
#define RV_LAYER_FAB 7u
#define RV_LAYER_EDGE_CUTS 8u
#define RV_LAYER_MARGIN 9u
#define RV_LAYER_USER 10u

/* Board sides (rv_layer_info.side). */
#define RV_SIDE_NONE 0u
#define RV_SIDE_FRONT 1u
#define RV_SIDE_BACK 2u
#define RV_SIDE_INNER 3u

typedef struct rv_layer_info {
    const char *name;    /* canonical KiCad name from the BRD layer table; borrowed */
    uint32_t index;      /* position in the BRD layer table (records' layer_index) */
    uint32_t kind;       /* RV_LAYER_* */
    uint32_t side;       /* RV_SIDE_* */
    uint32_t rgba;       /* theme colour, 0xRRGGBBAA, straight alpha */
    uint32_t visible;    /* 1 if shown */
    uint32_t draw_rank;  /* larger is drawn above; see rv_order_key */
    uint32_t items;      /* records of the board drawn on this layer */
    int32_t kicad_id;    /* informational (KiCad renumbers ids) */
    int32_t type;        /* copper LAYER_T, -1 for non-copper */
    uint32_t reserved;   /* 0 */
} rv_layer_info;

/* Layers of the board drawn, in the order of KiCad's Appearance panel
 * (copper front to back, then the technical layers).  0 without a board. */
rv_status rv_layer_count(rv_session *session, uint32_t *count);
rv_status rv_layer(rv_session *session, uint32_t position, rv_layer_info *out);

/*
 * Show or hide a layer by canonical name.  The setting is kept per name for
 * the life of the session, across boards and traces, and may name a layer
 * the board does not have.  Defaults: every layer shown except masks, paste
 * and courtyards.  Both KiCad's long ("F.Silkscreen") and short ("F.SilkS")
 * names are recognised for colours and defaults, but visibility is keyed by
 * the exact name the board uses.
 */
rv_status rv_layer_set_visible(rv_session *session, const char *name, uint32_t visible);

/* ---- object classes ------------------------------------------------------------ */

typedef enum rv_object_class {
    RV_OBJ_ZONES = 0,        /* zone fills and zone outlines */
    RV_OBJ_TRACKS = 1,
    RV_OBJ_PADS = 2,         /* pad shapes on every layer (copper, mask, paste) */
    RV_OBJ_VIAS = 3,
    RV_OBJ_HOLES = 4,        /* drills of pads, vias and mounting holes */
    RV_OBJ_TEXT = 5,         /* text glyph outlines, board and footprint */
    RV_OBJ_FP_GRAPHICS = 6,  /* footprint graphics (silk, fab, courtyard lines) */
    RV_OBJ_GRAPHICS = 7,     /* board graphics outside footprints */
    RV_OBJ_RULE_AREAS = 8,   /* keep-outs */
    RV_OBJ_OUTLINE = 9,      /* board outline (Edge.Cuts) */
    RV_OBJ_ROUTED = 10,      /* router copper and rip-up ghosts (overlay, M3) */
    RV_OBJ_WAVEFRONT = 11    /* the wavefront images (M4) */
    /* M5: the net highlight is no class of its own: it follows the classes
     * and layers of what it highlights (see rv_select_net). */
} rv_object_class;

/* Number of object classes this core produces (12). */
rv_status rv_class_count(rv_session *session, uint32_t *count);
/* Name for the Appearance panel ("Zones", ...), static; and visibility. */
rv_status rv_class(rv_session *session, uint32_t cls, const char **name, uint32_t *visible);
/* Default: all shown.  A via's hole needs Vias and Holes shown, a pad's
 * hole Pads and Holes. */
rv_status rv_class_set_visible(rv_session *session, uint32_t cls, uint32_t visible);

/* ---- the display list ----------------------------------------------------------- */

/*
 * The board as batches of five primitives, in board millimetres (x right,
 * y down, as KiCad), sorted bottom to top: draw the batches in array order
 * over the background colour.  Each batch has one colour and one primitive:
 *
 *   RV_FILL    filled polygons.  Item k is polygon first + k: polys[2p] is
 *              its first contour, polys[2p + 1] its number of contours.
 *              Contour c has contours[2c + 1] points starting at point
 *              contours[2c] (xy[2i], xy[2i + 1]); contours are closed
 *              implicitly.  Each polygon is filled even-odd over its own
 *              contours (the first is the outline, the others holes);
 *              polygons in a batch are unioned.  The core orients every
 *              outline with positive signed area (sum of x_i*y_(i+1) -
 *              x_(i+1)*y_i > 0) and every hole negative, so filling the
 *              whole batch as one path with the NON-ZERO rule gives the
 *              same result (it differs only where three holes of one
 *              polygon overlap); a client may fill per polygon instead.
 *   RV_STROKE  open polylines, round caps and joins, line width `width`.
 *              Item k is contour first + k (as above).
 *   RV_SEGS    straight segments with round caps, width `width`.  Item k is
 *              segs[4(first + k) .. + 3] = x0, y0, x1, y1.
 *   RV_DISC    filled circles.  Item k is circles[3(first + k) .. + 2] =
 *              x, y, r.
 *   RV_RING    circle outlines of radius r drawn with line width `width`,
 *              items as RV_DISC.
 *
 * Only visible batches are listed.  Batches of one layer are contiguous
 * except where another layer's batches interleave by `order`.
 *
 * Caching: geometry_generation changes only when the geometry may have
 * changed (another trace, or another board or router outline, chosen or
 * reached by the playback position); within one geometry_generation a
 * batch's `key` always names the same geometry, so a client may keep
 * prepared paths keyed by `key` across visibility changes.  Moving back to
 * one of the last two lists built gives back its geometry_generation.
 * `generation` changes whenever the list may differ.  A playback position
 * change that keeps the same board changes neither (only the overlay, M3).
 */
typedef enum rv_prim {
    RV_FILL = 1,
    RV_STROKE = 2,
    RV_SEGS = 3,
    RV_DISC = 4,
    RV_RING = 5
} rv_prim;

/* Sub-slots inside a layer's draw rank (the low 8 bits of `order`).
 * Slots marked "reserved" are not produced by this version; they fix where
 * later milestones' dynamic content goes. */
#define RV_SUB_WAVEFRONT 0x08u    /* M4: wavefront image (rv_get_wavefront_image), under the copper */
#define RV_SUB_ZONE 0x10u
#define RV_SUB_ZONE_OUTLINE 0x14u
#define RV_SUB_RULE_AREA 0x18u
#define RV_SUB_GRAPHIC 0x20u
#define RV_SUB_TEXT 0x28u
#define RV_SUB_TRACK 0x30u
#define RV_SUB_GHOST 0x34u        /* overlay (M3): ghosts of ripped router copper */
#define RV_SUB_ROUTED 0x38u       /* overlay (M3): router copper, above board tracks */
#define RV_SUB_ROUTED_VIA 0x3Au   /* overlay (M3): router vias */
#define RV_SUB_PAD 0x40u
#define RV_SUB_VIA 0x50u
#define RV_SUB_OUTLINE 0x60u
/* In the hole slot (layer RV_NONE): */
#define RV_SUB_HOLE_WALL 0x10u
#define RV_SUB_PAD_HOLE 0x20u
#define RV_SUB_VIA_HOLE 0x28u
#define RV_SUB_ROUTED_DRILL 0x2Cu /* overlay (M3): drills of router vias */
#define RV_SUB_NON_PLATED 0x30u

typedef struct rv_batch {
    uint32_t prim;     /* rv_prim */
    uint32_t cls;      /* rv_object_class */
    uint32_t layer;    /* BRD layer index; RV_NONE for holes */
    uint32_t order;    /* draw_rank * 256 + RV_SUB_*; ascending */
    uint32_t rgba;     /* 0xRRGGBBAA, straight alpha (opacity applied) */
    uint32_t key;      /* stable within one geometry_generation */
    float width;       /* RV_STROKE, RV_SEGS, RV_RING: line width in mm; else 0 */
    uint32_t first;    /* first item (meaning depends on prim, see above) */
    uint32_t count;    /* number of items */
    float bbox[4];     /* min x, min y, max x, max y in mm, line width included */
} rv_batch;

typedef struct rv_display_list {
    const rv_batch *batches;
    const float *xy;          /* 2 per point */
    const uint32_t *contours; /* 2 per contour: first point, number of points */
    const uint32_t *polys;    /* 2 per polygon: first contour, number of contours */
    const float *segs;        /* 4 per segment */
    const float *circles;     /* 3 per circle */
    uint32_t batch_count;
    uint32_t point_count;
    uint32_t contour_count;
    uint32_t poly_count;
    uint32_t seg_count;
    uint32_t circle_count;
    uint32_t background;          /* theme background, 0xRRGGBBAA */
    uint32_t board;               /* BRD index drawn, RV_NONE if none */
    uint32_t has_extent;          /* 0 if the board draws nothing */
    uint32_t geometry_generation;
    uint32_t generation;
    uint32_t reserved;            /* 0 */
    float extent[4];   /* board outline bounds (else all content), mm */
} rv_display_list;

/* The display list of the board drawn.  The arrays are borrowed (see the
 * rules at the top).  Built on first use after a change. */
rv_status rv_get_display_list(rv_session *session, rv_display_list *out);

/* The order key of a sub-slot of a layer (layer = BRD layer index), or of
 * the hole slot (layer = RV_NONE): where a client inserts content of its
 * own, such as the wavefront image (RV_SUB_WAVEFRONT), between batches. */
rv_status rv_order_key(rv_session *session, uint32_t layer, uint32_t sub, uint32_t *key);

/* ---- the camera ------------------------------------------------------------------ */

/*
 * View coordinates are the client's points: origin at the top-left corner
 * of the view, y down.  view = board_mm * scale + (tx, ty).  A client
 * drawing into pixels multiplies by backing (pixels per point).
 */
typedef struct rv_view {
    double scale;        /* view points per board mm */
    double tx;           /* translation, points */
    double ty;
    double width;        /* view size, points */
    double height;
    double backing;      /* pixels per point */
    double fit_scale;    /* the scale "zoom to fit" gives now */
    double zoom_percent; /* 100 * scale / fit_scale */
    double min_scale;    /* zoom limits */
    double max_scale;
} rv_view;

/* Size of the view in points and its backing scale.  The board point at
 * the top-left corner stays fixed.  A fit asked for before the view had a
 * size is done now. */
rv_status rv_view_set_size(rv_session *session, double width, double height, double backing);

/* Zoom to fit the board drawn (its outline, else all content) with a small
 * margin, centred.  Without a board: 10 points per mm about the origin. */
rv_status rv_view_fit(rv_session *session);

/* Multiply the scale by factor (> 1 zooms in) about view point (vx, vy),
 * which keeps showing the same board point.  Clamped to the limits. */
rv_status rv_view_zoom(rv_session *session, double factor, double vx, double vy);

/* Move the content by (dx, dy) points, as a drag does. */
rv_status rv_view_pan(rv_session *session, double dx, double dy);

rv_status rv_view_get(rv_session *session, rv_view *out);
rv_status rv_view_to_board(rv_session *session, double vx, double vy, double *bx, double *by);
rv_status rv_board_to_view(rv_session *session, double bx, double by, double *vx, double *vy);

/* Status-line text for the cursor at view point (vx, vy): board position
 * in mm and the zoom.  Borrowed until the next rv_view_status call or any
 * call that changes the session. */
rv_status rv_view_status(rv_session *session, double vx, double vy, const char **text);

/* ==== M3: timeline, playback and the overlay ================================
 *
 * The run is a sequence of events.  One event per chunk of these kinds:
 * RUN (a router session starts), SNAP, SRCH, CMIT, VIA with added = 1,
 * RNB, RNE, DROP, MARK with level 0 (a phase starts), BRD and END; an RBLD
 * and the RIP chunks with ctx = 1 that immediately follow it are one rip-up
 * event, and any other RIP is a rip-up event of its own.  All other chunks
 * (NETS, PAD, RECT, DISC, GRDS, ROUT, MARK notes, VIA with added = 0,
 * corrupt and unknown chunks) are applied with the next event; chunks after
 * the last event belong to it.
 *
 * Position.  The playback position is a time t in [0, total] seconds of
 * playback at 1x.  Event k occupies [start, start + duration); its chunks
 * take effect at its start, and the fraction played inside it only animates
 * (the wavefront cells revealed, M4).  The current event at t is the last
 * event starting at or before t.  An event the pacing gives no time
 * (duration 0) therefore takes effect together with the next event that has
 * time and is never current itself (unless it is the last event); stepping
 * skips it, and seeking to it shows that next event.  The state shown at a
 * position is a function of t alone: playing, stepping and seeking in either
 * direction to the same t give the same overlay, board and wavefront.
 * After rv_open the position is the end (t = total), paused; a refresh
 * keeps the position (and stays at the end if it was there).
 *
 * Pacing: see rv_set_pacing (M4).  The default, "fit length", makes the
 * whole run last 180 s at 1x; each event gets a share proportional to a
 * weight: a search 1 + ln(1 + size) (size = expanded states, else wavefront
 * entries; for a search that found nothing at most 1000), a commit 2, a via
 * 1, a rip-up 3, route_net begins 0.5, route_net ends 0.5 (2 if pads
 * failed), a drop 1, a phase 4, a snapshot 0.5, a session 1, a board 2, the
 * end 1.
 *
 * State at an event (TRACE_FORMAT.md 8):
 *   - router copper: every copper item created at or before the event and
 *     not removed by a RIP at or before it, in the event's router session;
 *   - ghosts: copper removed by a rip-up whose event started less than
 *     1.5 s of playback before t; alpha 0.75 * (1 - age / 1.5 s), quantised
 *     to 16 levels;
 *   - failed pads: each label in an RNE's failed list, and each DROP with
 *     ok = 0, from that event until the next RNB with the same route_net
 *     name or the next RUN (TRACE_FORMAT.md 7.10).  Placed at the RNB pad
 *     with that label; a DROP failure at the centre of the sources of the
 *     drop_to_plane search just before it, else at the pad of the latest RNB
 *     of that name; otherwise counted but not drawn;
 *   - the board underneath (when following the position): the latest BRD at
 *     or before the event's last chunk;
 *   - routed copper and ghosts are not drawn when the board underneath
 *     already holds the routing: it has at least one TRACK or VIA record,
 *     and its BRD chunk is after the last chunk of the event's session that
 *     created or removed copper and at or before the event's last chunk.
 */

/* Event kinds. */
typedef enum rv_event_kind {
    RV_EV_SESSION = 0,
    RV_EV_SNAPSHOT = 1,
    RV_EV_SEARCH = 2,
    RV_EV_COMMIT = 3,
    RV_EV_VIA = 4,
    RV_EV_RIPUP = 5,
    RV_EV_ROUTE_BEGIN = 6,
    RV_EV_ROUTE_END = 7,
    RV_EV_DROP = 8,
    RV_EV_PHASE = 9,
    RV_EV_BOARD = 10,
    RV_EV_END = 11,
    RV_EV_KIND_COUNT = 12
} rv_event_kind;

/* Name of an event kind ("search", ...), "" if out of range.  Static. */
const char *rv_event_kind_name(uint32_t kind);

/* rv_event.flags */
#define RV_EVF_FOUND (1u << 0)   /* search found a path */
#define RV_EVF_FAILED (1u << 1)  /* search not found; RNE with failed pads; DROP ok = 0 */
#define RV_EVF_ERROR (1u << 2)   /* SRCH error; RNE status 1; DROP raised */
#define RV_EVF_HAS_NET (1u << 3) /* net is meaningful */
#define RV_EVF_REBUILD (1u << 4) /* a rip-up started by an RBLD */

typedef struct rv_event {
    double start;        /* playback seconds */
    double duration;     /* >= 0; 0 when the pacing gives the event no time */
    float size;          /* searches: ln(1 + size) / ln(1 + largest size), 0..1; else 0 */
    uint32_t kind;       /* rv_event_kind */
    uint32_t flags;      /* RV_EVF_* */
    uint32_t chunk;      /* the chunk that made the event */
    uint32_t last_chunk; /* the last chunk applied by it */
    uint32_t record;     /* search_id index for searches, else the index of the
                            chunk among its kind's parsed chunks (RV_NONE for a
                            rip-up without an RBLD) */
    int32_t net;         /* router net, if RV_EVF_HAS_NET */
    uint32_t session;    /* router session (RUN index), RV_NONE before any RUN */
    uint32_t route;      /* route_net in progress (RNB index), else RV_NONE */
    uint32_t phase;      /* phase index (rv_phase), RV_NONE before the first */
    uint32_t board;      /* latest BRD index at or before last_chunk, else RV_NONE */
    uint32_t reserved;   /* 0 */
} rv_event;

typedef struct rv_timeline {
    const rv_event *events;      /* borrowed */
    const double *rip_ticks;     /* start of each rip-up event, ascending; borrowed */
    const double *failure_ticks; /* start of each event with failed pads; borrowed */
    double total;                /* playback seconds of the whole run at 1x (0 if no event has time) */
    double router_seconds;       /* t of the last chunk */
    double span;                 /* M6: the length the axis shows: total, except while a trace
                                    opened with RV_OPEN_LIVE grows: then it grows by steps
                                    (x1.5 when total reaches it), so old events keep their
                                    place on the axis between steps */
    uint32_t event_count;
    uint32_t rip_tick_count;
    uint32_t failure_tick_count;
    uint32_t phase_count;
    uint32_t generation;         /* changes when the timeline is rebuilt, extended or repaced
                                    (open, refresh, rv_poll, rv_set_pacing) */
    uint32_t unchanged;          /* M6: events [0, unchanged) are the same, in every field, as
                                    at the previous rv_get_timeline call on this session (0
                                    after an open) */
} rv_timeline;

/* The timeline.  With no trace open: no events.  Borrowed. */
rv_status rv_get_timeline(rv_session *session, rv_timeline *out);

typedef struct rv_phase_info {
    double start;        /* playback seconds */
    double end;          /* start of the next phase, or total */
    const char *label;   /* MARK text; borrowed */
    uint32_t event;      /* the phase event */
    uint32_t reserved;   /* 0 */
} rv_phase_info;

rv_status rv_phase(rv_session *session, uint32_t index, rv_phase_info *out);

typedef struct rv_position {
    double time;            /* t, playback seconds */
    double total;
    double fraction;        /* of the current event played, 0..1 */
    double speed;           /* playback speed factor */
    double router_time;     /* t of the last chunk applied, seconds */
    uint32_t event;         /* current event, RV_NONE without events */
    uint32_t event_count;
    uint32_t last_chunk;    /* last chunk applied, RV_NONE without events */
    uint32_t playing;       /* 1 while playing */
    int32_t direction;      /* +1 forward, -1 backward */
    uint32_t at_end;        /* 1 when t = total */
    uint32_t cells_revealed;/* searches: wavefront entries shown at this fraction (M4: see
                               rv_get_wavefront_image; counted whether or not the class is shown) */
    uint32_t copper_alive;  /* router copper items present (drawn unless routed_hidden) */
    uint32_t ghosts;        /* ghosts at this position (drawn unless routed_hidden) */
    uint32_t failures;      /* failed pads at this position (placed or not) */
    uint32_t routed_hidden; /* 1 when the board underneath holds the routing */
    int32_t net;            /* the current event's router net, else that of the route_net in
                               progress (M4, for "one net" pacing); valid if has_net */
    uint32_t has_net;       /* 1 if net is meaningful */
    uint32_t settled;       /* 1 in the settled part of a search event (M4) */
} rv_position;

rv_status rv_position_get(rv_session *session, rv_position *out);

/* Bits of the `changed` result of the playback calls. */
#define RV_CHANGED_POSITION 1u /* t changed: redraw the playhead and the texts */
#define RV_CHANGED_OVERLAY 2u  /* rv_get_overlay gives a different list */
#define RV_CHANGED_BOARD 4u    /* the static display list (board or outline) changed */
#define RV_CHANGED_WAVEFRONT 8u /* M4: what the wavefront images show changed (search, cells
                                   revealed, visibility): fetch them again and compare each
                                   image's generation.  RV_CHANGED_OVERLAY or BOARD may also
                                   change an image's order or layer. */
#define RV_CHANGED_SELECTION 16u /* M5: the selection changed (the overlay too) */
#define RV_CHANGED_TIMELINE 32u  /* M5: the timeline was repaced (one-net pacing follows the
                                    selection) or (M6) extended: fetch rv_get_timeline again */
#define RV_CHANGED_LIVE 64u      /* M6: following or playing changed */

/*
 * The clock.  rv_advance moves t by dt * speed * direction seconds of wall
 * time dt while playing, and stops (pauses) at either end.  It does
 * nothing while paused.  `changed` (may be NULL) gets RV_CHANGED_* bits.
 */
rv_status rv_advance(rv_session *session, double dt, uint32_t *changed);
/* Play (1) or pause (0).  Playing forward from the end, or backward from
 * the start, starts over from the other end. */
rv_status rv_play(rv_session *session, uint32_t playing, uint32_t *changed);
/* +1 forward, -1 backward. */
rv_status rv_set_direction(rv_session *session, int32_t direction);
/* Speeds: 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64 (default 1).  rv_set_speed
 * snaps to the nearest; rv_speed_step moves along the list (clamped). */
rv_status rv_set_speed(rv_session *session, double speed);
rv_status rv_speed_step(rv_session *session, int32_t steps);

/* Seeking.  The play state is kept. */
rv_status rv_seek_time(rv_session *session, double t, uint32_t *changed);
rv_status rv_seek_fraction(rv_session *session, double fraction, uint32_t *changed);
/* To the start of event index. */
rv_status rv_seek_event(rv_session *session, uint32_t event, uint32_t *changed);
/* to_end = 0: the start; 1: the end. */
rv_status rv_jump(rv_session *session, uint32_t to_end, uint32_t *changed);

/* Step kinds. */
typedef enum rv_step_kind {
    RV_STEP_EVENT = 0,   /* any event */
    RV_STEP_SEARCH = 1,
    RV_STEP_COMMIT = 2,
    RV_STEP_NET = 3,     /* route_net begins */
    RV_STEP_RIPUP = 4,
    RV_STEP_FAILURE = 5, /* RNE with failed pads, DROP ok = 0 */
    RV_STEP_PHASE = 6,
    RV_STEP_KIND_COUNT = 7
} rv_step_kind;

/* Name of a step kind ("event", "search", ...), "" if out of range.  Static. */
const char *rv_step_kind_name(uint32_t kind);

/* Step to the start of the next (direction > 0) or previous (< 0) event of
 * a kind: the first one starting after t, or the last one starting before
 * t.  Pauses.  With none in that direction the position is unchanged and
 * *changed is 0. */
rv_status rv_step(rv_session *session, uint32_t kind, int32_t direction, uint32_t *changed);

/* Texts for the current position; borrowed until the next rv_text call or
 * any call that changes the session. */
#define RV_TEXT_CAPTION 0u  /* phase, route_net, what the event does, counts, router time */
#define RV_TEXT_POSITION 1u /* "event 1234 of 9000 · 1:23.4 / 3:00.0" */
#define RV_TEXT_SPEED 2u    /* "1×" */
#define RV_TEXT_PACING 3u   /* M4: the pacing: "fit length 3:00", "one net: /CLK", "real time ×10" */
rv_status rv_text(rv_session *session, uint32_t which, const char **text);

/*
 * The overlay: what changes with the position, as a display list of the
 * same form as the board's (rv_display_list), drawn interleaved with it by
 * `order`:
 *   - router copper alive at the position: tracks (RV_STROKE, the path of
 *     each commit as polylines; RV_SEGS, pad stubs) at RV_SUB_ROUTED and
 *     vias (RV_DISC) at RV_SUB_ROUTED_VIA of each router layer's rank, in
 *     the layer's colour; via drills at RV_SUB_ROUTED_DRILL in the hole
 *     slot.  Class RV_OBJ_ROUTED; hidden with that class or the layer;
 *   - ghosts of ripped copper at RV_SUB_GHOST (class RV_OBJ_ROUTED);
 *   - markers and highlights in the overlay rank (rv_order_key with
 *     RV_OVERLAY_LAYER), always shown: failed pads (red ring and cross),
 *     the commit just made (lightened), the current search's sources
 *     (green), targets (orange; red once a search that found nothing is
 *     settled) and, once a search that found one is settled, its path
 *     (white), the pads of the route_net in progress (white rings), a new
 *     via (white ring).
 * Board layer indices in `layer` refer to the static list's layer table
 * (RV_NONE for content not on a board layer).
 *
 * Slots cut the order axis where the overlay has content.  Each slot is a
 * range [order_begin, order_end) holding a run of overlay batches; no board
 * batch has an order inside a slot.  Slots are sorted and never overlap; the
 * set of slots changes only with the router session or the board.  A
 * client can keep the board batches between two slots as one cached bitmap
 * and redraw a slot only when its generation changes.  An RV_SLOT_WAVEFRONT
 * slot (one per router layer, at RV_SUB_WAVEFRONT of its rank) holds no
 * batches: it is where that layer's wavefront image is drawn (the image
 * whose `order` equals the slot's order_begin), and its generation never
 * changes; the image has its own (rv_get_wavefront_image).
 *
 * Keys: an overlay key always names the same geometry for one value of the
 * overlay list's geometry_generation (the key epoch; it changes on rv_open
 * and rv_reload, M6: no longer on rv_refresh, which only adds keys), so a
 * client may keep prepared paths across positions and refreshes.
 * Copper is grouped in blocks of 64 copper ids per (slot, primitive,
 * width), so a commit or a rip-up changes only a few batches.  Overlay keys
 * are >= 0x80000000 and never equal a board key.  `generation` changes
 * whenever the overlay may differ; `board`, `background`, `has_extent` and
 * `extent` are those of the static list.
 */
#define RV_OVERLAY_LAYER 0xFFFFFFFEu /* rv_order_key layer for the overlay rank */

#define RV_SLOT_ROUTED 0u  /* one router layer's ghosts, copper and vias */
#define RV_SLOT_DRILLS 1u  /* router via drills, in the hole slot */
#define RV_SLOT_MARKERS 2u /* markers and highlights, above everything */
#define RV_SLOT_WAVEFRONT 3u /* M4: one router layer's wavefront image; no batches */
#define RV_SLOT_DIM 4u       /* M5: the dim rectangle of the net highlight */
#define RV_SLOT_HIGHLIGHT 5u /* M5: the selected net's copper, drawn again brightened */

typedef struct rv_slot {
    uint32_t kind;         /* RV_SLOT_* */
    uint32_t order_begin;  /* the slot holds orders in [order_begin, order_end) */
    uint32_t order_end;
    uint32_t layer;        /* RV_SLOT_ROUTED, RV_SLOT_WAVEFRONT: board layer index, or RV_NONE */
    uint32_t first_batch;  /* in the overlay list */
    uint32_t batch_count;
    uint32_t generation;   /* changes whenever this slot's batches change */
    uint32_t reserved;     /* 0 */
    float bbox[4];         /* union of the slot's batch bounds; 0s when empty */
} rv_slot;

/* The overlay list and its slots.  Both borrowed (see the rules at the
 * top).  Built on first use after a change. */
rv_status rv_get_overlay(rv_session *session, rv_display_list *out, const rv_slot **slots,
                         uint32_t *slot_count);

/* ==== M4: the wavefront and pacing ===========================================
 *
 * The wavefront.  While a search event plays, the cells its A* search
 * expanded appear in expansion order (the SRCH trace arrays; a repeat_of
 * search shows the arrays of the search it repeats).  A search event plays
 * in two parts: over the first 80 % of its time the first
 * floor(n * fraction / 0.8) of the n stored entries are shown; for the rest
 * ("settled") all n, and the overlay draws the path (found) or the targets
 * in red (not found).  Only the current event's search is shown: when the
 * event ends its wavefront goes.  rv_position.cells_revealed is the count.
 *
 * The images: one per router layer, at grid resolution (nx x ny pixels;
 * pixel (i, j) is cell (i, j), row 0 the smallest board y), premultiplied
 * sRGB, 4 bytes per pixel in memory order R, G, B, A.  A client draws each
 * scaled to its board rectangle with nearest-neighbour sampling, at the
 * slot of kind RV_SLOT_WAVEFRONT whose order_begin equals the image's
 * `order`, under the board's copper of that layer.  Colours (the router
 * layer's theme colour C; r = a cell's cost so far / the largest cost of the
 * search's stored entries, or its position in the order / (n - 1) when the
 * search stored no costs):
 *   colour = lerp(C mixed 18 % toward white, C, r),
 *   alpha  = lerp(0.22, 0.08, r);
 *   the frontier, the most recently revealed max(1, ceil(n * 0.02)) cells:
 *   C mixed 50 % toward white, alpha 0.55.
 * (M5 made these subtler: a faint tint of the layer colour that keeps the
 * board readable; API 5 had 55 %, 0.60 to 0.28, 0.04 and 85 % at 0.95.)
 * The images are empty when the class RV_OBJ_WAVEFRONT or the layer is
 * hidden, and between searches.  They are a pure function of the position,
 * the visibility and the trace: playing to a position and seeking to it,
 * from either side, give the same pixels.
 *
 * Updating: the core changes only the pixels that differ (when moving within
 * one search) and reports them.  `generation` changes whenever any pixel
 * does; `dirty` holds every pixel changed between generation `dirty_from`
 * and `generation`.  A client that has drawn generation dirty_from may update
 * only `dirty`; otherwise it takes the whole image (or `bounds`, outside
 * which every pixel is transparent).
 */

typedef struct rv_wavefront_image {
    const uint8_t *pixels;  /* height rows of `stride` bytes; NULL while nothing was ever drawn
                               (then every pixel is transparent).  Borrowed (see the rules). */
    uint32_t width;         /* pixels: the grid's nx (0 without a router session) */
    uint32_t height;        /* the grid's ny */
    uint32_t stride;        /* bytes per row: width * 4 */
    uint32_t order;         /* draw order: the order_begin of its RV_SLOT_WAVEFRONT slot */
    uint32_t layer;         /* board layer index, RV_NONE if the board drawn has no such layer */
    uint32_t router_layer;  /* 0 or 1 */
    uint32_t generation;    /* changes whenever a pixel changes (never 0) */
    uint32_t dirty_from;    /* the generation `dirty` is relative to */
    int32_t dirty[4];       /* x0, y0, x1, y1 in pixels, ends exclusive; empty if x0 >= x1 */
    int32_t bounds[4];      /* the same form: every pixel outside is transparent */
    uint32_t cells;         /* cells drawn in this image */
    uint32_t search;        /* the search event's search_id, RV_NONE when nothing is shown */
    double rect[4];         /* the whole image in board mm: x0, y0 (corner of pixel (0, 0)), x1, y1 */
} rv_wavefront_image;

/* Number of wavefront images: 2, 1 when both router layers have the same
 * name, 0 without a valid router session at the position. */
rv_status rv_wavefront_image_count(rv_session *session, uint32_t *count);

/* Wavefront image `index` (router layer 0 or 1) at the current position,
 * brought up to date.  RV_E_FORMAT if the search's stored arrays cannot be
 * read (the image is then empty). */
rv_status rv_get_wavefront_image(rv_session *session, uint32_t index, rv_wavefront_image *out);

/*
 * Pacing: how long each event lasts.  Changing it keeps the position in the
 * same event at the same fraction (at the end if it was there; at the
 * event's start if it gets no time now, which shows the next event with
 * time), changes the timeline's generation and the times of its events,
 * ticks and phases.  It is a setting of the session, kept across rv_open.
 *
 *   RV_PACING_FIT          "fit length": the run lasts fit_seconds at 1x;
 *                          each event gets a share proportional to its weight
 *                          (see M3 above; a search that found nothing weighs
 *                          as at most 1000 states: its wavefront plays
 *                          shortened).
 *   RV_PACING_NETS_ONLY    as fit length, but searches get no time: copper
 *                          appears commit by commit, no wavefront is shown.
 *   RV_PACING_ONE_NET      as fit length, but only the events of router net
 *                          `net` (rv_event.net with RV_EVF_HAS_NET) get time.
 *   RV_PACING_SKIP_FAILED  as fit length, but searches that found nothing
 *                          get no time.
 *   RV_PACING_REAL_TIME    an event lasts the router time from the last chunk
 *                          of the event before it to its own last chunk (the
 *                          first event: 0), times `slowdown`; a search that
 *                          found nothing at most 0.010 s of router time.
 * When no event gets time the run lasts 0 s and shows its end.
 *
 * Prefix pacing (M6): while a trace opened with RV_OPEN_LIVE grows, an
 * event's time depends only on itself and the events before it, so events
 * already read keep their times when the file grows.  The fit modes then
 * give an event weight * fit_seconds / RV_LIVE_FIT_WEIGHT seconds at 1x
 * (the run's total grows as it is written); real time is a prefix pacing
 * anyway.  When END is read the fit modes are refitted to fit_seconds over
 * the whole run (the position keeps its event and fraction).
 */
#define RV_LIVE_FIT_WEIGHT 30000.0
typedef enum rv_pacing_mode {
    RV_PACING_FIT = 0,
    RV_PACING_NETS_ONLY = 1,
    RV_PACING_ONE_NET = 2,
    RV_PACING_SKIP_FAILED = 3,
    RV_PACING_REAL_TIME = 4,
    RV_PACING_MODE_COUNT = 5
} rv_pacing_mode;

typedef struct rv_pacing {
    uint32_t mode;       /* rv_pacing_mode */
    int32_t net;         /* RV_PACING_ONE_NET: the router net */
    double fit_seconds;  /* the fit modes: length of the run at 1x, 1 to 86400 (default 180) */
    double slowdown;     /* RV_PACING_REAL_TIME: playback seconds per router second,
                            0.01 to 100000 (default 10) */
} rv_pacing;

/* Name of a pacing mode ("fit length", ...), "" if out of range.  Static. */
const char *rv_pacing_mode_name(uint32_t mode);

/* Set every field (RV_E_INVALID_ARGUMENT for a value out of range; then
 * nothing changes).  `changed` (may be NULL) gets RV_CHANGED_* bits. */
rv_status rv_set_pacing(rv_session *session, const rv_pacing *pacing, uint32_t *changed);
rv_status rv_get_pacing(rv_session *session, rv_pacing *out);

/* ==== M5: nets table, selection and highlight, hit testing, history ========
 *
 * Router nets are those of the NETS chunks (rv_net); a board net is a
 * (code, name) of the BRD drawn.  They are matched by name only, exactly
 * (case and all): a router net selects every board net of the same name on
 * the board drawn.  A router net without a name (no NETS entry, or "")
 * matches no board item; a board item without a net (code -1, or a net
 * with the name "") is never highlighted and selects nothing.  Codes are
 * never compared: the router's and KiCad's numbering differ.
 */

/* ---- the nets table ------------------------------------------------------------
 *
 * One row per (router session, router net) that any chunk is about, plus a
 * row without a session (session RV_NONE) for each net that NETS names but no
 * chunk mentions.  Totals are a pure function of the file; the "now" columns
 * (state, copper, failures) a function of the position as well (a row of an
 * earlier session shows that session's end; a row without a session is
 * always "not started").  Column rules:
 *   routes      RNB chunks; searches, found, not found: SRCH (RV_SRCH_FOUND)
 *   commits     CMIT chunks; vias: vias of its CMITs plus VIAs with added = 1
 *   rips        rip-up events that removed at least one of its copper items
 *   failed pads RNE failed labels plus DROPs with ok = 0
 *   expanded    sum of its searches' expanded states
 *   seconds     router time: each route_net call's t(RNE) - t(RNB) (an
 *               unfinished call: to the session's last chunk) plus duration_s
 *               of its searches outside route_net (ctx != 1)
 *   first, last its first and last event: events of its chunks other than
 *               PAD, RECT, DISC and VIA with added = 0, and the rip-ups that
 *               removed its copper (as rv_net_events)
 *   copper      its copper items alive; failures: its failed pads shown
 *   state       the first of: routing (a route_net call of the net is in
 *               progress: its RNB event at or before the position, its RNE
 *               after), failed (failures > 0), ripped (the last change to its
 *               copper removed some), complete (its latest route_net call
 *               returned status 0 with no failed pads and none of its copper
 *               was removed since), routed (copper > 0), not started.
 * Sorting: by any column, either direction; ties by session, then net code
 * (both ascending).  Names compare ASCII case-insensitively.  Filtering:
 * rows whose name contains the filter, ASCII case-insensitively.
 */
typedef enum rv_net_state {
    RV_NET_NOT_STARTED = 0,
    RV_NET_ROUTING = 1,
    RV_NET_ROUTED = 2,
    RV_NET_COMPLETE = 3,
    RV_NET_RIPPED = 4,
    RV_NET_FAILED = 5,
    RV_NET_STATE_COUNT = 6
} rv_net_state;

/* Name of a state ("not started", ...), "" if out of range.  Static. */
const char *rv_net_state_name(uint32_t state);

typedef enum rv_net_column {
    RV_NETCOL_NAME = 0, RV_NETCOL_NET = 1, RV_NETCOL_SESSION = 2, RV_NETCOL_STATE = 3,
    RV_NETCOL_ROUTES = 4, RV_NETCOL_SEARCHES = 5, RV_NETCOL_FOUND = 6, RV_NETCOL_NOT_FOUND = 7,
    RV_NETCOL_COMMITS = 8, RV_NETCOL_VIAS = 9, RV_NETCOL_RIPS = 10, RV_NETCOL_FAILED_PADS = 11,
    RV_NETCOL_EXPANDED = 12, RV_NETCOL_SECONDS = 13, RV_NETCOL_FIRST = 14, RV_NETCOL_LAST = 15,
    RV_NETCOL_COPPER = 16, RV_NETCOL_FAILURES = 17,
    RV_NETCOL_COUNT = 18
} rv_net_column;

/* Column title ("Net", "State", ...) and alignment (0 left, 1 right).
 * Static; "" and 0 for an unknown column. */
const char *rv_net_column_title(uint32_t column);
uint32_t rv_net_column_align(uint32_t column);

typedef struct rv_nets_info {
    uint32_t row_count;     /* rows of the table */
    uint32_t view_count;    /* rows passing the filter */
    uint32_t generation;    /* changes whenever a value shown or the view changes */
    uint32_t sort_column;   /* RV_NETCOL_* */
    uint32_t descending;    /* 1 if sorted descending */
    uint32_t selected_position; /* view position of the selected net's row in the position's
                                   session (else its latest row), RV_NONE if none is shown */
} rv_nets_info;

/* Bring the table to the position (it is built on first use) and describe
 * it.  Call it before reading rows after any change. */
rv_status rv_nets_get(rv_session *session, rv_nets_info *out);

/* Sort by column (descending if 1) and keep the rows whose name contains
 * filter (NULL or "" keeps all).  Default: name, ascending, no filter. */
rv_status rv_nets_set_view(rv_session *session, uint32_t column, uint32_t descending, const char *filter);

typedef struct rv_net_row {
    double seconds;
    int64_t expanded;
    const char *name;       /* borrowed: the NETS name, else "net <code>" */
    uint32_t row;           /* index in the table (stable until the table is rebuilt; M6: a
                               refresh that adds rows can move it, rows being sorted by
                               net and session) */
    int32_t net;            /* router net code */
    uint32_t session;       /* RUN index, RV_NONE for a net no chunk mentions */
    uint32_t named;         /* 1 if NETS gives it a name (it can match board nets) */
    uint32_t state;         /* rv_net_state, at the position */
    uint32_t routes, searches, found, not_found, commits, vias, rips, failed_pads;
    uint32_t first_event;   /* RV_NONE if none */
    uint32_t last_event;
    uint32_t copper;        /* at the position */
    uint32_t failures;      /* at the position */
} rv_net_row;

/* The row at view position (after sorting and filtering) as of the last
 * rv_nets_get. */
rv_status rv_nets_row(rv_session *session, uint32_t position, rv_net_row *out);

/* A cell as text (numbers formatted, events 1-based, "—" for none), as of
 * the last rv_nets_get.  Borrowed until the next rv_nets_cell call or any
 * call that changes the session. */
rv_status rv_nets_cell(rv_session *session, uint32_t position, uint32_t column, const char **text);

/* ---- the selection and its highlight -----------------------------------------
 *
 * At most one net is selected.  While one is, the overlay dims everything
 * and draws the net's copper again, brightened (KiCad's net highlight):
 *   - an RV_SLOT_DIM slot (order rank 955, between Edge.Cuts and the
 *     markers) holds one RV_FILL rectangle in the background colour at alpha
 *     0.65 over everything drawn (the board's content and the router grid,
 *     2 mm beyond), so all below is mixed 65 % toward the background;
 *   - an RV_SLOT_HIGHLIGHT slot (ranks 960 to 969) holds copies of the
 *     net's copper: the board's pads on copper layers, zone fills, tracks,
 *     vias and their holes (board nets matched by name), and the router's
 *     copper, vias, drills and ghosts of the selected router net; each in its
 *     colour mixed 30 % toward white with its alpha kept (holes keep their
 *     colours), each shown only when what it copies is shown (class, layer,
 *     and router copper not hidden by the board).  Their stacking keeps the
 *     copper layers' order (B.Cu lowest) and, within a layer, the sub-slot
 *     order; holes above all copper.  Unlike KiCad, the highlighted items are
 *     drawn above every layer's dimmed content, not in their layer's place.
 * The markers (failed pads, the current event) are drawn above both, undimmed.
 * Both slots are present whenever a trace is open (empty without a
 * selection, so drawing is unchanged then).  The selection is kept across
 * rv_refresh and cleared by rv_open and rv_close.  In "one net" pacing,
 * selecting a router net makes it the paced net (RV_CHANGED_TIMELINE).
 * `changed` (may be NULL) of the calls below gets RV_CHANGED_* bits.
 */

/* Select router net `net` (any code; its NETS name, if any, matches board nets). */
rv_status rv_select_net(rv_session *session, int32_t net, uint32_t *changed);
/* Select by name: the router net of that name (the lowest code) if there is
 * one, and the board nets of that name.  NULL or "" clears. */
rv_status rv_select_name(rv_session *session, const char *name, uint32_t *changed);
rv_status rv_select_clear(rv_session *session, uint32_t *changed);

typedef struct rv_selection {
    const char *name;      /* borrowed; "" if none or unnamed */
    uint32_t active;       /* 1 if a net is selected */
    uint32_t has_net;      /* 1 if it is a router net */
    int32_t net;           /* the router net, if has_net */
    uint32_t board_nets;   /* board nets of that name on the board drawn */
    uint32_t board_items;  /* their pads (copper layers), zone fills, tracks and vias */
    uint32_t reserved;     /* 0 */
} rv_selection;

rv_status rv_selection_get(rv_session *session, rv_selection *out);

/* ---- hit testing ---------------------------------------------------------------------
 *
 * What is under a view point, as drawn now: router copper alive (when its
 * class and layer are shown and the board does not hold the routing), the
 * board's pads on copper layers, tracks, vias and zone fills (when their
 * class and a layer they are on are shown), failed-pad markers (always), and
 * the wavefront cell revealed under the point (when shown).  An item is hit
 * within 3 view points of its edge (RV_PICK_POINTS); a zone fill or a
 * wavefront cell only from inside.  Topmost first: failure markers, then
 * the shapes (router copper, pads, tracks, vias) by draw order from the top,
 * then the areas (zone fills, wavefront cells) by draw order from the top;
 * ties: the item written later first, then the nearer.
 */
#define RV_PICK_POINTS 3.0

#define RV_HIT_ROUTED 1u    /* router copper (a commit's path, stub or via, or a VIA) */
#define RV_HIT_PAD 2u
#define RV_HIT_TRACK 3u
#define RV_HIT_VIA 4u
#define RV_HIT_ZONE 5u
#define RV_HIT_FAILURE 6u
#define RV_HIT_WAVEFRONT 7u

typedef struct rv_hit {
    double distance;       /* mm from the item's edge, 0 inside */
    int64_t cost;          /* WAVEFRONT: cost so far, -1 if not stored */
    const char *net_name;  /* borrowed: router or board net name, "" if none */
    const char *footprint; /* PAD: footprint reference, "" if none */
    const char *pad;       /* PAD: pad number (nearest PADINFO of its footprint), "" if none */
    const char *text;      /* one line describing it */
    uint32_t kind;         /* RV_HIT_* */
    uint32_t order;        /* draw order */
    uint32_t layer;        /* board layer index, RV_NONE if none */
    uint32_t router_layer; /* ROUTED, WAVEFRONT: 0 or 1, else RV_NONE */
    int32_t net;           /* router net, if has_net */
    uint32_t has_net;
    int32_t board_net;     /* board net code, -1 if none */
    uint32_t copper;       /* ROUTED: copper index (as rv_info's copper ids are counted) */
    uint32_t event;        /* ROUTED: the event that made it; FAILURE: that reported it;
                              WAVEFRONT: the search event; else RV_NONE */
    uint32_t record;       /* PAD, ZONE, TRACK, VIA: index of its record among the BRD's
                              records of that kind; FAILURE: failure index; WAVEFRONT: search_id */
    uint32_t cell;         /* WAVEFRONT: the cell (layer * nx * ny + j * nx + i) */
    uint32_t order_index;  /* WAVEFRONT: its position in the expansion order (0-based) */
} rv_hit;

/* Hit test at view point (vx, vy); stores the number of hits.  The hits
 * stay readable with rv_hit_get until the next rv_hit_test or any call that
 * changes the session. */
rv_status rv_hit_test(rv_session *session, double vx, double vy, uint32_t *count);
rv_status rv_hit_get(rv_session *session, uint32_t index, rv_hit *out);

/* Status-line text for the cursor: rv_view_status's text and, after " · ",
 * the topmost hit's text.  Borrowed until the next rv_hover_status call or
 * any call that changes the session. */
rv_status rv_hover_status(rv_session *session, double vx, double vy, const char **text);

/* A click at view point (vx, vy): selects the net of the topmost hit that
 * has one (a router net for router copper, failures and wavefront cells; a
 * board item's net by name), or clears the selection when no hit has one. */
rv_status rv_click(rv_session *session, double vx, double vy, uint32_t *changed);

/* ---- the attempt history of a net ---------------------------------------------------
 *
 * The chunks about router net `net` in file order, as entries with a depth:
 *   depth 0  "router session N"
 *   depth 1  each route_net call ("route_net K/M NAME, P pads: ok" / "raised" /
 *            "n pads failed" / "not finished"); searches, commits, vias and
 *            rip-ups outside route_net; drop_to_plane calls; rip-ups that
 *            removed its copper ("ripped up by the route_net of NAME", by "its
 *            own route_net", by "drop_to_plane" or by "a direct rip")
 *   depth 2  inside a route_net call: its searches (result, cost, path cells,
 *            states expanded, seconds, "repeats search N"), commits, rebuilds
 *            ("rebuild: rips up A, B"), what it returned, its failed pads;
 *            a drop_to_plane's failed pad
 * Rip-up cause: the RIP's route_id names the route_net call that ripped
 * (ctx 1); ctx 2 is drop_to_plane; anything else is a direct rip.
 */
#define RV_HIST_SESSION 0u
#define RV_HIST_ROUTE 1u
#define RV_HIST_SEARCH 2u
#define RV_HIST_COMMIT 3u
#define RV_HIST_VIA 4u
#define RV_HIST_RIPPED 5u     /* a rip-up removed its copper */
#define RV_HIST_REBUILD 6u    /* its route_net ripped other nets */
#define RV_HIST_FAILURE 7u    /* a failed pad */
#define RV_HIST_RETURN 8u     /* a route_net call returned */
#define RV_HIST_DROP 9u
#define RV_HIST_RIP_OTHER 10u /* a RIP of this net that removed none of its copper */

typedef struct rv_history_entry {
    const char *text;  /* borrowed */
    uint32_t depth;
    uint32_t kind;     /* RV_HIST_* */
    uint32_t event;    /* the event to seek to; RV_NONE without events */
    uint32_t chunk;
    uint32_t session;
    uint32_t flags;    /* RV_EVF_FOUND, RV_EVF_FAILED, RV_EVF_ERROR */
} rv_history_entry;

/* Build the history of router net `net`; stores the number of entries.
 * The entries stay readable with rv_history_get until the next
 * rv_net_history or any call that changes the session. */
rv_status rv_net_history(rv_session *session, int32_t net, uint32_t *count);
rv_status rv_history_get(rv_session *session, uint32_t index, rv_history_entry *out);

/* The events of router net `net`, ascending: those of its history entries
 * other than session headers (for marks on a timeline).  Borrowed until the
 * next rv_net_events or any call that changes the session. */
rv_status rv_net_events(rv_session *session, int32_t net, const uint32_t **events, uint32_t *count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RV_H */
