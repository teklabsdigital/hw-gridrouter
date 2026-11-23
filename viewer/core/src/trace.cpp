// trace.cpp: the .grtrace reader.  See trace.hpp for the design and
// docs/TRACE_FORMAT.md for the format; section numbers below refer to it.

#include "trace.hpp"

#include "bytes.hpp"
#include "errors.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace rv {

namespace {

const char kMagic[8] = {'G', 'R', 'T', 'R', 'A', 'C', 'E', '\0'};

const char *const kTags[kKindCount] = {
    "????", "RUN ", "NETS", "PAD ", "RECT", "DISC", "VIA ", "SNAP", "GRDS", "RNB ",
    "RNE ", "SRCH", "CMIT", "RBLD", "RIP ", "DROP", "MARK", "ROUT", "BRD ", "END ",
};

constexpr std::size_t kWindow = 1u << 20;          // read-ahead window while scanning
constexpr std::size_t kWindowMaxRead = 256u << 10; // larger reads bypass the window
// Largest payload read in one piece while indexing (every kind but the
// SRCH trace arrays and the GRDS grids, which are never read eagerly).  A
// larger chunk of those kinds is reported as corrupt rather than read.
constexpr std::uint64_t kMaxEager = 512ull << 20;
constexpr std::int32_t kBlock = 1 << 30; // 4.3

std::string num(double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.9g", v);
    return b;
}

std::string id_text(std::uint32_t v) {
    return v == kNone32 ? std::string("NONE") : std::to_string(v);
}

std::string hex(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// A string from the file, for messages: quoted, non-printable bytes escaped,
// shortened.
std::string quote(const std::string &s) {
    std::string out = "\"";
    std::size_t k = 0;
    for (; k < s.size() && k < 60; ++k) {
        const unsigned char ch = static_cast<unsigned char>(s[k]);
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += static_cast<char>(ch);
        } else if (ch < 0x20 || ch == 0x7F) {
            char b[8];
            std::snprintf(b, sizeof b, "\\x%02x", static_cast<unsigned>(ch));
            out += b;
        } else {
            out += static_cast<char>(ch);
        }
    }
    if (k < s.size()) out += "...";
    out += '"';
    return out;
}

std::string tag_text(const char tag[4]) {
    return std::string(tag, 4);
}

std::string cell_text(std::uint32_t cell, const Run *run) {
    if (run == nullptr || !run->valid || run->nx <= 0 || run->ny <= 0) return std::to_string(cell);
    const std::uint64_t nc = run->nc();
    const std::uint64_t layer = cell / nc;
    const std::uint64_t idx = cell % nc;
    const std::uint64_t nx = static_cast<std::uint64_t>(run->nx);
    return "L" + std::to_string(layer) + "(" + std::to_string(idx % nx) + "," +
           std::to_string(idx / nx) + ")";
}

// --- BRD parsing (7.18), shared by indexing and on-demand loading ----------

void check_layer_index(Cursor &b, std::int32_t index, std::size_t nlayers, const char *what) {
    if (index < 0 || static_cast<std::size_t>(index) >= nlayers)
        b.fail(what, "layer index is not in the layer table");
}

void check_fp(Cursor &b, std::int32_t fp, std::size_t nfp, const char *what) {
    if (fp < -1 || (fp >= 0 && static_cast<std::size_t>(fp) >= nfp))
        b.fail(what, "footprint index is not in the footprint table");
}

void parse_polyset(Cursor &b, const BoardInfo &info, BoardGeometry &geo) {
    Polyset p;
    p.layer_index = b.i32("POLYSET layer_index");
    p.net = b.i32("POLYSET net");
    p.fp = b.i32("POLYSET fp");
    p.role = b.u32("POLYSET role");
    p.sub = b.u32("POLYSET sub");
    const std::uint32_t npoly = b.u32("POLYSET npoly");
    check_layer_index(b, p.layer_index, info.layers.size(), "POLYSET layer_index");
    check_fp(b, p.fp, info.footprints.size(), "POLYSET fp");
    b.need_records(npoly, 4, "POLYSET polygons");
    p.poly_start.reserve(static_cast<std::size_t>(npoly) + 1);
    for (std::uint32_t q = 0; q < npoly; ++q) {
        p.poly_start.push_back(static_cast<std::uint32_t>(p.contour_start.size()));
        const std::uint32_t ncont = b.u32("POLYSET ncont");
        b.need_records(ncont, 4, "POLYSET contours");
        for (std::uint32_t c = 0; c < ncont; ++c) {
            p.contour_start.push_back(static_cast<std::uint32_t>(p.xy.size() / 2));
            const std::uint32_t npts = b.u32("POLYSET npts");
            b.need_records(npts, 8, "POLYSET points");
            const std::size_t at = p.xy.size();
            p.xy.resize(at + 2 * static_cast<std::size_t>(npts));
            for (std::size_t k = 0; k < 2 * static_cast<std::size_t>(npts); ++k)
                p.xy[at + k] = get_i32(b.here() + 4 * k);
            b.skip(8ull * npts, "POLYSET points");
        }
    }
    p.poly_start.push_back(static_cast<std::uint32_t>(p.contour_start.size()));
    p.contour_start.push_back(static_cast<std::uint32_t>(p.xy.size() / 2));
    geo.bytes += sizeof(Polyset) + 4 * (p.poly_start.size() + p.contour_start.size() + p.xy.size());
    geo.order.emplace_back(brd_kind::kPolyset, static_cast<std::uint32_t>(geo.polysets.size()));
    geo.polysets.push_back(std::move(p));
}

void parse_board_record(std::uint32_t kind, Cursor &b, const BoardInfo &info, BoardGeometry &geo) {
    const std::size_t nl = info.layers.size();
    const std::size_t nfp = info.footprints.size();
    switch (kind) {
    case brd_kind::kPolyset:
        parse_polyset(b, info, geo);
        break;
    case brd_kind::kHole: {
        Hole h;
        h.x = b.i32("HOLE x");
        h.y = b.i32("HOLE y");
        h.w = b.i32("HOLE w");
        h.h = b.i32("HOLE h");
        h.rot_deg = b.f64("HOLE rot_deg");
        h.plated = b.u32("HOLE plated");
        h.shape = b.u32("HOLE shape");
        h.fp = b.i32("HOLE fp");
        h.net = b.i32("HOLE net");
        check_fp(b, h.fp, nfp, "HOLE fp");
        geo.bytes += sizeof(Hole);
        geo.order.emplace_back(kind, static_cast<std::uint32_t>(geo.holes.size()));
        geo.holes.push_back(h);
        break;
    }
    case brd_kind::kPadInfo: {
        PadInfo p;
        p.fp = b.i32("PADINFO fp");
        p.net = b.i32("PADINFO net");
        p.x = b.i32("PADINFO x");
        p.y = b.i32("PADINFO y");
        p.attrib = b.u32("PADINFO attrib");
        p.number = b.str("PADINFO number");
        check_fp(b, p.fp, nfp, "PADINFO fp");
        geo.bytes += sizeof(PadInfo) + p.number.size();
        geo.order.emplace_back(kind, static_cast<std::uint32_t>(geo.padinfos.size()));
        geo.padinfos.push_back(std::move(p));
        break;
    }
    case brd_kind::kTrack: {
        BoardTrack t;
        t.layer_index = b.i32("TRACK layer_index");
        t.net = b.i32("TRACK net");
        t.x0 = b.i32("TRACK x0");
        t.y0 = b.i32("TRACK y0");
        t.x1 = b.i32("TRACK x1");
        t.y1 = b.i32("TRACK y1");
        t.w = b.i32("TRACK w");
        check_layer_index(b, t.layer_index, nl, "TRACK layer_index");
        geo.bytes += sizeof(BoardTrack);
        geo.order.emplace_back(kind, static_cast<std::uint32_t>(geo.tracks.size()));
        geo.tracks.push_back(t);
        break;
    }
    case brd_kind::kVia: {
        BoardVia v;
        v.x = b.i32("VIA x");
        v.y = b.i32("VIA y");
        v.dia = b.i32("VIA dia");
        v.drill = b.i32("VIA drill");
        v.net = b.i32("VIA net");
        v.top = b.i32("VIA top_layer_index");
        v.bottom = b.i32("VIA bottom_layer_index");
        check_layer_index(b, v.top, nl, "VIA top_layer_index");
        check_layer_index(b, v.bottom, nl, "VIA bottom_layer_index");
        geo.bytes += sizeof(BoardVia);
        geo.order.emplace_back(kind, static_cast<std::uint32_t>(geo.vias.size()));
        geo.vias.push_back(v);
        break;
    }
    case brd_kind::kText: {
        BoardText t;
        t.layer_index = b.i32("TEXT layer_index");
        t.fp = b.i32("TEXT fp");
        t.x = b.i32("TEXT x");
        t.y = b.i32("TEXT y");
        t.angle_deg = b.f64("TEXT angle_deg");
        t.text = b.str("TEXT text");
        check_layer_index(b, t.layer_index, nl, "TEXT layer_index");
        check_fp(b, t.fp, nfp, "TEXT fp");
        geo.bytes += sizeof(BoardText) + t.text.size();
        geo.order.emplace_back(kind, static_cast<std::uint32_t>(geo.texts.size()));
        geo.texts.push_back(std::move(t));
        break;
    }
    default:
        break; // unknown kinds are skipped (7.18, 11)
    }
}

// Parses a whole BRD payload.  Fills info (tables and counts) and geo.
void parse_board(Cursor &r, BoardInfo &info, BoardGeometry &geo) {
    info.thickness_mm = r.f64("thickness_mm");
    info.origin_x_nm = r.i32("origin_x_nm");
    info.origin_y_nm = r.i32("origin_y_nm");
    info.label = r.str("label");

    std::uint32_t n = r.u32("layer count");
    r.need_records(n, 12, "layers");
    info.layers.reserve(n);
    for (std::uint32_t k = 0; k < n; ++k) {
        BoardLayer l;
        l.kicad_id = r.i32("layer kicad_id");
        l.type = r.i32("layer type");
        l.name = r.str("layer name");
        info.layers.push_back(std::move(l));
    }

    n = r.u32("stackup count");
    r.need_records(n, 20, "stackup");
    info.stackup.reserve(n);
    for (std::uint32_t k = 0; k < n; ++k) {
        StackupItem s;
        r.align8("stackup align8");
        s.thickness_mm = r.f64("stackup thickness_mm");
        s.er = r.f64("stackup er");
        s.name = r.str("stackup name");
        info.stackup.push_back(std::move(s));
    }

    n = r.u32("net count");
    r.need_records(n, 8, "nets");
    info.nets.reserve(n);
    for (std::uint32_t k = 0; k < n; ++k) {
        BoardNet net;
        net.code = r.i32("net code");
        net.name = r.str("net name");
        info.nets.push_back(std::move(net));
    }

    n = r.u32("footprint count");
    r.need_records(n, 36, "footprints");
    info.footprints.reserve(n);
    for (std::uint32_t k = 0; k < n; ++k) {
        Footprint f;
        r.align8("footprint align8");
        f.rot_deg = r.f64("footprint rot_deg");
        f.x = r.i32("footprint x");
        f.y = r.i32("footprint y");
        f.flipped = r.u32("footprint flipped") != 0;
        r.u32("footprint reserved");
        f.ref = r.str("footprint ref");
        f.value = r.str("footprint value");
        f.fpid = r.str("footprint fpid");
        info.footprints.push_back(std::move(f));
    }

    n = r.u32("record count");
    r.align8("records align8");
    r.need_records(n, 8, "records");
    info.nrecords = n;
    for (std::uint32_t k = 0; k < n; ++k) {
        const std::uint32_t kind = r.u32("record kind");
        const std::uint32_t nbytes = r.u32("record nbytes");
        if (nbytes % 8 != 0) r.fail("record nbytes", "is not a multiple of 8");
        r.need(nbytes, "record body");
        Cursor body(r.here(), nbytes, "BRD record");
        parse_board_record(kind, body, info, geo);
        r.skip(nbytes, "record body");
        ++info.records_by_kind[kind < brd_kind::kCount ? kind : 0];
    }
}

} // namespace

