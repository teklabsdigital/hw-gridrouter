// records.hpp: the typed contents of a .grtrace file.
//
// One struct per chunk kind of docs/TRACE_FORMAT.md section 7, plus the
// chunk index entry and the derived tables.  Field names follow the spec.
// Lengths in router-grid mm, board coordinates in nm, costs in router units,
// times in seconds, exactly as stored.
#ifndef RV_RECORDS_HPP
#define RV_RECORDS_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rv {

constexpr std::uint32_t kNone32 = 0xFFFFFFFFu;

// Chunk kinds.  The numeric values are those of rv_chunk_kind in rv.h.
enum class Kind : std::uint8_t {
    Unknown = 0,
    Run, Nets, Pad, Rect, Disc, Via, Snap, Grds, Rnb, Rne,
    Srch, Cmit, Rbld, Rip, Drop, Mark, Rout, Brd, End,
};
constexpr int kKindCount = 20;

// The four tag bytes of a kind ("RUN ", "SRCH", ...); "????" for Unknown.
const char *kind_tag(Kind kind) noexcept;
// The kind of a tag, Unknown if the tag is not one of version 1.0.
Kind kind_from_tag(const char tag[4]) noexcept;

// One entry of the chunk index, in file order.  The index is the event
// sequence: chunk k is event k.
struct Chunk {
    std::uint64_t offset = 0;       // file offset of the 16-byte chunk header
    double t = 0;                   // chunk time, seconds since the file was opened
    std::uint32_t len = 0;          // payload length, excluding padding
    std::uint32_t record = kNone32; // index into the table of this kind (see Trace)
    std::uint32_t session = kNone32;// index into Trace::runs(): latest RUN at or before
    char tag[4] = {0, 0, 0, 0};
    Kind kind = Kind::Unknown;
    bool corrupt = false;           // payload did not fit its layout; see issues
    bool has_net = false;           // M6: the chunk is in Trace::net_chunks() under `net`
    std::int32_t net = 0;

    std::uint64_t payload_offset() const { return offset + 16; }
    std::uint64_t end_offset() const { return offset + 16 + len + (8 - len % 8) % 8; }
};

// --- RUN (7.1); one per router session -------------------------------------

namespace run_flags {
constexpr std::uint32_t kStrict = 1u << 0;
constexpr std::uint32_t kSkipJoinedStubs = 1u << 1;
constexpr std::uint32_t kHasEdgeMargin = 1u << 2;
} // namespace run_flags

struct Run {
    bool valid = false;               // false: the RUN chunk was corrupt
    std::uint32_t chunk = kNone32;    // the RUN chunk
    std::uint32_t end_chunk = 0;      // one past the last chunk of this session so far
    std::int32_t nx = 0, ny = 0, nd = 0, block = 0, via_cost = 0, rip_cost = 0, max_rips = 0;
    std::int32_t h_max = 0, h_min = 0, h_off = 0;
    std::int32_t r_track[2] = {0, 0}, r_wide[2] = {0, 0}, r_via[2] = {0, 0}, r_via_space = 0;
    std::uint32_t flags = 0;
    double cell = 0, origin_x = 0, origin_y = 0, track = 0, track_wide = 0;
    double pad_clear[2] = {0, 0}, pad_novia = 0, edge_margin[2] = {0, 0};
    double via_dia = 0, via_drill = 0;
    std::vector<std::int32_t> dirs; // 3 * nd: dx, dy, base_cost
    std::vector<std::int32_t> step; // 2 * nd
    std::vector<std::int32_t> turn; // (nd + 1) * nd
    std::string producer;
    std::string layer_name[2];

    std::uint64_t nc() const { return static_cast<std::uint64_t>(nx) * static_cast<std::uint64_t>(ny); }
    std::uint64_t ncells() const { return 2 * nc(); } // valid cell values are < this
};

// --- NETS (7.2) -------------------------------------------------------------

struct NetsChunk {
    std::uint32_t chunk = kNone32;
    std::vector<std::pair<std::int32_t, std::string>> entries;
};

// --- PAD (7.3), RECT (7.4), DISC (7.5), VIA (7.6) ----------------------------

struct Pad {
    std::uint32_t chunk = kNone32;
    std::int32_t net = 0;
    std::uint32_t layers = 0;
    double power = 0, cx = 0, cy = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0, d_t = 0, d_w = 0, d_novia = 0;
};

