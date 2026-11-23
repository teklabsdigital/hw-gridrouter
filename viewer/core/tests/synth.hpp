// synth.hpp: a synthetic .grtrace writer, for tests only.
//
// Written from docs/TRACE_FORMAT.md alone, independently of the reader in
// core/src (it shares no code or types with it) and of the Python recorder.
// The W* structs mirror the spec's field tables; encode() turns each into a
// chunk payload; Writer frames payloads into chunks.  build_sample() makes a
// small file that uses every chunk kind, with every id and order rule kept,
// and returns the structs it wrote so tests can compare field by field.
#ifndef RV_TESTS_SYNTH_HPP
#define RV_TESTS_SYNTH_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace synth {

constexpr std::uint32_t NONE32 = 0xFFFFFFFFu;
constexpr std::int32_t BLOCK = 1 << 30;

// Little-endian byte builder.  Offsets are relative to its own start, which
// is a payload start, so align8() pads to the payload's own 8-byte grid.
class Bytes {
public:
    std::vector<unsigned char> b;

    Bytes &u8(std::uint8_t v);
    Bytes &u16(std::uint16_t v);
    Bytes &u32(std::uint32_t v);
    Bytes &i32(std::int32_t v);
    Bytes &u64(std::uint64_t v);
    Bytes &i64(std::int64_t v);
    Bytes &f64(double v);
    Bytes &chars(const char *s, std::size_t n);
    Bytes &str(const std::string &s); // u32 n, bytes, zero pad to 4
    Bytes &zeros(std::size_t n);
    Bytes &align8();                  // 0 or 4 zero bytes
    Bytes &append(const Bytes &other);
    std::size_t size() const { return b.size(); }
};

// --- chunk contents, one struct per tag (spec section 7) ---------------------

struct WRun {
    std::int32_t nx = 6, ny = 5, nd = 4, block = BLOCK, via_cost = 400, rip_cost = 1000, max_rips = 600;
    std::int32_t h_max = 10, h_min = 10, h_off = 0;
    std::int32_t r_track[2] = {1, 2}, r_wide[2] = {2, 3}, r_via[2] = {2, 3}, r_via_space = 1;
    std::uint32_t flags = 0;
    double cell = 0.25, origin_x = 100.5, origin_y = 50.25, track = 0.2, track_wide = 0.5;
    double pad_clear[2] = {0.2, 0.3}, pad_novia = 0.1, edge_margin[2] = {0, 0};
    double via_dia = 0.6, via_drill = 0.3;
    std::vector<std::int32_t> dirs, step, turn; // 3nd, 2nd, (nd+1)nd
    std::string producer = "synth test writer";
    std::string layer_name[2] = {"F.Cu", "B.Cu"};
};

struct WNets {
    std::vector<std::pair<std::int32_t, std::string>> entries;
};

struct WPad {
    std::int32_t net = 0;
    std::uint32_t layers = 0;
    double power = 0, cx = 0, cy = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0, d_t = 0, d_w = 0, d_novia = 0;
};

struct WRect {
    std::uint32_t op = 0;
    std::int32_t net = 0;
    std::uint32_t layers = 0, levels = 0;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0, d_t = 0, d_w = 0;
};

struct WDisc {
    std::int32_t net = 0, layer = 0, i = 0, j = 0, r_t = 0, r_w = 0;
};

struct WVia {
    std::uint32_t copper_id = NONE32;
    std::int32_t net = 0, i = 0, j = 0, power = 0;
    std::uint32_t added = 0, ctx = 0, route_id = NONE32;
};

struct WGrds {
    std::uint32_t which = 0, nc = 0;
    std::string label;
    std::vector<std::int32_t> t0, t1, w0, w1;
    std::vector<std::uint8_t> novia;
};

struct WRnbPad {
    double cx = 0, cy = 0;
    std::uint32_t layers = 0, ncells = 0;
    std::string label;
};

struct WRnb {
    std::uint32_t route_id = 0;
    std::int32_t net = 0;
    std::uint32_t wide = 0;
    std::int32_t power = 0;
    std::vector<WRnbPad> pads;
    std::string name;
};

struct WRne {
    std::uint32_t route_id = 0;
    std::int32_t net = 0, rips_after = 0;
    std::uint32_t status = 0;
    std::vector<std::string> ripped, failed;
};