const char *kind_tag(Kind kind) noexcept {
    const int k = static_cast<int>(kind);
    return (k >= 0 && k < kKindCount) ? kTags[k] : kTags[0];
}

Kind kind_from_tag(const char tag[4]) noexcept {
    for (int k = 1; k < kKindCount; ++k)
        if (std::memcmp(tag, kTags[k], 4) == 0) return static_cast<Kind>(k);
    return Kind::Unknown;
}

// --- opening, scanning, refreshing ----------------------------------------------

Trace::Trace(std::unique_ptr<Source> source) : src_(std::move(source)) {
    if (!src_) throw std::invalid_argument("no source to read a trace from");
    scan();
}

std::unique_ptr<Trace> Trace::open_file(const std::string &utf8_path) {
    std::unique_ptr<Source> src(new FileSource(utf8_path));
    return std::unique_ptr<Trace>(new Trace(std::move(src)));
}

std::uint32_t Trace::refresh() {
    if (failed()) throw FormatError(failure_);
    const std::size_t before = chunks_.size();
    try {
        scan();
    } catch (const FormatError &e) {
        fail_state(e.what());
        throw;
    } catch (const UnsupportedVersion &e) {
        fail_state(e.what());
        throw;
    } catch (const std::bad_alloc &) {
        fail_state("out of memory while reading the trace");
        throw;
    }
    return static_cast<std::uint32_t>(chunks_.size() - before);
}

void Trace::fail_state(const std::string &message) {
    failure_ = message.empty() ? std::string("the trace can no longer be read") : message;
}

void Trace::issue(Issue::Severity sev, std::uint32_t chunk, std::uint64_t offset,
                  const std::string &msg) {
    Issue is;
    is.severity = sev;
    is.chunk = chunk;
    is.offset = offset;
    is.message = msg;
    issues_.push_back(std::move(is));
    if (sev == Issue::Error) ++errors_;
    else ++warnings_;
}

void Trace::read_exact(std::uint64_t offset, void *buf, std::size_t n) {
    if (n == 0) return;
    const std::size_t got = src_->read_at(offset, buf, n);
    if (got != n)
        throw FormatError("the file ended at offset " + std::to_string(offset + got) +
                          " although " + std::to_string(offset + n) +
                          " bytes were indexed; it shrank or was rewritten");
}

const unsigned char *Trace::window(std::uint64_t offset, std::size_t n) {
    if (win_have_ > 0 && offset >= win_base_ && offset - win_base_ <= win_have_ &&
        n <= win_have_ - static_cast<std::size_t>(offset - win_base_))
        return win_.data() + (offset - win_base_);
    if (win_.size() < kWindow) win_.resize(kWindow);
    win_have_ = 0;
    win_base_ = offset;
    // Callers only ask for bytes inside the size seen at the start of the
    // scan; never read past it (that part may be being written right now).
    const std::uint64_t avail = size_ > offset ? size_ - offset : 0;
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(kWindow, avail));
    if (want < n) throw FormatError("internal: window read outside the file");
    const std::size_t got = src_->read_at(offset, win_.data(), want);
    if (got < n)
        throw FormatError("the file ended at offset " + std::to_string(offset + got) +
                          " although it was " + std::to_string(size_) +
                          " bytes long; it shrank or was rewritten");
    win_have_ = got;
    return win_.data();
}

const std::vector<unsigned char> &Trace::payload(const Chunk &c, std::uint64_t from, std::uint64_t n) {
    if (from > c.len || n > c.len - from)
        throw CorruptChunk(std::string(kind_tag(c.kind)) + ": fields run past the end of the payload");
    if (n > kMaxEager)
        throw CorruptChunk(std::string(kind_tag(c.kind)) + ": payload of " + std::to_string(n) +
                           " bytes is too large to read");
    const std::size_t sz = to_size(n, "payload");
    buf_.resize(sz);
    if (sz == 0) return buf_;
    const std::uint64_t off = c.payload_offset() + from;
    if (sz <= kWindowMaxRead) std::memcpy(buf_.data(), window(off, sz), sz);
    else read_exact(off, buf_.data(), sz);
    return buf_;
}

void Trace::read_header(std::uint64_t size) {
    unsigned char h[24] = {};
    const std::size_t have = static_cast<std::size_t>(std::min<std::uint64_t>(size, 24));
    read_exact(0, h, have);
    if (std::memcmp(h, kMagic, std::min<std::size_t>(have, 8)) != 0)
        throw FormatError("not a gridrouter trace file: it does not start with \"GRTRACE\\0\"");
    if (have >= 10) {
        const std::uint16_t major = get_u16(h + 8);
        if (major != 1) {
            std::string v = std::to_string(major);
            if (have >= 12) v += "." + std::to_string(get_u16(h + 10));
            throw UnsupportedVersion("trace format version " + v +
                                     " is not supported: this reader reads version 1.x");
        }
    }
    if (have < 24) return; // header not written yet (10)
    std::memcpy(header_, h, 24);
    major_ = get_u16(h + 8);
    minor_ = get_u16(h + 10);
    header_flags_ = get_u32(h + 12);
    open_time_ = get_f64(h + 16);
    header_done_ = true;
    scan_off_ = 24;
}

void Trace::check_unchanged() {
    unsigned char h[24];
    read_exact(0, h, 24);
    if (std::memcmp(h, header_, 24) != 0)
        throw FormatError("the file header changed: the file was rewritten since it was opened");
    if (!chunks_.empty()) {
        const Chunk &c = chunks_.back();
        unsigned char x[16];
        read_exact(c.offset, x, 16);
        std::uint64_t tbits;
        std::memcpy(&tbits, &c.t, 8);
        if (std::memcmp(x, c.tag, 4) != 0 || get_u32(x + 4) != c.len || get_u64(x + 8) != tbits)
            throw FormatError("the chunk header at offset " + std::to_string(c.offset) +
                              " changed: the file was rewritten since it was opened");
    }
}

