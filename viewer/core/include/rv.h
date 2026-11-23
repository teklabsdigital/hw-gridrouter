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
 *     calls that change a session are rv_open, rv_close, rv_refresh,
 *     rv_board_select, rv_layer_set_visible, rv_class_set_visible, and the
 *     calls that return a borrowed buffer (rv_chunk_text, rv_wavefront,
 *     rv_view_status), each of which replaces only the buffer it returned
 *     before.  The camera calls (rv_view_*) never invalidate the display
 *     list.
 *
 * Milestone M1: the session lifecycle and the trace reader (open, close,
 * refresh and read-only facts about the file).  Milestone M2: the board
 * display list in KiCad's default colours, layer and object visibility, and
 * the camera.  Later milestones add the timeline, router copper and
 * wavefront images.
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
#define RV_API_VERSION 3u

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

/* ==== M2: the board ========================================================
 *
 * The board drawn is a BRD chunk (TRACE_FORMAT.md 7.18): by default the
 * latest one in the file (a refresh that reads a newer BRD switches to it),
 * or one chosen with rv_board_select.  The calls below also work with no
 * trace open or a trace without a BRD chunk: there is then no board, no
 * layers and an empty display list, and the camera still works.
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

/* Draw BRD number index, or the latest one with RV_NONE (the default, and
 * the state after every rv_open).  An index beyond the boards read so far
 * is accepted and shows no board until a refresh reads it. */
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
    RV_OBJ_OUTLINE = 9       /* board outline (Edge.Cuts) */
    /* Reserved for later milestones, not produced by this version:
     * 10 router copper (M3), 11 wavefront (M4), 12 net highlight (M5). */
} rv_object_class;

/* Number of object classes this core produces (10). */
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
 * changed (another board or trace); within one geometry_generation a
 * batch's `key` always names the same geometry, so a client may keep
 * prepared paths keyed by `key` across visibility changes.  `generation`
 * changes whenever the list may differ.
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
#define RV_SUB_WAVEFRONT 0x08u    /* reserved (M4): wavefront image, under the copper */
#define RV_SUB_ZONE 0x10u
#define RV_SUB_ZONE_OUTLINE 0x14u
#define RV_SUB_RULE_AREA 0x18u
#define RV_SUB_GRAPHIC 0x20u
#define RV_SUB_TEXT 0x28u
#define RV_SUB_TRACK 0x30u
#define RV_SUB_ROUTED 0x38u       /* reserved (M3): router copper, above board tracks */
#define RV_SUB_PAD 0x40u
#define RV_SUB_VIA 0x50u
#define RV_SUB_OUTLINE 0x60u
/* In the hole slot (layer RV_NONE): */
#define RV_SUB_HOLE_WALL 0x10u
#define RV_SUB_PAD_HOLE 0x20u
#define RV_SUB_VIA_HOLE 0x28u
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RV_H */