struct Rect {
    std::uint32_t chunk = kNone32;
    std::uint32_t op = 0; // 0 claim_rect, 1 block_rect, 2 block_cells
    std::int32_t net = 0;
    std::uint32_t layers = 0, levels = 0;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0, d_t = 0, d_w = 0;
};

struct Disc {
    std::uint32_t chunk = kNone32;
    std::int32_t net = 0, layer = 0, i = 0, j = 0, r_t = 0, r_w = 0;
};

struct Via {
    std::uint32_t chunk = kNone32;
    std::uint32_t copper_id_field = kNone32; // as stored
    std::uint32_t copper = kNone32;          // index into Trace::copper() if added, else kNone32
    std::int32_t net = 0, i = 0, j = 0, power = 0;
    bool added = false;
    bool off_grid = false; // (i, j) outside the grid: valid, but not drawn (17 open point 5)
    std::uint32_t ctx = 0, route_id = kNone32;
};

// --- SNAP (7.7), GRDS (7.8) -------------------------------------------------

struct Snap {
    std::uint32_t chunk = kNone32;
};

struct GridsInfo {
    std::uint32_t chunk = kNone32;
    std::uint32_t which = 0; // 0 live, 1 snapshot
    std::uint32_t nc = 0;
    std::string label;
    std::uint64_t data_offset = 0; // file offset of T0
};

// The five grids of one GRDS chunk, loaded on demand.
struct Grids {
    std::uint32_t nc = 0;
    std::vector<std::int32_t> t[2]; // thin-track level, layers 0 and 1
    std::vector<std::int32_t> w[2]; // wide-track / via level
    std::vector<std::uint8_t> novia;
};

// --- RNB (7.9), RNE (7.10): one Route per RNB --------------------------------

struct RoutePad {
    double cx = 0, cy = 0;
    std::uint32_t layers = 0, ncells = 0;
    std::string label;
};

struct Route {
    bool valid = false;                // false: the RNB chunk was corrupt
    std::uint32_t chunk = kNone32;     // the RNB chunk
    std::uint32_t route_id_field = kNone32;
    std::int32_t net = 0;
    std::uint32_t wide = 0;
    std::int32_t power = 0;
    std::vector<RoutePad> pads;
    std::string name;
    std::uint32_t end = kNone32;       // index into Trace::route_ends(), once seen
};

struct RouteEnd {
    std::uint32_t chunk = kNone32;
    std::uint32_t route_id = kNone32;
    std::int32_t net = 0, rips_after = 0;
    std::uint32_t status = 0;
    std::vector<std::string> ripped, failed;
};

// --- SRCH (7.11) -------------------------------------------------------------

namespace srch_flags {
constexpr std::uint32_t kWide = 1u << 0;
constexpr std::uint32_t kHasGoal = 1u << 1;
constexpr std::uint32_t kHasMax = 1u << 2;
constexpr std::uint32_t kViaGoal = 1u << 3;
constexpr std::uint32_t kRelax = 1u << 4;
constexpr std::uint32_t kFound = 1u << 5;
constexpr std::uint32_t kNewVia = 1u << 6;
constexpr std::uint32_t kHasCosts = 1u << 7;
constexpr std::uint32_t kError = 1u << 8;
constexpr std::uint32_t kHasTrace = 1u << 9;
} // namespace srch_flags

// One search.  The fixed header and the source, target and path cells are
// read when the chunk is indexed; the wavefront (trace arrays) is read on
// demand through Trace::wavefront().
struct Search {
    bool valid = false;               // false: the SRCH chunk was corrupt
    std::uint32_t chunk = kNone32;
    std::uint32_t id_field = kNone32; // search_id as stored
    std::uint32_t route_id = kNone32, ctx = 0;
    std::int32_t net = 0;
    std::uint32_t flags = 0;
    std::int32_t gi = 0, gj = 0, rips_at_call = 0;
    std::int64_t max_cost = -1, cost = -1, expanded = -1;
    double duration_s = 0;
    std::uint32_t nsrc = 0, ntgt = 0, npath = 0, ntrace = 0, ntrace_total = 0; // as stored
    std::uint32_t repeat_of = kNone32, enc = 0;
    // Source and target cells kept in `cells`.  Equal to nsrc and ntgt,
    // except in a search with `error` set whose sources or targets hold
    // cells outside the grid: those are dropped (17 open point 4).
    std::uint32_t nsrc_kept = 0, ntgt_kept = 0;
    std::vector<std::uint32_t> cells;   // kept src, then kept tgt, then path
    std::uint64_t trace_cell_offset = 0; // file offsets of this chunk's own arrays (0 = none)
    std::uint64_t trace_d_offset = 0;
    // The search whose stored arrays are this search's wavefront: itself,
    // its repeat_of, or kNone32 when it has no usable wavefront.
    std::uint32_t trace_source = kNone32;