void Trace::scan() {
    win_have_ = 0; // the window never outlives one scan
    const std::uint64_t size = src_->size();
    if (size < size_)
        throw FormatError("the file shrank from " + std::to_string(size_) + " to " +
                          std::to_string(size) + " bytes; it is no longer the trace that was opened");
    size_ = size;

    if (!header_done_) {
        read_header(size_);
        if (!header_done_) return;
    } else {
        check_unchanged();
    }

    if (complete_) {
        // 6 rule 1: nothing follows END.
        if (size_ > scan_off_ && !after_end_warned_) {
            after_end_warned_ = true;
            issue(Issue::Warning, kNone32, scan_off_, "bytes after the END chunk are ignored (6 rule 1)");
        }
        return;
    }
    if (stopped_) return;

    while (size_ - scan_off_ >= 16) {
        const unsigned char *p = window(scan_off_, 16);
        Chunk c;
        c.offset = scan_off_;
        std::memcpy(c.tag, p, 4);
        c.len = get_u32(p + 4);
        c.t = get_f64(p + 8);
        for (int k = 0; k < 4; ++k) {
            const unsigned char ch = static_cast<unsigned char>(c.tag[k]);
            if (ch < 0x20 || ch > 0x7E) {
                stopped_ = true;
                issue(Issue::Error, kNone32, scan_off_,
                      "corrupt chunk header at offset " + std::to_string(scan_off_) + ": tag byte " +
                          hex(ch) + " is not printable ASCII; reading stops here (3.3)");
                return;
            }
        }
        if (c.end_offset() > size_) break; // not written yet (3.3)
        if (chunks_.size() >= kNone32 - 1) {
            stopped_ = true;
            issue(Issue::Error, kNone32, scan_off_, "too many chunks; reading stops here");
            return;
        }
        process(c);
        scan_off_ = c.end_offset();
        if (complete_) {
            if (size_ > scan_off_) {
                after_end_warned_ = true;
                issue(Issue::Warning, kNone32, scan_off_, "bytes after the END chunk are ignored (6 rule 1)");
            }
            break;
        }
    }
    if (buf_.capacity() > (16u << 20)) std::vector<unsigned char>().swap(buf_);
}

void Trace::process(Chunk c) {
    const std::uint32_t idx = static_cast<std::uint32_t>(chunks_.size());
    c.kind = kind_from_tag(c.tag);
    c.session = session_;
    ++kind_counts_[static_cast<int>(c.kind)];

    if (!chunks_.empty() && !(c.t >= chunks_.back().t))
        issue(Issue::Warning, idx, c.offset,
              "chunk time " + num(c.t) + " is earlier than the previous chunk's " +
                  num(chunks_.back().t) + " (6 rule 10)");
    const bool want_snapshot_grids = expect_snapshot_grids_;
    expect_snapshot_grids_ = false;

    try {
        switch (c.kind) {
        case Kind::Run: on_run(c, idx); break;
        case Kind::Nets: on_nets(c, idx); break;
        case Kind::Pad: on_pad(c, idx); break;
        case Kind::Rect: on_rect(c, idx); break;
        case Kind::Disc: on_disc(c, idx); break;
        case Kind::Via: on_via(c, idx); break;
        case Kind::Snap: on_snap(c, idx); break;
        case Kind::Grds: on_grds(c, idx); break;
        case Kind::Rnb: on_rnb(c, idx); break;
        case Kind::Rne: on_rne(c, idx); break;
        case Kind::Srch: on_srch(c, idx); break;
        case Kind::Cmit: on_cmit(c, idx); break;
        case Kind::Rbld: on_rbld(c, idx); break;
        case Kind::Rip: on_rip(c, idx); break;
        case Kind::Drop: on_drop(c, idx); break;
        case Kind::Mark: on_mark(c, idx); break;
        case Kind::Rout: on_rout(c, idx); break;
        case Kind::Brd: on_brd(c, idx); break;
        case Kind::End: on_end(c, idx); break;
        case Kind::Unknown: break; // skipped using len (3.3)
        }
    } catch (const CorruptChunk &e) {
        c.corrupt = true;
        issue(Issue::Error, idx, c.offset,
              "corrupt " + tag_text(c.tag) + " chunk at offset " + std::to_string(c.offset) + ": " +
                  e.what());
        // A corrupt SRCH, RNB or CMIT still takes the next id of its
        // sequence (4.1); its table entry was added before parsing.
        if (c.kind == Kind::Srch) search_ids_.add_unknown();
        else if (c.kind == Kind::Rnb) route_ids_.add_unknown();
        else if (c.kind == Kind::Cmit) copper_.back().id = copper_ids_.add_unknown();
    }

    if (want_snapshot_grids &&
        !(c.kind == Kind::Grds && !c.corrupt && c.record != kNone32 && grids_[c.record].which == 1))
        issue(Issue::Warning, idx, c.offset,
              "SNAP is not immediately followed by a GRDS with which = 1 (6 rule 5)");

    if (c.session != kNone32) runs_[c.session].end_chunk = idx + 1;
    chunks_.push_back(c);
}

const Run &Trace::current_run(const char *what) const {
    if (session_ == kNone32)
        throw CorruptChunk(std::string(what) + " before any RUN chunk (6 rule 2)");
    const Run &run = runs_[session_];
    if (!run.valid)
        throw CorruptChunk(std::string(what) + " belongs to a router session whose RUN chunk is corrupt");
    return run;
}

void Trace::check_route_ref(std::uint32_t route_id, std::uint32_t idx, const Chunk &c, const char *what) {
    const std::uint32_t index = route_id == kNone32 ? kNone32 : route_ids_.index_of(route_id);
    if (route_id != kNone32 && index == kNone32) {
        issue(Issue::Error, idx, c.offset,
              std::string(what) + " refers to route_id " + std::to_string(route_id) +
                  ", which has no RNB chunk");
    } else if (index != open_route_) {
        issue(Issue::Warning, idx, c.offset,
              std::string(what) + " has route_id " + id_text(route_id) +
                  " but the route_net call in progress is " +
                  id_text(open_route_ == kNone32 ? kNone32 : route_ids_.id_of(open_route_)) + " (4.2, 6 rule 6)");
    }
}

void Trace::add_net_chunk(std::int32_t net, std::uint32_t idx) {
    if (net == kBlock) return;
    if (session_ != kNone32 && runs_[session_].valid && net == runs_[session_].block) return;
    net_chunks_[net].push_back(idx);
}

// --- one function per kind --------------------------------------------------------

void Trace::on_run(Chunk &c, std::uint32_t idx) {
    // A RUN starts a new session even if it is corrupt: the chunks after it
    // belong to a new router, not to the previous one (6 rule 3).
    const std::uint32_t sid = static_cast<std::uint32_t>(runs_.size());
    Run placeholder;
    placeholder.chunk = idx;
    placeholder.end_chunk = idx + 1;
    runs_.push_back(std::move(placeholder));
    session_ = sid;
    c.session = sid;
    c.record = sid;
    snapped_ = false;
    since_snap_.clear();
    if (open_route_ != kNone32) {
        issue(Issue::Warning, idx, c.offset,
              "RUN while route_net " + std::to_string(open_route_) + " has no RNE (6 rule 6)");
        open_route_ = kNone32;
    }

    const std::vector<unsigned char> &p = payload(c, 0, c.len);
    Cursor r(p.data(), p.size(), "RUN");
    Run run;
    run.chunk = idx;
    run.end_chunk = idx + 1;
    r.need(168, "fixed part");
    run.nx = r.i32("nx");
    run.ny = r.i32("ny");
    run.nd = r.i32("nd");
    run.block = r.i32("block");
    run.via_cost = r.i32("via_cost");
    run.rip_cost = r.i32("rip_cost");
    run.max_rips = r.i32("max_rips");
    run.h_max = r.i32("h_max");
    run.h_min = r.i32("h_min");
    run.h_off = r.i32("h_off");
    run.r_track[0] = r.i32("r_track");
    run.r_track[1] = r.i32("r_track");
    run.r_wide[0] = r.i32("r_wide");
    run.r_wide[1] = r.i32("r_wide");
    run.r_via[0] = r.i32("r_via");
    run.r_via[1] = r.i32("r_via");
    run.r_via_space = r.i32("r_via_space");
    run.flags = r.u32("flags");
    run.cell = r.f64("cell");
    run.origin_x = r.f64("origin_x");
    run.origin_y = r.f64("origin_y");
    run.track = r.f64("track");
    run.track_wide = r.f64("track_wide");
    run.pad_clear[0] = r.f64("pad_clear");
    run.pad_clear[1] = r.f64("pad_clear");
    run.pad_novia = r.f64("pad_novia");
    run.edge_margin[0] = r.f64("edge_margin");
    run.edge_margin[1] = r.f64("edge_margin");
    run.via_dia = r.f64("via_dia");
    run.via_drill = r.f64("via_drill");
    if (run.nx <= 0 || run.ny <= 0) r.fail("nx, ny", "grid size must be positive");
    // Cells are u32 values < 2 * nx * ny (1.3).
    if (run.ncells() > 0x100000000ull) r.fail("nx, ny", "grid too large for u32 cells");
    if (run.nd < 0 || run.nd > 16) r.fail("nd", "must be 0 to 16");
    if (!(std::isfinite(run.cell) && run.cell > 0)) r.fail("cell", "must be a positive number");
    if (!std::isfinite(run.origin_x) || !std::isfinite(run.origin_y))
        r.fail("origin", "must be finite");
    const std::uint64_t nd = static_cast<std::uint64_t>(run.nd);
    r.i32_array(run.dirs, 3 * nd, "dirs");
    r.i32_array(run.step, 2 * nd, "step");
    r.i32_array(run.turn, (nd + 1) * nd, "turn");
    run.producer = r.str("producer");
    run.layer_name[0] = r.str("layer_name[0]");
    run.layer_name[1] = r.str("layer_name[1]");
    run.valid = true;
    runs_[sid] = std::move(run);
}