struct WSrch {
    std::uint32_t search_id = 0, route_id = NONE32, ctx = 0;
    std::int32_t net = 0;
    std::uint32_t flags = 0;
    std::int32_t gi = 0, gj = 0, rips_at_call = 0;
    std::int64_t max_cost = -1, cost = -1, expanded = 0;
    double duration_s = 0;
    std::vector<std::uint32_t> src, tgt, path, trace_cell; // ntrace = trace_cell.size()
    std::vector<std::int32_t> trace_d;                     // written when flags has bit 7
    std::uint32_t ntrace_total = 0, repeat_of = NONE32, enc = 0;
    std::vector<unsigned char> enc_data; // written after path when enc != 0
};

struct WStub {
    std::uint32_t layer = 0;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0, w = 0;
};

struct WCmit {
    std::uint32_t copper_id = 0;
    std::int32_t net = 0;
    std::uint32_t flags = 0;
    std::int32_t power = 0;
    std::uint32_t ctx = 0, route_id = NONE32;
    double width_mm = 0;
    std::vector<WStub> stubs;
    std::vector<std::pair<std::int32_t, std::int32_t>> vias;
    std::vector<std::uint32_t> path;
};

struct WRbld {
    std::uint32_t route_id = NONE32;
    std::vector<std::int32_t> nets;
    std::vector<std::string> names;
};

struct WRip {
    std::int32_t net = 0;
    std::uint32_t ctx = 0, route_id = NONE32;
    std::vector<std::uint32_t> removed;
};

struct WDrop {
    std::int32_t net = 0;
    std::uint32_t ok = 1;
    std::int64_t max_cost = -1;
    std::string name, pad_label;
};

struct WMark {
    std::uint32_t level = 0;
    std::string text;
};

struct WRoutSeg {
    std::uint32_t layer = 0;
    std::int32_t net = 0;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0, w = 0;
};

struct WRoutVia {
    double x = 0, y = 0;
    std::int32_t net = 0;
};

struct WRout {
    std::vector<WRoutSeg> segs;
    std::vector<WRoutVia> vias;
};

// BRD records (7.18).
struct WPolyset {
    std::int32_t layer_index = 0, net = -1, fp = -1;
    std::uint32_t role = 0, sub = 0;
    // polygons -> contours -> points (x, y)
    std::vector<std::vector<std::vector<std::pair<std::int32_t, std::int32_t>>>> polys;
};
struct WHole {
    std::int32_t x = 0, y = 0, w = 0, h = 0;
    double rot_deg = 0;
    std::uint32_t plated = 0, shape = 0;
    std::int32_t fp = -1, net = -1;
};
struct WPadInfo {
    std::int32_t fp = 0, net = -1, x = 0, y = 0;
    std::uint32_t attrib = 0;
    std::string number;
};
struct WTrack {
    std::int32_t layer_index = 0, net = -1, x0 = 0, y0 = 0, x1 = 0, y1 = 0, w = 0;
};
struct WBVia {
    std::int32_t x = 0, y = 0, dia = 0, drill = 0, net = -1, top = 0, bottom = 0;
};
struct WText {
    std::int32_t layer_index = 0, fp = -1, x = 0, y = 0;
    double angle_deg = 0;
    std::string text;
};
struct WFootprint {
    double rot_deg = 0;
    std::int32_t x = 0, y = 0;
    std::uint32_t flipped = 0;
    std::string ref, value, fpid;
};
struct WLayer {
    std::int32_t kicad_id = 0, type = -1;
    std::string name;
};
struct WStackup {
    double thickness_mm = 0, er = 0;
    std::string name;
};

struct WBrd {
    double thickness_mm = 1.6;
    std::int32_t origin_x_nm = 0, origin_y_nm = 0;
    std::string label;
    std::vector<WLayer> layers;
    std::vector<WStackup> stackup;
    std::vector<std::pair<std::int32_t, std::string>> nets;
    std::vector<WFootprint> footprints;
    std::vector<WPolyset> polysets;
    std::vector<WHole> holes;
    std::vector<WPadInfo> padinfos;
    std::vector<WTrack> tracks;
    std::vector<WBVia> vias;
    std::vector<WText> texts;
    // Records in file order: (kind, index into the vector of that kind).
    // A kind not in 1..6 writes an opaque record of `index` bytes (rounded
    // up to 8) that a reader must skip.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> order;
};

struct WEnd {
    std::uint32_t searches = 0, routes = 0, nfailed = 0;
    std::int32_t rips = 0;
    std::int64_t expanded_total = 0, trace_entries = 0;
    double wall_s = 0;
    std::vector<std::pair<std::string, std::string>> failed; // nfailed is taken from its size
};

