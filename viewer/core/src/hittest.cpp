// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// hittest.cpp: what is under a point of the view (hittest.hpp).

#include "hittest.hpp"

#include "inspect.hpp"
#include "playback.hpp"
#include "timeline.hpp"
#include "trace.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>

namespace rv {

namespace {

std::string fmt(const char *f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

double seg_dist(double px, double py, double x0, double y0, double x1, double y1) {
    const double dx = x1 - x0, dy = y1 - y0;
    const double l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? ((px - x0) * dx + (py - y0) * dy) / l2 : 0;
    t = std::max(0.0, std::min(1.0, t));
    const double ex = x0 + t * dx - px, ey = y0 + t * dy - py;
    return std::sqrt(ex * ex + ey * ey);
}

bool in_box(const float b[4], double x, double y, double pad) {
    return x >= b[0] - pad && x <= b[2] + pad && y >= b[1] - pad && y <= b[3] + pad;
}

constexpr std::uint32_t kPadsBit = 1u << static_cast<std::uint32_t>(ObjClass::Pads);
constexpr std::uint32_t kZonesBit = 1u << static_cast<std::uint32_t>(ObjClass::Zones);
constexpr std::uint32_t kTracksBit = 1u << static_cast<std::uint32_t>(ObjClass::Tracks);
constexpr std::uint32_t kViasBit = 1u << static_cast<std::uint32_t>(ObjClass::Vias);

// Point (x, y) (mm) against one polygon of a polyset: inside (even-odd over
// its contours) gives 0, else the distance to its nearest edge.
double polygon_dist(const Polyset &p, std::uint32_t poly, double x, double y) {
    const double px = x * 1e6, py = y * 1e6; // nm
    bool inside = false;
    double best = std::numeric_limits<double>::infinity();
    for (std::uint32_t c = p.poly_start[poly]; c < p.poly_start[poly + 1]; ++c) {
        const std::uint32_t a = p.contour_start[c], e = p.contour_start[c + 1];
        if (e - a < 2) continue;
        for (std::uint32_t k = a; k < e; ++k) {
            const std::uint32_t n = k + 1 == e ? a : k + 1;
            const double x0 = p.xy[2 * static_cast<std::size_t>(k)], y0 = p.xy[2 * static_cast<std::size_t>(k) + 1];
            const double x1 = p.xy[2 * static_cast<std::size_t>(n)], y1 = p.xy[2 * static_cast<std::size_t>(n) + 1];
            if ((y0 > py) != (y1 > py) && px < x0 + (py - y0) * (x1 - x0) / (y1 - y0)) inside = !inside;
            best = std::min(best, seg_dist(px, py, x0, y0, x1, y1));
        }
    }
    return inside ? 0.0 : best * 1e-6;
}

} // namespace

void Picker::reset() {
    board_ = kNone32;
    board_list_ = nullptr;
    geo_.reset();
    shapes_.clear();
    board_nets_.clear();
    wave_source_ = kNone32;
    wave_.reset();
    wave_first_.clear();
    wave_pairs_.clear();
}

std::uint32_t Picker::first_position(std::uint64_t cell) const {
    if (!wave_first_.empty()) return cell < wave_first_.size() ? wave_first_[cell] : kNone32;
    if (cell > 0xFFFFFFFFull) return kNone32;
    const std::pair<std::uint32_t, std::uint32_t> key(static_cast<std::uint32_t>(cell), 0u);
    const auto it = std::lower_bound(wave_pairs_.begin(), wave_pairs_.end(), key);
    return it != wave_pairs_.end() && it->first == key.first ? it->second : kNone32;
}

void Picker::index_board(const PickInput &in) {
    if (in.board == board_ && in.board_list == board_list_ && (geo_ || in.board == kNone32)) return;
    shapes_.clear();
    board_nets_.clear();
    geo_.reset();
    board_ = in.board;
    board_list_ = in.board_list;
    if (in.trace == nullptr || in.board == kNone32 || in.board >= in.trace->boards().size() || in.board_list == nullptr)
        return;
    geo_ = in.trace->board_geometry(in.board);
    for (const BoardNet &n : in.trace->boards()[in.board].nets) board_nets_[n.code] = n.name;
    std::vector<const LayerEntry *> by_index;
    for (const LayerEntry &le : in.board_list->layers) {
        if (by_index.size() <= le.index) by_index.resize(le.index + 1, nullptr);
        by_index[le.index] = &le;
    }
    auto layer = [&](std::int32_t i) -> const LayerEntry * {
        return i >= 0 && static_cast<std::size_t>(i) < by_index.size() ? by_index[static_cast<std::size_t>(i)] : nullptr;
    };
    const float inf = std::numeric_limits<float>::infinity();
    for (std::uint32_t k = 0; k < geo_->polysets.size(); ++k) {
        const Polyset &p = geo_->polysets[k];
        const LayerEntry *le = layer(p.layer_index);
        if (le == nullptr || le->cls.kind != LayerKind::Copper || (p.role != 0 && p.role != 3)) continue;
        Shape s;
        s.kind = p.role == 0 ? hitkind::Pad : hitkind::Zone;
        s.record = k;
        s.layer = le->index;
        s.order = order_key(le->rank, p.role == 0 ? sub::kPad : sub::kZone);
        float b[4] = {inf, inf, -inf, -inf};
        for (std::size_t q = 0; q + 1 < p.xy.size(); q += 2) {
            const float x = static_cast<float>(p.xy[q] * 1e-6), y = static_cast<float>(p.xy[q + 1] * 1e-6);
            b[0] = std::min(b[0], x);
            b[1] = std::min(b[1], y);
            b[2] = std::max(b[2], x);
            b[3] = std::max(b[3], y);
        }
        if (!(b[0] <= b[2])) continue;
        std::copy(b, b + 4, s.bbox);
        shapes_.push_back(s);
    }
    for (std::uint32_t k = 0; k < geo_->tracks.size(); ++k) {
        const BoardTrack &t = geo_->tracks[k];
        const LayerEntry *le = layer(t.layer_index);
        if (le == nullptr || le->cls.kind != LayerKind::Copper) continue;
        Shape s;
        s.kind = hitkind::Track;
        s.record = k;
        s.layer = le->index;
        s.order = order_key(le->rank, sub::kTrack);
        const float hw = static_cast<float>(t.w * 0.5e-6);
        s.bbox[0] = static_cast<float>(std::min(t.x0, t.x1) * 1e-6) - hw;
        s.bbox[1] = static_cast<float>(std::min(t.y0, t.y1) * 1e-6) - hw;
        s.bbox[2] = static_cast<float>(std::max(t.x0, t.x1) * 1e-6) + hw;
        s.bbox[3] = static_cast<float>(std::max(t.y0, t.y1) * 1e-6) + hw;
        shapes_.push_back(s);
    }
    for (std::uint32_t k = 0; k < geo_->vias.size(); ++k) {
        const BoardVia &v = geo_->vias[k];
        Shape s;
        s.kind = hitkind::Via;
        s.record = k;
        s.layer = kNone32; // chosen per pick: the topmost visible layer of its span
        const float r = static_cast<float>(v.dia * 0.5e-6);
        s.bbox[0] = static_cast<float>(v.x * 1e-6) - r;
        s.bbox[1] = static_cast<float>(v.y * 1e-6) - r;
        s.bbox[2] = static_cast<float>(v.x * 1e-6) + r;
        s.bbox[3] = static_cast<float>(v.y * 1e-6) + r;
        shapes_.push_back(s);
    }
}

std::vector<Hit> Picker::pick(const PickInput &in) {
    std::vector<Hit> hits;
    if (in.trace == nullptr) return hits;
    const Trace &t = *in.trace;
    const double x = in.x, y = in.y, tol = std::max(0.0, in.tol);

    // --- the board ------------------------------------------------------------------
    index_board(in);
    if (geo_ && in.board_list != nullptr) {
        std::vector<const LayerEntry *> by_index;
        for (const LayerEntry &le : in.board_list->layers) {
            if (by_index.size() <= le.index) by_index.resize(le.index + 1, nullptr);
            by_index[le.index] = &le;
        }
        auto visible = [&](std::uint32_t l) { return l < in.layer_visible.size() && in.layer_visible[l]; };
        auto net_name = [&](std::int32_t code) -> std::string {
            auto it = board_nets_.find(code);
            return it != board_nets_.end() ? it->second : std::string();
        };
        auto layer_name = [&](std::uint32_t l) {
            return l < by_index.size() && by_index[l] != nullptr ? by_index[l]->name : std::string("?");
        };
        const BoardInfo &info = t.boards()[in.board];
        for (const Shape &s : shapes_) {
            if (!in_box(s.bbox, x, y, s.kind == hitkind::Zone ? 0.0 : tol)) continue;
            Hit h;
            h.kind = s.kind;
            h.record = s.record;
            h.seq = s.record;
            if (s.kind == hitkind::Pad || s.kind == hitkind::Zone) {
                const std::uint32_t bit = s.kind == hitkind::Pad ? kPadsBit : kZonesBit;
                if ((in.classes & bit) == 0 || !visible(s.layer)) continue;
                const Polyset &p = geo_->polysets[s.record];
                double best = std::numeric_limits<double>::infinity();
                for (std::uint32_t q = 0; q + 1 < p.poly_start.size(); ++q) best = std::min(best, polygon_dist(p, q, x, y));
                if (s.kind == hitkind::Zone ? best > 0 : best > tol) continue;
                h.dist = best;
                h.layer = s.layer;
                h.order = s.order;
                h.group = s.kind == hitkind::Zone ? 2 : 1;
                h.board_net = p.net;
                h.net_name = net_name(p.net);
                if (s.kind == hitkind::Pad) {
                    if (p.fp >= 0 && static_cast<std::size_t>(p.fp) < info.footprints.size())
                        h.footprint = info.footprints[static_cast<std::size_t>(p.fp)].ref;
                    // The pad's number: the nearest PADINFO of the same footprint (and net).
                    double nd = std::numeric_limits<double>::infinity();
                    for (const PadInfo &pi : geo_->padinfos) {
                        if (pi.fp != p.fp || (p.net >= 0 && pi.net != p.net)) continue;
                        const double dx = pi.x * 1e-6 - x, dy = pi.y * 1e-6 - y;
                        if (dx * dx + dy * dy < nd) {
                            nd = dx * dx + dy * dy;
                            h.pad = pi.number;
                        }
                    }
                    std::string who = h.footprint.empty() ? std::string("pad") : "pad " + h.footprint;
                    if (!h.pad.empty()) who += (h.footprint.empty() ? " " : ".") + h.pad;
                    h.text = who + ", " + (h.net_name.empty() ? std::string("no net") : "net " + h.net_name) + ", " +
                             layer_name(s.layer);
                } else {
                    h.text = "zone fill, " + (h.net_name.empty() ? std::string("no net") : "net " + h.net_name) + ", " +
                             layer_name(s.layer);
                }
            } else if (s.kind == hitkind::Track) {
                if ((in.classes & kTracksBit) == 0 || !visible(s.layer)) continue;
                const BoardTrack &tr = geo_->tracks[s.record];
                const double d = seg_dist(x, y, tr.x0 * 1e-6, tr.y0 * 1e-6, tr.x1 * 1e-6, tr.y1 * 1e-6) - tr.w * 0.5e-6;
                if (d > tol) continue;
                h.dist = std::max(0.0, d);
                h.layer = s.layer;
                h.order = s.order;
                h.board_net = tr.net;
                h.net_name = net_name(tr.net);
                h.text = fmt("track, %s, %s, %.3f mm", h.net_name.empty() ? "no net" : ("net " + h.net_name).c_str(),
                             layer_name(s.layer).c_str(), tr.w * 1e-6);
            } else { // via
                if ((in.classes & kViasBit) == 0) continue;
                const BoardVia &v = geo_->vias[s.record];
                const double d = std::hypot(x - v.x * 1e-6, y - v.y * 1e-6) - v.dia * 0.5e-6;
                if (d > tol) continue;
                // The topmost visible copper layer of its span.
                const LayerEntry *top = v.top >= 0 && static_cast<std::size_t>(v.top) < by_index.size()
                                            ? by_index[static_cast<std::size_t>(v.top)] : nullptr;
                const LayerEntry *bot = v.bottom >= 0 && static_cast<std::size_t>(v.bottom) < by_index.size()
                                            ? by_index[static_cast<std::size_t>(v.bottom)] : nullptr;
                if (top == nullptr || bot == nullptr) continue;
                const int lo = std::min(top->cls.number, bot->cls.number), hi = std::max(top->cls.number, bot->cls.number);
                const LayerEntry *best = nullptr;
                for (const LayerEntry *le : by_index)
                    if (le != nullptr && le->cls.kind == LayerKind::Copper && le->cls.number >= lo && le->cls.number <= hi &&
                        visible(le->index) && (best == nullptr || le->rank > best->rank))
                        best = le;
                if (best == nullptr) continue;
                h.dist = std::max(0.0, d);
                h.layer = best->index;
                h.order = order_key(best->rank, sub::kVia);
                h.board_net = v.net;
                h.net_name = net_name(v.net);
                h.text = fmt("via, %s, %.3f / %.3f mm", h.net_name.empty() ? "no net" : ("net " + h.net_name).c_str(),
                             v.dia * 1e-6, v.drill * 1e-6);
            }
            hits.push_back(std::move(h));
        }
    }

    // --- router content ----------------------------------------------------------------
    const Timeline *tl = in.timeline;
    const std::uint32_t e = tl != nullptr && in.event < tl->size() ? in.event : kNone32;
    if (e != kNone32 && in.geometry != nullptr) {
        const Event &ev = tl->events()[e];
        const std::uint32_t session = ev.session;
        const bool have_run = session < t.runs().size() && t.runs()[session].valid;
        if (in.routed_shown && have_run) {
            std::vector<std::uint32_t> alive;
            tl->alive_copper(e, alive);
            const std::vector<CopperGeom> &geo = *in.geometry;
            for (std::uint32_t c : alive) {
                if (c >= geo.size() || !geo[c].drawable) continue;
                const CopperGeom &g = geo[c];
                const double pad = tol + std::max(g.width, g.via_dia);
                if (x < g.bbox[0] - pad || x > g.bbox[2] + pad || y < g.bbox[1] - pad || y > g.bbox[3] + pad) continue;
                double best = std::numeric_limits<double>::infinity();
                int best_layer = -1;
                std::uint32_t best_sub = sub::kRouted;
                auto consider = [&](double d, int l, std::uint32_t sb) {
                    const RouterLayer &rl = in.rl[l & 1];
                    if (!rl.visible || d > tol) return;
                    // Prefer the upper layer when both are hit.
                    const bool better = best_layer < 0 || rl.rank > in.rl[best_layer & 1].rank ||
                                        (rl.rank == in.rl[best_layer & 1].rank && d < best);
                    if (better) {
                        best = d;
                        best_layer = l;
                        best_sub = sb;
                    }
                };
                for (const CopperGeom::Line &ln : g.lines) {
                    double d = std::numeric_limits<double>::infinity();
                    const double *p = &g.pts[2 * static_cast<std::size_t>(ln.first)];
                    if (ln.count == 1) d = std::hypot(x - p[0], y - p[1]);
                    for (std::uint32_t k = 0; k + 1 < ln.count; ++k)
                        d = std::min(d, seg_dist(x, y, p[2 * k], p[2 * k + 1], p[2 * k + 2], p[2 * k + 3]));
                    consider(std::max(0.0, d - g.width / 2), static_cast<int>(ln.layer), sub::kRouted);
                }
                for (const CopperGeom::Seg &s : g.stubs)
                    consider(std::max(0.0, seg_dist(x, y, s.x0, s.y0, s.x1, s.y1) - s.w / 2), static_cast<int>(s.layer),
                             sub::kRouted);
                for (const auto &v : g.vias) {
                    const double d = std::max(0.0, std::hypot(x - v[0], y - v[1]) - g.via_dia / 2);
                    for (int l = 0; l < in.router_layer_count; ++l) consider(d, l, sub::kRoutedVia);
                }
                if (best_layer < 0) continue;
                Hit h;
                h.kind = hitkind::Routed;
                h.dist = best;
                h.seq = c;
                h.router_layer = static_cast<std::uint32_t>(best_layer);
                h.layer = in.rl[best_layer & 1].index;
                h.order = order_key(in.rl[best_layer & 1].rank, best_sub);
                h.copper = c;
                h.event = tl->copper_life()[c].birth;
                h.net = t.copper()[c].net;
                h.has_net = true;
                h.net_name = router_net_name(t, h.net);
                h.text = fmt("router copper %u, net %s, %s, %s at event %u", t.copper()[c].id, h.net_name.c_str(),
                             in.rl[best_layer & 1].name.c_str(),
                             t.copper()[c].kind == Kind::Via ? "via placed" : "committed", h.event + 1);
                hits.push_back(std::move(h));
            }
        }
        // Failure markers (drawn whatever the visibility).
        if (have_run) {
            const double cell = t.runs()[session].cell;
            const double r = std::max(5 * cell, 0.4);
            std::vector<std::uint32_t> fl;
            tl->alive_failures(e, fl);
            for (std::uint32_t f : fl) {
                const Failure &fa = tl->failures()[f];
                if (!fa.placed) continue;
                const double d = std::max(0.0, std::hypot(x - fa.x, y - fa.y) - r);
                if (d > tol) continue;
                Hit h;
                h.kind = hitkind::Failure;
                h.group = 0;
                h.order = order_key(kRankOverlay, sub::kMarkFailure);
                h.dist = d;
                h.seq = f;
                h.record = f;
                h.event = fa.birth;
                h.net = fa.net;
                h.has_net = true;
                h.net_name = router_net_name(t, fa.net);
                h.text = fmt("failed pad %s of %s (%s), reported at event %u", fa.label.c_str(),
                             fa.name.empty() ? h.net_name.c_str() : fa.name.c_str(),
                             fa.from_drop ? "drop_to_plane" : "route_net", fa.birth + 1);
                hits.push_back(std::move(h));
            }
        }
        // The wavefront cell under the point.
        if (in.wave_shown && have_run && in.wave_source != kNone32 && in.wave_count > 0) {
            if (wave_source_ != in.wave_source) {
                wave_source_ = in.wave_source;
                wave_.reset();
                wave_first_.clear();
                wave_pairs_.clear();
                try {
                    wave_ = in.trace->wavefront(in.wave_source);
                } catch (...) {
                    wave_.reset(); // unreadable arrays: nothing to pick (the images report the error)
                }
                if (wave_ && !wave_->cells.empty()) {
                    const std::uint64_t ncells = t.runs()[session].ncells();
                    const auto n = static_cast<std::uint32_t>(wave_->cells.size());
                    if (dense_cell_array_ok(4 * ncells, n)) {
                        wave_first_.assign(static_cast<std::size_t>(ncells), kNone32);
                        for (std::uint32_t k = 0; k < n; ++k) {
                            const std::uint32_t cc = wave_->cells[k];
                            if (cc < wave_first_.size() && wave_first_[cc] == kNone32) wave_first_[cc] = k;
                        }
                    } else {
                        wave_pairs_.reserve(n);
                        for (std::uint32_t k = 0; k < n; ++k) wave_pairs_.emplace_back(wave_->cells[k], k);
                        std::sort(wave_pairs_.begin(), wave_pairs_.end());
                    }
                }
            }
            if (wave_ && (!wave_first_.empty() || !wave_pairs_.empty())) {
                const Run &run = t.runs()[session];
                const Frame fr(run);
                const double ci = (x - fr.cx(0)) / run.cell, cj = (y - fr.cy(0)) / run.cell;
                const long i = std::lround(ci), j = std::lround(cj);
                if (i >= 0 && j >= 0 && i < run.nx && j < run.ny) {
                    // Each router layer's cell, drawn in its image (one image
                    // for both when the layers share a name); the sort puts the
                    // upper one first.
                    for (int rlay = 0; rlay < 2; ++rlay) {
                        const int img = in.router_layer_count == 1 ? 0 : rlay;
                        if (!in.rl[img].visible) continue;
                        const std::uint64_t cell = static_cast<std::uint64_t>(rlay) * run.nc() +
                                                   static_cast<std::uint64_t>(j) * static_cast<std::uint64_t>(run.nx) +
                                                   static_cast<std::uint64_t>(i);
                        const std::uint32_t k = first_position(cell);
                        if (k == kNone32 || k >= in.wave_count) continue;
                        Hit h;
                        h.kind = hitkind::Wavefront;
                        h.group = 2;
                        h.order = order_key(in.rl[img].rank, sub::kWavefront);
                        h.seq = k;
                        h.layer = in.rl[img].index;
                        h.router_layer = static_cast<std::uint32_t>(rlay);
                        h.cell = static_cast<std::uint32_t>(cell);
                        h.order_index = k;
                        h.cost = wave_->has_costs && k < wave_->costs.size() ? wave_->costs[k] : -1;
                        h.event = e;
                        h.record = in.wave_search;
                        const Search &s = t.searches()[in.wave_search];
                        h.net = s.net;
                        h.has_net = true;
                        h.net_name = router_net_name(t, s.net);
                        h.text = fmt("wavefront of search %u (%s): cell (%ld, %ld) on %s, expanded %u of %zu",
                                     s.id_field, h.net_name.c_str(), i, j, run.layer_name[rlay].c_str(), k + 1,
                                     wave_->cells.size());
                        if (h.cost >= 0) h.text += fmt(", cost %lld", static_cast<long long>(h.cost));
                        hits.push_back(std::move(h));
                    }
                }
            }
        }
    }

    std::stable_sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
        if (a.group != b.group) return a.group < b.group;
        if (a.order != b.order) return a.order > b.order;
        if (a.seq != b.seq) return a.seq > b.seq;
        return a.dist < b.dist;
    });
    return hits;
}

} // namespace rv