void Trace::on_nets(Chunk &c, std::uint32_t idx) {
    const std::vector<unsigned char> &p = payload(c, 0, c.len);
    Cursor r(p.data(), p.size(), "NETS");
    NetsChunk n;
    n.chunk = idx;
    const std::uint32_t count = r.u32("n");
    r.need_records(count, 8, "entries");
    n.entries.reserve(count);
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::int32_t code = r.i32("code");
        std::string name = r.str("name");
        n.entries.emplace_back(code, std::move(name));
    }
    for (const auto &e : n.entries) net_names_[e.first] = e.second; // later replaces (7.2)
    c.record = static_cast<std::uint32_t>(nets_.size());
    nets_.push_back(std::move(n));
}

void Trace::on_pad(Chunk &c, std::uint32_t idx) {
    current_run("PAD");
    const std::vector<unsigned char> &p = payload(c, 0, c.len);
    Cursor r(p.data(), p.size(), "PAD");
    r.need(88, "fixed part");
    Pad d;
    d.chunk = idx;
    d.net = r.i32("net");
    d.layers = r.u32("layers");
    d.power = r.f64("power");
    d.cx = r.f64("cx");
    d.cy = r.f64("cy");
    d.x0 = r.f64("x0");
    d.y0 = r.f64("y0");
    d.x1 = r.f64("x1");
    d.y1 = r.f64("y1");
    d.d_t = r.f64("d_t");
    d.d_w = r.f64("d_w");
    d.d_novia = r.f64("d_novia");
    c.record = static_cast<std::uint32_t>(pads_.size());
    pads_.push_back(d);
    add_net_chunk(d.net, idx);
}

void Trace::on_rect(Chunk &c, std::uint32_t idx) {
    current_run("RECT");
    const std::vector<unsigned char> &p = payload(c, 0, c.len);
    Cursor r(p.data(), p.size(), "RECT");
    r.need(64, "fixed part");
    Rect d;
    d.chunk = idx;
    d.op = r.u32("op");
    d.net = r.i32("net");
    d.layers = r.u32("layers");
    d.levels = r.u32("levels");
    d.x0 = r.f64("x0");
    d.y0 = r.f64("y0");
    d.x1 = r.f64("x1");
    d.y1 = r.f64("y1");
    d.d_t = r.f64("d_t");
    d.d_w = r.f64("d_w");
    c.record = static_cast<std::uint32_t>(rects_.size());
    rects_.push_back(d);
    if (d.op == 0) add_net_chunk(d.net, idx);
}

void Trace::on_disc(Chunk &c, std::uint32_t idx) {
    current_run("DISC");
    const std::vector<unsigned char> &p = payload(c, 0, c.len);
    Cursor r(p.data(), p.size(), "DISC");
    r.need(24, "fixed part");
    Disc d;
    d.chunk = idx;
    d.net = r.i32("net");
    d.layer = r.i32("layer");
    d.i = r.i32("i");
    d.j = r.i32("j");
    d.r_t = r.i32("r_t");
    d.r_w = r.i32("r_w");
    c.record = static_cast<std::uint32_t>(discs_.size());
    discs_.push_back(d);
    add_net_chunk(d.net, idx);
}

void Trace::on_via(Chunk &c, std::uint32_t idx) {
    const Run &run = current_run("VIA");
    const std::vector<unsigned char> &p = payload(c, 0, c.len);
    Cursor r(p.data(), p.size(), "VIA");
    r.need(32, "fixed part");
    Via v;
    v.chunk = idx;
    v.copper_id_field = r.u32("copper_id");
    v.net = r.i32("net");
    v.i = r.i32("i");
    v.j = r.i32("j");
    v.power = r.i32("power");
    v.added = r.u32("added") != 0;
    v.ctx = r.u32("ctx");
    v.route_id = r.u32("route_id");
    // The C core clips a via outside the grid instead of rejecting it, so
    // the chunk is valid; it is not drawn (17 open point 5).
    if (v.i < 0 || v.i >= run.nx || v.j < 0 || v.j >= run.ny) {
        v.off_grid = true;
        issue(Issue::Warning, idx, c.offset,
              "VIA cell (" + std::to_string(v.i) + ", " + std::to_string(v.j) +
                  ") is outside the grid; the via is not drawn (3.4)");
    }

    const std::uint32_t rec = static_cast<std::uint32_t>(vias_.size());
    if (v.added) {
        v.copper = static_cast<std::uint32_t>(copper_.size());
        bool mismatch = false;
        std::uint32_t expected = 0;
        const std::uint32_t id = copper_ids_.add(v.copper_id_field, mismatch, expected);
        if (mismatch)
            issue(Issue::Error, idx, c.offset,
                  "VIA copper_id " + id_text(v.copper_id_field) + ", expected " + std::to_string(expected) +
                      " (4.1); continuing from " + std::to_string(id));
        Copper cu;
        cu.id = id;
        cu.chunk = idx;
        cu.session = session_;
        cu.kind = Kind::Via;
        cu.valid = true;
        cu.net = v.net;
        cu.record = rec;
        copper_.push_back(cu);
        since_snap_[v.net].push_back(v.copper); // an index
    } else if (v.copper_id_field != kNone32) {
        issue(Issue::Error, idx, c.offset,
              "VIA with added = 0 has copper_id " + std::to_string(v.copper_id_field) +
                  ", expected NONE32 (4.1)");
    }
    check_route_ref(v.route_id, idx, c, "VIA");
    c.record = rec;
    vias_.push_back(v);
    add_net_chunk(v.net, idx);
}

void Trace::on_snap(Chunk &c, std::uint32_t idx) {
    current_run("SNAP");
    if (c.len < 4) throw CorruptChunk("SNAP reserved: runs past the end of the payload");
    Snap s;
    s.chunk = idx;
    c.record = static_cast<std::uint32_t>(snaps_.size());
    snaps_.push_back(s);
    snapped_ = true;
    since_snap_.clear();
    expect_snapshot_grids_ = true;
}

void Trace::on_grds(Chunk &c, std::uint32_t idx) {
    const Run &run = current_run("GRDS");
    if (c.len < 12) throw CorruptChunk("GRDS fixed part: runs past the end of the payload");
    const std::uint64_t label_len = get_u32(payload(c, 0, 12).data() + 8);
    const std::uint64_t a = 12 + ((label_len + 3) & ~static_cast<std::uint64_t>(3));
    const std::vector<unsigned char> &p = payload(c, 0, a);
    Cursor r(p.data(), p.size(), "GRDS");
    GridsInfo g;
    g.chunk = idx;
    g.which = r.u32("which");
    g.nc = r.u32("nc");
    g.label = r.str("label");
    if (g.nc != run.nc())
        r.fail("nc", ("is " + std::to_string(g.nc) + " but the RUN grid has " +
                      std::to_string(run.nc()) + " cells")
                         .c_str());
    if (a + 17ull * g.nc > c.len) r.fail("grids", "run past the end of the payload");
    g.data_offset = c.payload_offset() + a;
    c.record = static_cast<std::uint32_t>(grids_.size());
    grids_.push_back(std::move(g));
}

