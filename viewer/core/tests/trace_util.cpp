// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// trace_util.cpp: helpers shared by the reader tests.

#include "trace_util.hpp"

#include "errors.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <sstream>
#include <system_error>

namespace rvtest {

std::unique_ptr<rv::Trace> open_bytes(const std::vector<unsigned char> &bytes) {
    auto data = std::make_shared<std::vector<unsigned char>>(bytes);
    return open_shared(data);
}

std::unique_ptr<rv::Trace> open_shared(const std::shared_ptr<std::vector<unsigned char>> &bytes) {
    std::unique_ptr<rv::Source> src(new rv::MemorySource(bytes));
    return std::unique_ptr<rv::Trace>(new rv::Trace(std::move(src)));
}

namespace {

std::string bits(double v) {
    std::uint64_t u;
    std::memcpy(&u, &v, 8);
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(u));
    return b;
}

} // namespace

std::string fingerprint(const rv::Trace &t) {
    std::ostringstream o;
    o << "header " << t.header_complete() << " " << t.major() << "." << t.minor() << " flags "
      << t.header_flags() << " open " << bits(t.open_time()) << "\n";
    o << "state complete " << t.complete() << " stopped " << t.stopped() << " failed " << t.failed()
      << " size " << t.file_size() << " indexed " << t.indexed_end() << "\n";
    for (std::size_t k = 0; k < t.chunks().size(); ++k) {
        const rv::Chunk &c = t.chunks()[k];
        o << "chunk " << k << " @" << c.offset << " " << std::string(c.tag, 4) << " len " << c.len
          << " t " << bits(c.t) << " kind " << static_cast<int>(c.kind) << " corrupt " << c.corrupt
          << " record " << c.record << " session " << c.session << " | "
          << t.describe(static_cast<std::uint32_t>(k)) << "\n";
    }
    for (const rv::Issue &is : t.issues())
        o << "issue " << static_cast<int>(is.severity) << " chunk " << is.chunk << " @" << is.offset
          << " " << is.message << "\n";
    for (std::size_t k = 0; k < t.runs().size(); ++k)
        o << "run " << k << " valid " << t.runs()[k].valid << " chunk " << t.runs()[k].chunk
          << " end " << t.runs()[k].end_chunk << "\n";
    for (std::size_t k = 0; k < t.routes().size(); ++k)
        o << "route " << k << " valid " << t.routes()[k].valid << " end " << t.routes()[k].end << "\n";
    for (std::size_t k = 0; k < t.searches().size(); ++k) {
        const rv::Search &s = t.searches()[k];
        o << "search " << k << " valid " << s.valid << " source " << s.trace_source << " cells";
        for (std::uint32_t c : s.cells) o << " " << c;
        o << " offs " << s.trace_cell_offset << " " << s.trace_d_offset << "\n";
    }
    for (std::size_t k = 0; k < t.copper().size(); ++k) {
        const rv::Copper &c = t.copper()[k];
        o << "copper " << k << " id " << c.id << " chunk " << c.chunk << " session " << c.session << " kind "
          << static_cast<int>(c.kind) << " valid " << c.valid << " net " << c.net << " record "
          << c.record << " removed_by " << c.removed_by << "\n";
    }
    for (const auto &n : t.net_names()) o << "name " << n.first << " " << n.second << "\n";
    for (const auto &n : t.net_chunks()) {
        o << "netchunks " << n.first << ":";
        for (std::uint32_t c : n.second) o << " " << c;
        o << "\n";
    }
    for (int k = 0; k < rv::kKindCount; ++k) o << "count " << k << " " << t.kind_count(static_cast<rv::Kind>(k)) << "\n";
    o << "totals " << t.expanded_total() << " " << t.trace_entries() << " " << t.failed_pads() << " "
      << t.drops_failed() << " " << t.drops_raised() << " " << t.copper_removed() << "\n";
    return o.str();
}

namespace {

template <class Table>
bool record_ok(const rv::Trace &t, const Table &table, std::uint32_t record, std::uint32_t chunk) {
    return record < table.size() && table[record].chunk == chunk && chunk < t.chunks().size();
}

std::size_t table_size(const rv::Trace &t, rv::Kind kind) {
    switch (kind) {
    case rv::Kind::Run: return t.runs().size();
    case rv::Kind::Nets: return t.nets_chunks().size();
    case rv::Kind::Pad: return t.pads().size();
    case rv::Kind::Rect: return t.rects().size();
    case rv::Kind::Disc: return t.discs().size();
    case rv::Kind::Via: return t.vias().size();
    case rv::Kind::Snap: return t.snaps().size();
    case rv::Kind::Grds: return t.grids_infos().size();
    case rv::Kind::Rnb: return t.routes().size();
    case rv::Kind::Rne: return t.route_ends().size();
    case rv::Kind::Srch: return t.searches().size();
    case rv::Kind::Cmit: return t.commits().size();
    case rv::Kind::Rbld: return t.rebuilds().size();
    case rv::Kind::Rip: return t.rips().size();
    case rv::Kind::Drop: return t.drops().size();
    case rv::Kind::Mark: return t.marks().size();
    case rv::Kind::Rout: return t.routes_outputs().size();
    case rv::Kind::Brd: return t.boards().size();
    case rv::Kind::End: return t.ends().size();
    case rv::Kind::Unknown: return 0;
    }
    return 0;
}

} // namespace