Bytes encode(const WRun &x);
Bytes encode(const WNets &x);
Bytes encode(const WPad &x);
Bytes encode(const WRect &x);
Bytes encode(const WDisc &x);
Bytes encode(const WVia &x);
Bytes encode_snap();
Bytes encode(const WGrds &x);
Bytes encode(const WRnb &x);
Bytes encode(const WRne &x);
Bytes encode(const WSrch &x);
Bytes encode(const WCmit &x);
Bytes encode(const WRbld &x);
Bytes encode(const WRip &x);
Bytes encode(const WDrop &x);
Bytes encode(const WMark &x);
Bytes encode(const WRout &x);
Bytes encode(const WBrd &x);
Bytes encode(const WEnd &x);

// A RUN with consistent dirs/step/turn arrays for its nd.
WRun make_run(std::int32_t nx, std::int32_t ny, std::int32_t nd);
// GRDS arrays filled with a recognisable pattern.
WGrds make_grids(std::uint32_t which, std::uint32_t nc, const std::string &label);

// Frames chunks (3.1) after a file header (2).
class Writer {
public:
    explicit Writer(double open_time = 1795392000.25, std::uint16_t major = 1,
                    std::uint16_t minor = 0, std::uint32_t flags = 0);

    // Appends one chunk; t defaults to the previous t plus 0.125 s.
    void chunk(const char tag[4], const Bytes &payload);
    void chunk_at(const char tag[4], const Bytes &payload, double t);
    // Appends raw bytes (for building corrupt files).
    void raw(const Bytes &bytes);

    std::vector<unsigned char> out;
    std::vector<std::size_t> chunk_offsets;
    double t = 0;
};

// The sample: every chunk kind of 1.0, two router sessions, an unknown tag,
// and an END.  Every id, count and order rule of the spec is kept, so a
// correct reader reports no issues on it.
struct Sample {
    std::vector<unsigned char> bytes;
    std::vector<std::size_t> chunk_offsets;
    std::vector<std::string> tags;   // tag of each chunk, in order
    std::vector<double> times;       // t of each chunk
    double open_time = 0;
    std::vector<WRun> runs;
    std::vector<WNets> nets;
    std::vector<WPad> pads;
    std::vector<WRect> rects;
    std::vector<WDisc> discs;
    std::vector<WVia> vias;
    std::vector<WGrds> grds;
    std::vector<WRnb> rnbs;
    std::vector<WRne> rnes;
    std::vector<WSrch> srchs;
    std::vector<WCmit> cmits;
    std::vector<WRbld> rblds;
    std::vector<WRip> rips;
    std::vector<WDrop> drops;
    std::vector<WMark> marks;
    std::vector<WRout> routs;
    std::vector<WBrd> brds;
    std::vector<WEnd> ends;
    std::size_t unknown_chunks = 0;
};

Sample build_sample(bool with_end = true);

// --- the M2 stress board ---------------------------------------------------------
//
// A synthetic board at least as heavy as the dongle's, for the drawing
// performance gate and the golden images: 100 x 72 mm, 4 copper layers,
// 120 footprints with 450 pads (shapes on copper, mask and paste), two
// full-board zone fills (In1.Cu, B.Cu) with 400 clearance holes each,
// about 1500 track segments, 80 vias, 152 texts as glyph outline polygons,
// silkscreen, fab and courtyard graphics, mounting holes and a keep-out.
// Deterministic.  routed = false leaves out zones, tracks and vias (the
// "placed" board).
WBrd stress_board(const std::string &label, bool routed);

// A complete trace with the stress board twice: BRD "placed", BRD "final",
// then END.
std::vector<unsigned char> build_stress_trace();

// Glyph outline polygons of a text in a small stroke font, as KiCad's
// TransformTextToPolySet gives them: one stadium polygon per stroke.
// (x, y) is the centre of the text, height the cap height, angle counter-
// clockwise on screen in degrees; mirror for back-side text.  Appended to
// polys.
void text_polygons(const std::string &text, double x_nm, double y_nm, double height_nm, double angle_deg,
                   bool mirror, std::vector<std::vector<std::vector<std::pair<std::int32_t, std::int32_t>>>> &polys);

// Writes bytes to a file; returns false on failure.
bool write_file(const std::string &path, const std::vector<unsigned char> &bytes);

// --- controlled timelines (M3) ----------------------------------------------------
//
// A writer that keeps the id sequences and the RIP rule of 7.14 itself, so a
// test can describe a routing run call by call.  It also remembers which
// copper is alive (its own replay, independent of the reader), so it can
// write a "final" board whose tracks and vias are exactly that copper.