void Trace::on_rnb(Chunk &c, std::uint32_t idx) {
    // Every RNB takes the next route_id, even a corrupt one (4.1).
    const std::uint32_t id = static_cast<std::uint32_t>(routes_.size());
    Route placeholder;
    placeholder.chunk = idx;
    routes_.push_back(std::move(placeholder));
    c.record = id;
    if (open_route_ != kNone32)
        issue(Issue::Warning, idx, c.offset,
              "RNB while route_net " + std::to_string(route_ids_.id_of(open_route_)) + " has no RNE (6 rule 6)");
    open_route_ = id;

    current_run("RNB");
    const std::vector<unsigned char> &p = payload(c, 0, c.len);
    Cursor r(p.data(), p.size(), "RNB");
    r.need(24, "fixed part");
    Route rt;
    rt.chunk = idx;
    rt.route_id_field = r.u32("route_id");
    rt.net = r.i32("net");
    rt.wide = r.u32("wide");
    rt.power = r.i32("power");
    const std::uint32_t npads = r.u32("npads");
    r.u32("reserved");
    r.need_records(npads, 28, "pads"); // 24 bytes of numbers and a str of at least 4 each
    rt.pads.resize(npads);
    for (RoutePad &pad : rt.pads) {
        pad.cx = r.f64("pad cx");
        pad.cy = r.f64("pad cy");
        pad.layers = r.u32("pad layers");
        pad.ncells = r.u32("pad ncells");
    }
    rt.name = r.str("name");
    for (RoutePad &pad : rt.pads) pad.label = r.str("pad label");
    rt.valid = true;
    bool mismatch = false;
    std::uint32_t expected = 0;
    const std::uint32_t rid = route_ids_.add(rt.route_id_field, mismatch, expected);
    if (mismatch)
        issue(Issue::Error, idx, c.offset,
              "RNB route_id " + id_text(rt.route_id_field) + ", expected " + std::to_string(expected) +
                  " (4.1); continuing from " + std::to_string(rid));
    const std::int32_t net = rt.net;
    routes_[id] = std::move(rt);
    add_net_chunk(net, idx);
}

void Trace::on_rne(Chunk &c, std::uint32_t idx) {
    current_run("RNE");
    const std::vector<unsigned char> &p = payload(c, 0, c.len);
    Cursor r(p.data(), p.size(), "RNE");
    r.need(24, "fixed part");
    RouteEnd e;
    e.chunk = idx;
    e.route_id = r.u32("route_id");
    e.net = r.i32("net");
    e.rips_after = r.i32("rips_after");
    const std::uint32_t nripped = r.u32("nripped");
    const std::uint32_t nfailed = r.u32("nfailed");
    e.status = r.u32("status");
    r.need_records(nripped, 4, "ripped");
    e.ripped.reserve(nripped);
    for (std::uint32_t k = 0; k < nripped; ++k) e.ripped.push_back(r.str("ripped name"));
    r.need_records(nfailed, 4, "failed");
    e.failed.reserve(nfailed);
    for (std::uint32_t k = 0; k < nfailed; ++k) e.failed.push_back(r.str("failed label"));

    const std::uint32_t rec = static_cast<std::uint32_t>(route_ends_.size());
    const std::uint32_t ri = e.route_id == kNone32 ? kNone32 : route_ids_.index_of(e.route_id);
    if (ri == kNone32) {
        issue(Issue::Error, idx, c.offset,
              "RNE refers to route_id " + id_text(e.route_id) + ", which has no RNB chunk");
    } else {
        if (ri != open_route_)
            issue(Issue::Warning, idx, c.offset,
                  "RNE for route_id " + std::to_string(e.route_id) +
                      " but the route_net call in progress is " +
                      id_text(open_route_ == kNone32 ? kNone32 : route_ids_.id_of(open_route_)) + " (6 rule 6)");
        Route &rt = routes_[ri];
        if (rt.end != kNone32)
            issue(Issue::Warning, idx, c.offset,
                  "second RNE for route_id " + std::to_string(e.route_id));
        else
            rt.end = rec;
        if (rt.valid && rt.net != e.net)
            issue(Issue::Warning, idx, c.offset,
                  "RNE net " + std::to_string(e.net) + " differs from its RNB net " + std::to_string(rt.net));
    }
    open_route_ = kNone32;
    failed_pads_ += nfailed;
    const std::int32_t net = e.net;
    c.record = rec;
    route_ends_.push_back(std::move(e));
    add_net_chunk(net, idx);
}

void Trace::on_srch(Chunk &c, std::uint32_t idx) {
    // Every SRCH takes the next search_id, even a corrupt one (4.1).
    const std::uint32_t id = static_cast<std::uint32_t>(searches_.size());
    Search placeholder;
    placeholder.chunk = idx;
    searches_.push_back(std::move(placeholder));
    c.record = id;

    const Run &run = current_run("SRCH");
    if (c.len < 96) throw CorruptChunk("SRCH fixed part: runs past the end of the payload");
    Search s;
    {
        const std::vector<unsigned char> &h = payload(c, 0, 96);
        Cursor r(h.data(), h.size(), "SRCH");
        s.chunk = idx;
        s.id_field = r.u32("search_id");
        s.route_id = r.u32("route_id");
        s.ctx = r.u32("ctx");
        s.net = r.i32("net");
        s.flags = r.u32("flags");
        s.gi = r.i32("gi");
        s.gj = r.i32("gj");
        s.rips_at_call = r.i32("rips_at_call");
        s.max_cost = r.i64("max_cost");
        s.cost = r.i64("cost");
        s.expanded = r.i64("expanded");
        s.duration_s = r.f64("duration_s");
        s.nsrc = r.u32("nsrc");
        s.ntgt = r.u32("ntgt");
        s.npath = r.u32("npath");
        s.ntrace = r.u32("ntrace");
        s.ntrace_total = r.u32("ntrace_total");
        s.repeat_of = r.u32("repeat_of");
        s.enc = r.u32("enc");
        r.u32("reserved");
    }
    // Sizes in 64 bits: each count is < 2^32, so no sum below can overflow.
    const std::uint64_t ncells = static_cast<std::uint64_t>(s.nsrc) + s.ntgt + s.npath;
    const std::uint64_t eager = 4 * ncells;
    const bool has_costs = s.has(srch_flags::kHasCosts);
    const std::uint64_t trace_bytes =
        s.enc == 0 ? 4ull * s.ntrace * (has_costs ? 2 : 1) : 0;
    if (96 + eager + trace_bytes > c.len)
        throw CorruptChunk("SRCH arrays (nsrc " + std::to_string(s.nsrc) + ", ntgt " +
                           std::to_string(s.ntgt) + ", npath " + std::to_string(s.npath) +
                           ", ntrace " + std::to_string(s.ntrace) +
                           "): run past the end of the payload");
    std::uint64_t dropped = 0;
    {
        const std::vector<unsigned char> &a = payload(c, 96, eager);
        s.cells.reserve(static_cast<std::size_t>(ncells));
        const std::uint64_t limit = run.ncells();
        const std::uint64_t nends = static_cast<std::uint64_t>(s.nsrc) + s.ntgt;
        const bool error = s.has(srch_flags::kError);
        for (std::size_t k = 0; k < static_cast<std::size_t>(ncells); ++k) {
            const std::uint32_t cell = get_u32(a.data() + 4 * k);
            if (cell >= limit) {
                // A search that raised may hold the invalid sources or
                // targets it raised about (3.4, 17 open point 4): drop them.
                if (error && k < nends) {
                    ++dropped;
                    continue;
                }
                throw CorruptChunk("SRCH cell " + std::to_string(cell) + " is outside the grid (" +
                                   std::to_string(limit) + " cells)");
            }
            if (k < s.nsrc) ++s.nsrc_kept;
            else if (k < nends) ++s.ntgt_kept;
            s.cells.push_back(cell);
        }
    }
    if (dropped > 0)
        issue(Issue::Warning, idx, c.offset,
              "search " + std::to_string(id) + " (error set) has " + std::to_string(dropped) +
                  " source or target cells outside the grid; they are not shown");
    if (s.enc == 0 && s.ntrace > 0) {
        s.trace_cell_offset = c.payload_offset() + 96 + eager;
        if (has_costs) s.trace_d_offset = s.trace_cell_offset + 4ull * s.ntrace;
    }

    // Which arrays hold this search's wavefront (7.11 "Repeats").
    const bool has_trace = s.has(srch_flags::kHasTrace);
    if (s.repeat_of != kNone32) {
        const std::uint32_t t = search_ids_.index_of(s.repeat_of);
        bool ok = t != kNone32 && t < id && searches_[t].valid && searches_[t].repeat_of == kNone32 &&
                  searches_[t].trace_source == t && searches_[t].chunk < chunks_.size() &&
                  chunks_[searches_[t].chunk].session == session_;
        if (!ok) {
            issue(Issue::Warning, idx, c.offset,
                  "search " + std::to_string(id) + " repeats search " + std::to_string(s.repeat_of) +
                      ", which is not an earlier non-repeat search with a wavefront in this session;"
                      " the search has no wavefront");
        } else {
            const Search &ts = searches_[t];
            if (s.ntrace != 0 || s.ntrace_total != ts.ntrace_total ||
                (s.flags & (srch_flags::kHasTrace | srch_flags::kHasCosts)) !=
                    (ts.flags & (srch_flags::kHasTrace | srch_flags::kHasCosts)))
                issue(Issue::Warning, idx, c.offset,
                      "search " + std::to_string(id) + " repeats search " + std::to_string(t) +
                          " but its ntrace, ntrace_total, has_trace or has_costs do not match;"
                          " the wavefront of search " + std::to_string(t) + " is used");
            s.trace_source = t;
        }
    } else if (s.enc == 0 && has_trace) {
        s.trace_source = id;
    } else if (s.enc == 0 && s.ntrace > 0) {
        issue(Issue::Warning, idx, c.offset,
              "search " + std::to_string(id) + " stores " + std::to_string(s.ntrace) +
                  " trace entries without has_trace; they are ignored");
    }
    if (s.ntrace > s.ntrace_total && s.repeat_of == kNone32)
        issue(Issue::Warning, idx, c.offset,
              "search " + std::to_string(id) + " stores ntrace " + std::to_string(s.ntrace) +
                  " > ntrace_total " + std::to_string(s.ntrace_total));
    {
        bool mismatch = false;
        std::uint32_t expected = 0;
        const std::uint32_t sid = search_ids_.add(s.id_field, mismatch, expected);
        if (mismatch)
            issue(Issue::Error, idx, c.offset,
                  "SRCH search_id " + id_text(s.id_field) + ", expected " + std::to_string(expected) +
                      " (4.1); continuing from " + std::to_string(sid));
    }
    check_route_ref(s.route_id, idx, c, "SRCH");

    // expanded is any i64 in a corrupt file: saturate instead of overflowing.
    // trace_entries cannot overflow: each entry takes 4 bytes of the file.
    if (s.expanded > 0)
        expanded_total_ = s.expanded > INT64_MAX - expanded_total_ ? INT64_MAX : expanded_total_ + s.expanded;
    trace_entries_ += s.ntrace;
    s.valid = true;
    const std::int32_t net = s.net;
    searches_[id] = std::move(s);
    add_net_chunk(net, idx);
}