std::string invariant_failure(const rv::Trace &t) {
    const auto &chunks = t.chunks();
    std::uint64_t expect_off = 24;
    for (std::size_t k = 0; k < chunks.size(); ++k) {
        const rv::Chunk &c = chunks[k];
        const std::string at = "chunk " + std::to_string(k) + ": ";
        if (c.offset != expect_off) return at + "not contiguous";
        if (c.end_offset() > t.file_size()) return at + "ends past the file";
        expect_off = c.end_offset();
        if (rv::kind_from_tag(c.tag) != c.kind) return at + "kind does not match tag";
        if (c.session != rv::kNone32 && c.session >= t.runs().size()) return at + "bad session";
        const bool every = c.kind == rv::Kind::Run || c.kind == rv::Kind::Srch || c.kind == rv::Kind::Rnb;
        if (c.kind == rv::Kind::Unknown) {
            if (c.record != rv::kNone32) return at + "unknown chunk has a record";
        } else if (every) {
            if (c.record >= table_size(t, c.kind)) return at + "record out of range";
        } else if (c.corrupt) {
            if (c.record != rv::kNone32) return at + "corrupt chunk has a record";
        } else if (c.record >= table_size(t, c.kind)) {
            return at + "record out of range";
        }
    }
    if (t.header_complete() && t.indexed_end() != expect_off) return "indexed_end mismatch";
    for (int k = 1; k < rv::kKindCount; ++k) {
        const rv::Kind kind = static_cast<rv::Kind>(k);
        if (kind == rv::Kind::Run || kind == rv::Kind::Srch || kind == rv::Kind::Rnb)
            if (table_size(t, kind) != t.kind_count(kind)) return "table size != kind count";
    }
    for (std::size_t k = 0; k < t.searches().size(); ++k) {
        const rv::Search &s = t.searches()[k];
        if (s.chunk >= chunks.size() || chunks[s.chunk].record != k) return "search chunk link";
        if (s.valid) {
            if (s.cells.size() != static_cast<std::size_t>(s.nsrc_kept) + s.ntgt_kept + s.npath) return "search cells size";
            if (s.nsrc_kept > s.nsrc || s.ntgt_kept > s.ntgt) return "search kept counts";
            if (s.trace_source != rv::kNone32) {
                if (s.trace_source > k) return "trace_source after search";
                if (t.searches()[s.trace_source].trace_source != s.trace_source) return "trace_source chain";
            }
        }
    }
    for (std::size_t k = 0; k < t.copper().size(); ++k) {
        const rv::Copper &c = t.copper()[k];
        if (c.chunk >= chunks.size()) return "copper chunk";
        if (c.removed_by != rv::kNone32) {
            if (c.removed_by <= c.chunk || c.removed_by >= chunks.size()) return "copper removed_by order";
            if (chunks[c.removed_by].kind != rv::Kind::Rip) return "copper removed_by kind";
        }
        if (c.valid) {
            if (c.kind == rv::Kind::Cmit && !record_ok(t, t.commits(), c.record, c.chunk)) return "copper commit link";
            if (c.kind == rv::Kind::Via && !record_ok(t, t.vias(), c.record, c.chunk)) return "copper via link";
        }
    }
    for (const auto &n : t.net_chunks())
        for (std::uint32_t c : n.second)
            if (c >= chunks.size()) return "net chunk index";
    for (const rv::Route &r : t.routes())
        if (r.end != rv::kNone32 && r.end >= t.route_ends().size()) return "route end index";
    if (t.error_count() + t.warning_count() != t.issues().size()) return "issue counts";
    return "";
}

int exercise(rv::Trace &t) {
    int failures = 0;
    for (std::size_t k = 0; k < t.chunks().size(); ++k) (void)t.describe(static_cast<std::uint32_t>(k));
    for (std::size_t k = 0; k < t.searches().size(); ++k) {
        try {
            auto w = t.wavefront(static_cast<std::uint32_t>(k));
            if (w) {
                const rv::Search &src = t.searches()[w->source_search];
                if (w->cells.size() != src.ntrace) throw std::logic_error("wavefront length");
                if (w->has_costs && w->costs.size() != w->cells.size()) throw std::logic_error("costs length");
            }
        } catch (const rv::FormatError &) {
            ++failures;
        }
    }
    for (std::size_t k = 0; k < t.grids_infos().size(); ++k) {
        try {
            auto g = t.grids(static_cast<std::uint32_t>(k));
            if (g->novia.size() != g->nc || g->t[0].size() != g->nc) throw std::logic_error("grid size");
        } catch (const rv::FormatError &) {
            ++failures;
        }
    }
    for (std::size_t k = 0; k < t.boards().size(); ++k) {
        try {
            (void)t.board_geometry(static_cast<std::uint32_t>(k));
        } catch (const rv::FormatError &) {
            ++failures;
        }
    }
    return failures;
}

TempDir::TempDir() {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) return;
    std::random_device rd;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::filesystem::path p = base / ("rv_tests_" + std::to_string(rd()) + std::to_string(attempt));
        if (std::filesystem::create_directory(p, ec) && !ec) {
            dir_ = p.string();
            return;
        }
    }
}

TempDir::~TempDir() {
    if (dir_.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
}

std::string TempDir::path(const std::string &name) const {
    return (std::filesystem::path(dir_) / name).string();
}

bool append_file(const std::string &path, const unsigned char *data, std::size_t n) {
    std::FILE *f = std::fopen(path.c_str(), "ab");
    if (f == nullptr) return false;
    const std::size_t w = n == 0 ? 0 : std::fwrite(data, 1, n, f);
    const bool ok = w == n;
    return std::fclose(f) == 0 && ok;
}

} // namespace rvtest