    bool has(std::uint32_t flag) const { return (flags & flag) != 0; }
    const std::uint32_t *src() const { return cells.data(); }
    const std::uint32_t *tgt() const { return cells.data() + nsrc_kept; }
    const std::uint32_t *path() const { return cells.data() + nsrc_kept + ntgt_kept; }
};

// A search's wavefront, loaded on demand and shared through the cache.
struct Wavefront {
    std::uint32_t source_search = kNone32; // the search whose arrays these are
    std::vector<std::uint32_t> cells;      // trace_cell, pop order
    std::vector<std::int32_t> costs;       // trace_d; empty without has_costs
    bool has_costs = false;
    std::uint32_t ntrace_total = 0;
    std::size_t bytes() const { return cells.size() * 4 + costs.size() * 4; }
};

// --- CMIT (7.12) ---------------------------------------------------------------

namespace cmit_flags {
constexpr std::uint32_t kWide = 1u << 0;
constexpr std::uint32_t kEndVia = 1u << 1;
constexpr std::uint32_t kSkipJoinedStubs = 1u << 2;
} // namespace cmit_flags

struct Stub {
    std::uint32_t layer = 0;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0, w = 0;
};

struct GridPoint {
    std::int32_t i = 0, j = 0;
    bool off_grid = false; // outside the grid: valid, but not drawn (17 open point 5)
};

struct Commit {
    std::uint32_t chunk = kNone32;
    std::uint32_t copper_id_field = kNone32; // as stored
    std::uint32_t copper = kNone32;          // index into Trace::copper()
    std::int32_t net = 0;
    std::uint32_t flags = 0;
    std::int32_t power = 0;
    std::uint32_t ctx = 0, route_id = kNone32;
    double width_mm = 0;
    std::vector<Stub> stubs;
    std::vector<GridPoint> vias;
    std::vector<std::uint32_t> path;
};

// --- RBLD (7.13), RIP (7.14), DROP (7.15), MARK (7.16) ----------------------

struct Rebuild {
    std::uint32_t chunk = kNone32;
    std::uint32_t route_id = kNone32;
    std::vector<std::int32_t> nets;
    std::vector<std::string> names;
};

struct Rip {
    std::uint32_t chunk = kNone32;
    std::int32_t net = 0;
    std::uint32_t ctx = 0, route_id = kNone32;
    std::vector<std::uint32_t> removed; // copper ids as stored
    std::vector<std::uint32_t> applied; // indices into Trace::copper() this RIP removed
};

struct Drop {
    std::uint32_t chunk = kNone32;
    std::int32_t net = 0;
    std::uint32_t ok = 0; // 1 True, 0 False, 2 raised
    std::int64_t max_cost = -1;
    std::string name, pad_label;
};

struct Mark {
    std::uint32_t chunk = kNone32;
    std::uint32_t level = 0; // 0 phase, 1 note
    std::string text;
};

// --- ROUT (7.17) ---------------------------------------------------------------

struct RouteSeg {
    std::uint32_t layer = 0;
    std::int32_t net = 0;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0, w = 0;
};

struct RouteVia {
    double x = 0, y = 0;
    std::int32_t net = 0;
};

struct Routes {
    std::uint32_t chunk = kNone32;
    std::vector<RouteSeg> segs;
    std::vector<RouteVia> vias;
};

// --- BRD (7.18) ----------------------------------------------------------------

struct BoardLayer {
    std::int32_t kicad_id = 0, type = 0;
    std::string name;
};

struct StackupItem {
    double thickness_mm = 0, er = 0;
    std::string name;
};

struct BoardNet {
    std::int32_t code = 0;
    std::string name;
};

struct Footprint {
    double rot_deg = 0;
    std::int32_t x = 0, y = 0;
    bool flipped = false;
    std::string ref, value, fpid;
};

// BRD record kinds.
namespace brd_kind {
constexpr std::uint32_t kPolyset = 1, kHole = 2, kPadInfo = 3, kTrack = 4, kVia = 5, kText = 6;
constexpr std::uint32_t kCount = 7; // index 0 counts unknown kinds
} // namespace brd_kind