void Trace::on_cmit(Chunk &c, std::uint32_t idx) {
    // Every CMIT takes the next copper_id, even a corrupt one (4.1).
    const std::uint32_t k = static_cast<std::uint32_t>(copper_.size());
    Copper cu;
    cu.chunk = idx;
    cu.session = session_;
    cu.kind = Kind::Cmit;
    copper_.push_back(cu);

    const Run &run = current_run("CMIT");
    const std::vector<unsigned char> &p = payload(c, 0, c.len);
    Cursor r(p.data(), p.size(), "CMIT");
    r.need(48, "fixed part");
    Commit m;
    m.chunk = idx;
    m.copper = k;
    m.copper_id_field = r.u32("copper_id");
    m.net = r.i32("net");
    m.flags = r.u32("flags");
    m.power = r.i32("power");
    m.ctx = r.u32("ctx");
    m.route_id = r.u32("route_id");
    const std::uint32_t npath = r.u32("npath");
    const std::uint32_t nstub = r.u32("nstub");
    const std::uint32_t nvia = r.u32("nvia");
    r.u32("reserved");
    m.width_mm = r.f64("width_mm");
    r.need_records(nstub, 48, "stubs");
    m.stubs.resize(nstub);
    for (Stub &st : m.stubs) {
        st.layer = r.u32("stub layer");
        r.u32("stub reserved");
        st.x0 = r.f64("stub x0");
        st.y0 = r.f64("stub y0");
        st.x1 = r.f64("stub x1");
        st.y1 = r.f64("stub y1");
        st.w = r.f64("stub w");
        if (st.layer > 1) r.fail("stub layer", "must be 0 or 1");
    }
    r.need_records(nvia, 8, "vias");
    m.vias.resize(nvia);
    std::uint32_t off_grid = 0;
    for (GridPoint &v : m.vias) {
        v.i = r.i32("via i");
        v.j = r.i32("via j");
        // A path not in canonical form can make commit() add a via outside
        // the grid; the chunk is valid, the via is not drawn (17 open point 5).
        if (v.i < 0 || v.i >= run.nx || v.j < 0 || v.j >= run.ny) {
            v.off_grid = true;
            ++off_grid;
        }
    }
    r.u32_array(m.path, npath, "path");
    const std::uint64_t limit = run.ncells();
    for (std::uint32_t cell : m.path)
        if (cell >= limit) r.fail("path", "cell is outside the grid");

    if (off_grid > 0)
        issue(Issue::Warning, idx, c.offset,
              "CMIT has " + std::to_string(off_grid) + " vias outside the grid; they are not drawn (3.4)");
    bool mismatch = false;
    std::uint32_t expected = 0;
    copper_[k].id = copper_ids_.add(m.copper_id_field, mismatch, expected);
    if (mismatch)
        issue(Issue::Error, idx, c.offset,
              "CMIT copper_id " + id_text(m.copper_id_field) + ", expected " + std::to_string(expected) +
                  " (4.1); continuing from " + std::to_string(copper_[k].id));
    check_route_ref(m.route_id, idx, c, "CMIT");
    const std::uint32_t rec = static_cast<std::uint32_t>(commits_.size());
    copper_[k].valid = true;
    copper_[k].net = m.net;
    copper_[k].record = rec;
    since_snap_[m.net].push_back(k);
    const std::int32_t net = m.net;
    c.record = rec;
    commits_.push_back(std::move(m));
    add_net_chunk(net, idx);
}

void Trace::on_rbld(Chunk &c, std::uint32_t idx) {
    current_run("RBLD");
    const std::vector<unsigned char> &p = payload(c, 0, c.len);
    Cursor r(p.data(), p.size(), "RBLD");
    Rebuild b;
    b.chunk = idx;
    b.route_id = r.u32("route_id");
    const std::uint32_t n = r.u32("n");
    r.need_records(n, 8, "nets and names");
    r.i32_array(b.nets, n, "nets");
    b.names.reserve(n);
    for (std::uint32_t k = 0; k < n; ++k) b.names.push_back(r.str("name"));
    check_route_ref(b.route_id, idx, c, "RBLD");
    const std::uint32_t route = b.route_id == kNone32 ? kNone32 : route_ids_.index_of(b.route_id);
    c.record = static_cast<std::uint32_t>(rebuilds_.size());
    rebuilds_.push_back(std::move(b));
    if (route < routes_.size() && routes_[route].valid) add_net_chunk(routes_[route].net, idx);
}

void Trace::on_rip(Chunk &c, std::uint32_t idx) {
    current_run("RIP");
    const std::vector<unsigned char> &p = payload(c, 0, c.len);
    Cursor r(p.data(), p.size(), "RIP");
    r.need(16, "fixed part");
    Rip rp;
    rp.chunk = idx;
    rp.net = r.i32("net");
    rp.ctx = r.u32("ctx");
    rp.route_id = r.u32("route_id");
    const std::uint32_t n = r.u32("nremoved");
    r.u32_array(rp.removed, n, "removed");

    for (std::size_t k = 1; k < rp.removed.size(); ++k)
        if (rp.removed[k] <= rp.removed[k - 1]) {
            issue(Issue::Warning, idx, c.offset, "RIP removed ids are not strictly ascending (7.14)");
            break;
        }
    check_route_ref(rp.route_id, idx, c, "RIP");

    // 7.14: the ids this RIP should list are the net's copper of this
    // session created since its last SNAP (since its RUN without one) and
    // not removed yet.  Before the session's first SNAP an empty list is
    // valid too: the router may never have been snapshotted (17 open
    // point 1).  Computed before the removal is applied.
    std::vector<std::uint32_t> expected;
    auto it = since_snap_.find(rp.net);
    if (it != since_snap_.end())
        for (std::uint32_t ix : it->second)
            if (copper_[ix].removed_by == kNone32) expected.push_back(copper_[ix].id);

    // Apply: each listed copper item ends here (8).
    for (std::uint32_t id : rp.removed) {
        const std::uint32_t ix = copper_ids_.index_of(id);
        if (ix == kNone32) {
            issue(Issue::Error, idx, c.offset,
                  "RIP removes copper id " + std::to_string(id) + ", which does not exist");
            continue;
        }
        Copper &cu = copper_[ix];
        if (cu.session != session_) {
            issue(Issue::Error, idx, c.offset,
                  "RIP removes copper id " + std::to_string(id) + " from an earlier router session");
        } else if (cu.removed_by != kNone32) {
            issue(Issue::Error, idx, c.offset,
                  "RIP removes copper id " + std::to_string(id) + ", already removed by chunk " +
                      std::to_string(cu.removed_by));
        } else {
            // Another net's copper of this session is removed too, as 8
            // says, with a warning (17 open point 6).
            if (cu.valid && cu.net != rp.net)
                issue(Issue::Warning, idx, c.offset,
                      "RIP of net " + std::to_string(rp.net) + " removes copper id " + std::to_string(id) +
                          " of net " + std::to_string(cu.net) + " (7.14)");
            cu.removed_by = idx;
            rp.applied.push_back(ix);
            ++copper_removed_;
        }
    }
    const bool exact = rp.removed == expected || (!snapped_ && rp.removed.empty());
    if (!exact) {
        std::string want;
        for (std::size_t k = 0; k < expected.size() && k < 8; ++k) want += (k ? "," : "") + std::to_string(expected[k]);
        if (expected.size() > 8) want += ",...";
        issue(Issue::Warning, idx, c.offset,
              "RIP of net " + std::to_string(rp.net) + " lists " + std::to_string(rp.removed.size()) +
                  " copper ids, but the copper it should remove is {" + want + "}" +
                  (snapped_ ? "" : " or nothing") + " (7.14)");
    }

    const std::int32_t net = rp.net;
    c.record = static_cast<std::uint32_t>(rips_.size());
    rips_.push_back(std::move(rp));
    add_net_chunk(net, idx);
}