struct Cell {
    std::uint32_t layer = 0;
    std::int32_t i = 0, j = 0;
};

class TimelineWriter {
public:
    Writer w;
    WRun run;
    std::uint32_t next_copper = 0, next_search = 0, next_route = 0;
    // Per net, the copper ids since the session's last SNAP (or RUN) that no
    // RIP listed yet: what a RIP of that net lists.
    std::map<std::int32_t, std::vector<std::uint32_t>> since_snap;
    // Alive copper of the current session: id -> what it draws.
    struct Item {
        std::int32_t net = 0;
        std::vector<Cell> path;
        std::vector<std::pair<std::int32_t, std::int32_t>> vias;
        std::vector<WStub> stubs;
        double width = 0;
    };
    std::map<std::uint32_t, Item> alive;
    std::vector<WPad> pads; // of the current session
    std::int64_t expanded_total = 0;
    // M4: with wavefronts = true, srch() also stores a wavefront (has_trace,
    // has_costs): an A* expansion over the empty grid (8 directions, cost 10
    // straight, 14 diagonal, 50 for a via; heuristic 0.6 x octile distance
    // to the nearest target) in pop order, at most min(expanded, 2 * nc)
    // cells.  A search with a path stops when it pops a target; one without
    // never reaches its targets (they count as blocked) and runs to the
    // budget.  The search's own fields (expanded, path) are unchanged, so the
    // timeline is the same with or without wavefronts.
    bool wavefronts = false;
    std::int64_t trace_entries = 0;
    std::vector<WSrch> written; // every SRCH of the file, by search_id

    void nets(const std::vector<std::pair<std::int32_t, std::string>> &entries);
    // RUN: an nx x ny grid, 8 directions, cell 0.25 mm at (100.5, 50.25).
    void start(std::int32_t nx, std::int32_t ny);
    std::uint32_t cell(const Cell &c) const;
    std::uint32_t commit(std::int32_t net, const std::vector<Cell> &path, double width = 0.2,
                         const std::vector<std::pair<std::int32_t, std::int32_t>> &vias = {},
                         const std::vector<WStub> &stubs = {}, std::uint32_t ctx = 0, std::uint32_t route = NONE32);
    std::uint32_t via(std::int32_t net, std::int32_t i, std::int32_t j); // added = 1
    // RIP of a net: lists since_snap[net] (plus extra ids, then sorted).
    void rip(std::int32_t net, std::uint32_t ctx = 0, std::uint32_t route = NONE32,
             const std::vector<std::uint32_t> &extra = {});
    void snap(); // SNAP and its GRDS
    std::uint32_t rnb(std::int32_t net, const std::string &name, const std::vector<WRnbPad> &pads);
    void rne(std::uint32_t route, std::int32_t net, const std::vector<std::string> &failed);
    std::uint32_t srch(std::int32_t net, const std::vector<Cell> &src, const std::vector<Cell> &tgt,
                       const std::vector<Cell> &path, std::int64_t expanded, std::uint32_t ctx = 0,
                       std::uint32_t route = NONE32);
    // A repeat of search `of` (repeat_of, 4.x Repeats): same sources,
    // targets, path and expanded; no stored arrays.
    std::uint32_t srch_repeat(std::uint32_t of, std::uint32_t ctx = 0, std::uint32_t route = NONE32);
    void rbld(std::uint32_t route, const std::vector<std::string> &names, const std::vector<std::int32_t> &nets);
    void drop(std::int32_t net, std::uint32_t ok, const std::string &name, const std::string &label);
    void mark(std::uint32_t level, const std::string &text);
    void pad(std::int32_t net, double cx, double cy, double half);
    void brd(const WBrd &b);
    // ROUT of the alive copper: every same-layer step of every path as a
    // segment, the stubs, and the vias.
    void rout();
    void end(const std::vector<std::pair<std::string, std::string>> &failed);

    // A board for this run: F.Cu, B.Cu, Edge.Cuts (the grid extent as the
    // outline); with routed = true, the alive copper as tracks and vias.
    WBrd board(const std::string &label, bool routed) const;
};

// The M3 sample: a "placed" board, then two router sessions with phases,
// route_net calls, commits, standalone vias, a SNAP, direct and rebuild
// rip-ups (one listing another net's copper), failed pads from RNE and
// DROP, a ROUT, and a "final" board holding the first session's routing...
// written after the second session ends, so its routing is that of the
// second session (see the function for the exact sequence).
std::vector<unsigned char> build_timeline_sample();

} // namespace synth

#endif // RV_TESTS_SYNTH_HPP