// Tables and counts of one BRD chunk, read when the chunk is indexed.  The
// whole chunk is validated then; the geometry is kept on disk and decoded on
// demand into a BoardGeometry.
struct BoardInfo {
    std::uint32_t chunk = kNone32;
    double thickness_mm = 0;
    std::int32_t origin_x_nm = 0, origin_y_nm = 0;
    std::string label;
    std::vector<BoardLayer> layers;
    std::vector<StackupItem> stackup;
    std::vector<BoardNet> nets;
    std::vector<Footprint> footprints;
    std::uint32_t nrecords = 0;
    std::uint32_t records_by_kind[brd_kind::kCount] = {0, 0, 0, 0, 0, 0, 0};
};

struct Polyset {
    std::int32_t layer_index = 0, net = -1, fp = -1;
    std::uint32_t role = 0, sub = 0;
    // Polygon p has contours [poly_start[p], poly_start[p + 1]); contour c
    // has points [contour_start[c], contour_start[c + 1]); point k is
    // (xy[2k], xy[2k + 1]).  Both start arrays have a final end entry.
    std::vector<std::uint32_t> poly_start;
    std::vector<std::uint32_t> contour_start;
    std::vector<std::int32_t> xy;
};

struct Hole {
    std::int32_t x = 0, y = 0, w = 0, h = 0;
    double rot_deg = 0;
    std::uint32_t plated = 0, shape = 0;
    std::int32_t fp = -1, net = -1;
};

struct PadInfo {
    std::int32_t fp = -1, net = -1, x = 0, y = 0;
    std::uint32_t attrib = 0;
    std::string number;
};

struct BoardTrack {
    std::int32_t layer_index = 0, net = -1, x0 = 0, y0 = 0, x1 = 0, y1 = 0, w = 0;
};

struct BoardVia {
    std::int32_t x = 0, y = 0, dia = 0, drill = 0, net = -1, top = 0, bottom = 0;
};

struct BoardText {
    std::int32_t layer_index = 0, fp = -1, x = 0, y = 0;
    double angle_deg = 0;
    std::string text;
};

struct BoardGeometry {
    std::vector<Polyset> polysets;
    std::vector<Hole> holes;
    std::vector<PadInfo> padinfos;
    std::vector<BoardTrack> tracks;
    std::vector<BoardVia> vias;
    std::vector<BoardText> texts;
    // File order of the known records: (kind, index into its vector).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> order;
    std::size_t bytes = 0; // approximate memory use, for the cache
};

// --- END (7.19) ----------------------------------------------------------------

struct End {
    std::uint32_t chunk = kNone32;
    std::uint32_t searches = 0, routes = 0, nfailed = 0;
    std::int32_t rips = 0;
    std::int64_t expanded_total = 0, trace_entries = 0;
    double wall_s = 0;
    std::vector<std::pair<std::string, std::string>> failed;
};

// --- derived -----------------------------------------------------------------------

// One copper item: a CMIT, or a VIA with added = 1, in file order.  `id` is
// its copper_id: the one the rule of 4.1 assigns, except after an id
// mismatch, where the reader reports one error and continues from the
// stored id (17 open point 3).  In a file without mismatches the index into
// Trace::copper() equals the id.
struct Copper {
    std::uint32_t id = kNone32;
    std::uint32_t chunk = kNone32;      // creating chunk
    std::uint32_t session = kNone32;
    Kind kind = Kind::Unknown;          // Cmit or Via
    bool valid = false;                 // false: the creating CMIT was corrupt
    std::int32_t net = 0;
    std::uint32_t record = kNone32;     // index into commits() or vias()
    std::uint32_t removed_by = kNone32; // the RIP chunk that removed it, if any
};

// A problem found while reading.  Errors are corruption (a chunk that does
// not fit its layout, a broken id sequence, a reference to nothing);
// warnings are departures from the order guarantees of section 6 that do not
// stop a chunk from being understood.
struct Issue {
    enum Severity : std::uint8_t { Warning = 1, Error = 2 };
    Severity severity = Error;
    std::uint32_t chunk = kNone32; // kNone32 for problems not tied to one chunk
    std::uint64_t offset = 0;      // file offset the problem refers to
    std::string message;
};

} // namespace rv

#endif // RV_RECORDS_HPP