void Trace::on_drop(Chunk &c, std::uint32_t idx) {
    current_run("DROP");
    const std::vector<unsigned char> &p = payload(c, 0, c.len);
    Cursor r(p.data(), p.size(), "DROP");
    r.need(16, "fixed part");
    Drop d;
    d.chunk = idx;
    d.net = r.i32("net");
    d.ok = r.u32("ok");
    d.max_cost = r.i64("max_cost");
    d.name = r.str("name");
    d.pad_label = r.str("pad_label");
    if (d.ok == 0) ++drops_failed_;
    else if (d.ok == 2) ++drops_raised_;
    const std::int32_t net = d.net;
    c.record = static_cast<std::uint32_t>(drops_.size());
    drops_.push_back(std::move(d));
    add_net_chunk(net, idx);
}

void Trace::on_mark(Chunk &c, std::uint32_t idx) {
    const std::vector<unsigned char> &p = payload(c, 0, c.len);
    Cursor r(p.data(), p.size(), "MARK");
    Mark m;
    m.chunk = idx;
    m.level = r.u32("level");
    m.text = r.str("text");
    c.record = static_cast<std::uint32_t>(marks_.size());
    marks_.push_back(std::move(m));
}

void Trace::on_rout(Chunk &c, std::uint32_t idx) {
    current_run("ROUT");
    const std::vector<unsigned char> &p = payload(c, 0, c.len);
    Cursor r(p.data(), p.size(), "ROUT");
    Routes o;
    o.chunk = idx;
    const std::uint32_t nseg = r.u32("nseg");
    const std::uint32_t nvia = r.u32("nvia");
    r.need_records(nseg, 48, "segments");
    o.segs.resize(nseg);
    for (RouteSeg &s : o.segs) {
        s.layer = r.u32("segment layer");
        s.net = r.i32("segment net");
        s.x0 = r.f64("segment x0");
        s.y0 = r.f64("segment y0");
        s.x1 = r.f64("segment x1");
        s.y1 = r.f64("segment y1");
        s.w = r.f64("segment w");
        if (s.layer > 1) r.fail("segment layer", "must be 0 or 1");
    }
    r.need_records(nvia, 24, "vias");
    o.vias.resize(nvia);
    for (RouteVia &v : o.vias) {
        v.x = r.f64("via x");
        v.y = r.f64("via y");
        v.net = r.i32("via net");
        r.u32("via reserved");
    }
    c.record = static_cast<std::uint32_t>(routes_out_.size());
    routes_out_.push_back(std::move(o));
}

void Trace::on_brd(Chunk &c, std::uint32_t idx) {
    // Read and validate the whole chunk now, so that loading its geometry
    // later cannot discover corruption; keep only the tables and counts.
    const std::vector<unsigned char> &p = payload(c, 0, c.len);
    Cursor r(p.data(), p.size(), "BRD");
    BoardInfo info;
    info.chunk = idx;
    BoardGeometry scratch;
    parse_board(r, info, scratch);
    c.record = static_cast<std::uint32_t>(boards_.size());
    boards_.push_back(std::move(info));
}

void Trace::on_end(Chunk &c, std::uint32_t idx) {
    const std::vector<unsigned char> &p = payload(c, 0, c.len);
    Cursor r(p.data(), p.size(), "END");
    r.need(40, "fixed part");
    End e;
    e.chunk = idx;
    e.searches = r.u32("searches");
    e.routes = r.u32("routes");
    e.nfailed = r.u32("nfailed");
    e.rips = r.i32("rips");
    e.expanded_total = r.i64("expanded_total");
    e.trace_entries = r.i64("trace_entries");
    e.wall_s = r.f64("wall_s");
    r.need_records(e.nfailed, 8, "failed");
    e.failed.reserve(e.nfailed);
    for (std::uint32_t k = 0; k < e.nfailed; ++k) {
        std::string name = r.str("failed name");
        std::string label = r.str("failed label");
        e.failed.emplace_back(std::move(name), std::move(label));
    }
    complete_ = true;
    if (e.searches != searches_.size())
        issue(Issue::Warning, idx, c.offset,
              "END counts " + std::to_string(e.searches) + " searches, the file has " +
                  std::to_string(searches_.size()));
    if (e.routes != routes_.size())
        issue(Issue::Warning, idx, c.offset,
              "END counts " + std::to_string(e.routes) + " route_net calls, the file has " +
                  std::to_string(routes_.size()));
    if (e.expanded_total != expanded_total_)
        issue(Issue::Warning, idx, c.offset,
              "END expanded_total " + std::to_string(e.expanded_total) + ", the searches sum to " +
                  std::to_string(expanded_total_));
    if (e.trace_entries != trace_entries_)
        issue(Issue::Warning, idx, c.offset,
              "END trace_entries " + std::to_string(e.trace_entries) + ", the searches sum to " +
                  std::to_string(trace_entries_));
    c.record = static_cast<std::uint32_t>(ends_.size());
    ends_.push_back(std::move(e));
}

// --- on demand ---------------------------------------------------------------------

std::shared_ptr<const Wavefront> Trace::wavefront(std::uint32_t search_id) {
    if (search_id >= searches_.size())
        throw std::invalid_argument("no search with id " + std::to_string(search_id));
    const Search &s = searches_[search_id];
    if (!s.valid || s.trace_source == kNone32) return nullptr;
    const std::uint32_t key = s.trace_source;
    if (std::shared_ptr<const Wavefront> hit = wave_cache_.get(key)) return hit;

    const Search &t = searches_[key];
    if (t.chunk >= chunks_.size() || chunks_[t.chunk].session >= runs_.size())
        throw FormatError("search " + std::to_string(key) + " is not fully indexed");
    const Run &run = runs_[chunks_[t.chunk].session];
    auto w = std::make_shared<Wavefront>();
    w->source_search = key;
    w->has_costs = t.has(srch_flags::kHasCosts);
    w->ntrace_total = t.ntrace_total;
    const std::size_t n = t.ntrace;
    if (n > 0) {
        std::vector<unsigned char> raw(to_size(4ull * n, "wavefront"));
        read_exact(t.trace_cell_offset, raw.data(), raw.size());
        w->cells.resize(n);
        const std::uint64_t limit = run.ncells();
        for (std::size_t k = 0; k < n; ++k) {
            const std::uint32_t cell = get_u32(raw.data() + 4 * k);
            if (cell >= limit)
                throw FormatError("search " + std::to_string(key) + ": trace entry " + std::to_string(k) +
                                  " has cell " + std::to_string(cell) + ", outside the grid");
            w->cells[k] = cell;
        }
        if (w->has_costs) {
            read_exact(t.trace_d_offset, raw.data(), raw.size());
            w->costs.resize(n);
            for (std::size_t k = 0; k < n; ++k) w->costs[k] = get_i32(raw.data() + 4 * k);
        }
    }
    wave_cache_.put(key, w, w->bytes());
    return w;
}

std::shared_ptr<const Grids> Trace::grids(std::uint32_t grds_index) {
    if (grds_index >= grids_.size())
        throw std::invalid_argument("no GRDS chunk with index " + std::to_string(grds_index));
    if (std::shared_ptr<const Grids> hit = grids_cache_.get(grds_index)) return hit;
    const GridsInfo &g = grids_[grds_index];
    const std::size_t nc = g.nc;
    auto out = std::make_shared<Grids>();
    out->nc = g.nc;
    std::vector<unsigned char> raw(to_size(17ull * nc, "grids"));
    read_exact(g.data_offset, raw.data(), raw.size());
    std::vector<std::int32_t> *arrays[4] = {&out->t[0], &out->t[1], &out->w[0], &out->w[1]};
    for (int a = 0; a < 4; ++a) {
        arrays[a]->resize(nc);
        const unsigned char *src = raw.data() + 4 * nc * static_cast<std::size_t>(a);
        for (std::size_t k = 0; k < nc; ++k) (*arrays[a])[k] = get_i32(src + 4 * k);
    }
    out->novia.assign(raw.begin() + static_cast<std::ptrdiff_t>(16 * nc), raw.end());
    grids_cache_.put(grds_index, out, 17 * nc);
    return out;
}

std::shared_ptr<const BoardGeometry> Trace::board_geometry(std::uint32_t brd_index) {
    if (brd_index >= boards_.size())
        throw std::invalid_argument("no BRD chunk with index " + std::to_string(brd_index));
    if (std::shared_ptr<const BoardGeometry> hit = board_cache_.get(brd_index)) return hit;
    const Chunk &c = chunks_.at(boards_[brd_index].chunk);
    std::vector<unsigned char> raw(to_size(c.len, "BRD"));
    read_exact(c.payload_offset(), raw.data(), raw.size());
    BoardInfo info;
    auto geo = std::make_shared<BoardGeometry>();
    try {
        Cursor r(raw.data(), raw.size(), "BRD");
        parse_board(r, info, *geo);
    } catch (const CorruptChunk &e) {
        // It validated when indexed, so the file changed underneath.
        throw FormatError(std::string("BRD chunk changed since it was indexed: ") + e.what());
    }
    board_cache_.put(brd_index, geo, geo->bytes);
    return geo;
}

// --- description ------------------------------------------------------------------

std::string Trace::describe(std::uint32_t idx) const {
    if (idx >= chunks_.size()) throw std::invalid_argument("no chunk with index " + std::to_string(idx));
    const Chunk &c = chunks_[idx];
    std::ostringstream o;
    if (c.corrupt) {
        o << "corrupt";
        for (const Issue &is : issues_)
            if (is.chunk == idx && is.severity == Issue::Error) {
                o << ": " << is.message;
                break;
            }
        return o.str();
    }
    const Run *run = c.session < runs_.size() ? &runs_[c.session] : nullptr;
    const std::uint32_t k = c.record;
    switch (c.kind) {
    case Kind::Unknown:
        o << "unknown tag " << quote(tag_text(c.tag)) << ", skipped";
        break;
    case Kind::Run: {
        const Run &x = runs_[k];
        o << "session " << k << ": grid " << x.nx << " x " << x.ny << ", nd " << x.nd << ", cell "
          << num(x.cell) << " mm, origin (" << num(x.origin_x) << ", " << num(x.origin_y)
          << ") mm, via_cost " << x.via_cost << ", rip_cost " << x.rip_cost << ", max_rips "
          << x.max_rips << ", flags " << hex(x.flags) << ", layers " << quote(x.layer_name[0])
          << "/" << quote(x.layer_name[1]) << ", producer " << quote(x.producer);
        break;
    }
    case Kind::Nets: {
        const NetsChunk &x = nets_[k];
        o << x.entries.size() << " net names";
        for (std::size_t e = 0; e < x.entries.size() && e < 4; ++e)
            o << (e ? ", " : ": ") << x.entries[e].first << "=" << quote(x.entries[e].second);
        if (x.entries.size() > 4) o << ", ...";
        break;
    }
    case Kind::Pad: {
        const Pad &x = pads_[k];
        o << "add_pad net " << x.net << " layers " << hex(x.layers) << " centre (" << num(x.cx)
          << ", " << num(x.cy) << ") rect (" << num(x.x0) << ", " << num(x.y0) << ")-(" << num(x.x1)
          << ", " << num(x.y1) << ") power " << num(x.power);
        break;
    }
    case Kind::Rect: {
        const Rect &x = rects_[k];
        static const char *const ops[3] = {"claim_rect", "block_rect", "block_cells"};
        o << (x.op < 3 ? ops[x.op] : "op ?") << " net " << x.net << " layers " << hex(x.layers)
          << " levels " << hex(x.levels) << " (" << num(x.x0) << ", " << num(x.y0) << ")-("
          << num(x.x1) << ", " << num(x.y1) << ")";
        break;
    }
    case Kind::Disc: {
        const Disc &x = discs_[k];
        o << "claim_disc net " << x.net << " layer " << x.layer << " cell (" << x.i << ", " << x.j
          << ") r_t " << x.r_t << " r_w " << x.r_w;
        break;
    }
    case Kind::Via: {
        const Via &x = vias_[k];
        o << "add_via net " << x.net << " cell (" << x.i << ", " << x.j << ") power " << x.power
          << (x.added ? " added, copper " + std::to_string(copper_[x.copper].id) : std::string(" not added"))
          << ", ctx " << x.ctx << ", route " << id_text(x.route_id) << (x.off_grid ? ", outside the grid" : "");
        break;
    }
    case Kind::Snap:
        o << "snapshot";
        break;
    case Kind::Grds: {
        const GridsInfo &x = grids_[k];
        o << (x.which == 0 ? "live grids" : x.which == 1 ? "snapshot grids" : "grids, which ?")
          << ", nc " << x.nc << ", label " << quote(x.label);
        break;
    }
    case Kind::Rnb: {
        const Route &x = routes_[k];
        o << "route_net " << k << " begins: net " << x.net << " " << quote(x.name) << ", "
          << x.pads.size() << " pads, wide " << x.wide << ", power " << x.power;
        break;
    }
    case Kind::Rne: {
        const RouteEnd &x = route_ends_[k];
        o << "route_net " << id_text(x.route_id) << " ends: net " << x.net << ", "
          << (x.status == 0 ? "returned" : "raised") << ", rips_after " << x.rips_after << ", "
          << x.ripped.size() << " ripped, " << x.failed.size() << " failed";
        break;
    }
    case Kind::Srch: {
        const Search &x = searches_[k];
        o << "search " << k << ": net " << x.net << ", ctx " << x.ctx << ", route "
          << id_text(x.route_id) << ", "
          << (x.has(srch_flags::kError) ? "error" : x.has(srch_flags::kFound) ? "found" : "not found")
          << ", cost " << x.cost << ", expanded " << x.expanded << ", src " << x.nsrc << ", tgt "
          << x.ntgt << ", path " << x.npath << ", trace " << x.ntrace << "/" << x.ntrace_total;
        if (x.repeat_of != kNone32) o << ", repeat of " << x.repeat_of;
        if (x.enc != 0) o << ", enc " << x.enc;
        o << ", " << num(x.duration_s) << " s";
        break;
    }
    case Kind::Cmit: {
        const Commit &x = commits_[k];
        o << "commit, copper " << copper_[x.copper].id << ": net " << x.net << ", ctx " << x.ctx << ", route "
          << id_text(x.route_id) << ", path " << x.path.size() << ", " << x.stubs.size()
          << " stubs, " << x.vias.size() << " vias, width " << num(x.width_mm) << " mm, flags "
          << hex(x.flags);
        if (!x.path.empty())
            o << ", " << cell_text(x.path.front(), run) << " to " << cell_text(x.path.back(), run);
        break;
    }
    case Kind::Rbld: {
        const Rebuild &x = rebuilds_[k];
        o << "rebuild, route " << id_text(x.route_id) << ", drops " << x.names.size();
        for (std::size_t e = 0; e < x.names.size() && e < 4; ++e)
            o << (e ? ", " : ": ") << quote(x.names[e]) << " (net " << x.nets[e] << ")";
        if (x.names.size() > 4) o << ", ...";
        break;
    }
    case Kind::Rip: {
        const Rip &x = rips_[k];
        o << "rip net " << x.net << ", ctx " << x.ctx << ", route " << id_text(x.route_id)
          << ", removes " << x.removed.size() << " copper";
        for (std::size_t e = 0; e < x.removed.size() && e < 8; ++e) o << (e ? "," : ": ") << x.removed[e];
        if (x.removed.size() > 8) o << ",...";
        break;
    }
    case Kind::Drop: {
        const Drop &x = drops_[k];
        o << "drop_to_plane net " << x.net << " " << quote(x.name) << " pad " << quote(x.pad_label)
          << ": " << (x.ok == 1 ? "ok" : x.ok == 0 ? "failed" : x.ok == 2 ? "raised" : "ok ?")
          << ", max_cost " << x.max_cost;
        break;
    }
    case Kind::Mark: {
        const Mark &x = marks_[k];
        o << (x.level == 0 ? "phase " : x.level == 1 ? "note " : "mark ") << quote(x.text);
        break;
    }
    case Kind::Rout: {
        const Routes &x = routes_out_[k];
        o << "routes(): " << x.segs.size() << " segments, " << x.vias.size() << " vias";
        break;
    }
    case Kind::Brd: {
        const BoardInfo &x = boards_[k];
        o << "board " << quote(x.label) << ": " << x.layers.size() << " layers, " << x.stackup.size()
          << " stackup items, " << x.nets.size() << " nets, " << x.footprints.size()
          << " footprints, " << x.nrecords << " records";
        break;
    }
    case Kind::End: {
        const End &x = ends_[k];
        o << "end: " << x.searches << " searches, " << x.routes << " routes, " << x.nfailed
          << " failed, rips " << x.rips << ", expanded " << x.expanded_total << ", trace entries "
          << x.trace_entries << ", wall " << num(x.wall_s) << " s";
        break;
    }
    }
    return o.str();
}

} // namespace rv
