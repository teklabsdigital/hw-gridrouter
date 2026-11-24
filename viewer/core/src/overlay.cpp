// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// overlay.cpp: the dynamic display list at a playback position (overlay.hpp).

#include "overlay.hpp"

#include "listbuild.hpp"
#include "timeline.hpp"
#include "trace.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rv {

Rgba mix_white(Rgba c, double f) {
    auto ch = [&](int shift) {
        const double v = static_cast<double>((c >> shift) & 0xFF);
        return static_cast<Rgba>(std::lround(v + (255.0 - v) * f)) << shift;
    };
    return ch(24) | ch(16) | ch(8) | (c & 0xFF);
}

namespace {

Rgba with_alpha(Rgba c, double a) {
    return (c & 0xFFFFFF00u) | static_cast<Rgba>(std::lround(std::max(0.0, std::min(1.0, a)) * 255.0));
}

constexpr std::uint64_t kGroupHilite = 1ull << 62;
constexpr std::uint64_t kGroupMarker = 2ull << 62;
constexpr std::uint64_t kGroupSelect = 3ull << 62; // board items of the selected net
constexpr std::uint64_t kGroupDim = kGroupMarker | (1ull << 40);

// Position of a sub-slot among the known ones of its rank (ascending).
std::uint32_t sub_index(std::uint32_t s, const std::uint32_t *subs, std::uint32_t n) {
    std::uint32_t k = 0;
    while (k + 1 < n && subs[k + 1] <= s) ++k;
    return k;
}

} // namespace

std::uint32_t highlight_order(std::uint32_t order) {
    static const std::uint32_t copper_subs[] = {0x08, 0x10, 0x14, 0x18, 0x20, 0x28, 0x30,
                                                0x34, 0x38, 0x3A, 0x3C, 0x40, 0x50, 0x60};
    static const std::uint32_t hole_subs[] = {0x10, 0x20, 0x28, 0x2C, 0x30};
    const std::uint32_t rank = order >> 8, s = order & 0xFF;
    std::uint32_t idx, sb;
    if (rank == kRankHoles) {
        idx = 71;
        sb = sub_index(s, hole_subs, 5);
    } else {
        idx = rank < 100 ? 0 : (rank > 170 ? 70 : rank - 100);
        sb = sub_index(s, copper_subs, 14);
    }
    return order_key(kRankHighlight, 0) + idx * 16 + sb;
}

int router_layers(const Trace &t, std::uint32_t session, const DisplayList *board, const Theme &theme,
                  const std::vector<std::pair<std::string, bool>> &layer_visible, RouterLayer rl[2]) {
    rl[0] = RouterLayer();
    rl[1] = RouterLayer();
    if (session >= t.runs().size() || !t.runs()[session].valid) return 0;
    const Run &run = t.runs()[session];
    for (int k = 0; k < 2; ++k) {
        RouterLayer &l = rl[k];
        l.name = run.layer_name[k];
        l.rank = draw_rank(classify_layer(l.name));
        if (board != nullptr)
            for (const LayerEntry &le : board->layers)
                if (le.name == l.name) {
                    l.index = le.index;
                    l.rank = le.rank;
                }
        l.color = theme.layer_color(l.name);
        for (const auto &v : layer_visible)
            if (v.first == l.name) l.visible = v.second;
    }
    return rl[1].name == rl[0].name ? 1 : 2;
}

void Overlay::reset() {
    list_ = DisplayList();
    slots_.clear();
    keys_.clear();
    next_key_ = kKeyBase;
    slot_sigs_.clear();
    ++key_epoch_;
    ++generation_;
}

std::uint32_t Overlay::key_for(const std::array<std::uint64_t, 4> &ident) {
    auto it = keys_.find(ident);
    if (it != keys_.end()) return it->second;
    const std::uint32_t k = next_key_++;
    if (next_key_ == 0) next_key_ = kKeyBase; // wrapped after 2^31 distinct batches; never in practice
    keys_.emplace(ident, k);
    return k;
}

void Overlay::build(const OverlayInput &in) {
    std::vector<std::array<std::uint32_t, 4>> old_shape;
    for (const Slot &s : slots_) old_shape.push_back({{s.order_begin, s.generation, s.batch_count, s.layer}});
    list_ = DisplayList();
    slots_.clear();
    if (in.trace == nullptr || in.timeline == nullptr || in.geometry == nullptr || in.theme == nullptr) {
        if (!old_shape.empty()) ++generation_;
        return;
    }
    const Trace &t = *in.trace;
    const Timeline &tl = *in.timeline;
    const Theme &theme = *in.theme;
    const std::uint32_t e = in.event < tl.size() ? in.event : kNone32;
    std::uint32_t session = e != kNone32 ? tl.events()[e].session : kNone32;
    if (e == kNone32 && !t.runs().empty()) session = static_cast<std::uint32_t>(t.runs().size() - 1);
    const bool have_run = session < t.runs().size() && t.runs()[session].valid;
    // The slots of an event before the first RUN (a board written before
    // the router started): those of the first session, empty.  M4 had no
    // router slots there, so the first frame of the session changed the
    // slot structure and every plane of a client was drawn again.
    std::uint32_t layout_session = session;
    if (e != kNone32 && session == kNone32 && !t.runs().empty()) layout_session = 0;

    // Router layers and slots: each router layer's wavefront image (under
    // the board's copper) and its ghosts, copper and vias (above the board's
    // tracks).
    RouterLayer rl[2];
    if (layout_session < t.runs().size() && t.runs()[layout_session].valid) {
        const int nl = router_layers(t, layout_session, in.board, theme, in.layer_visible, rl);
        for (int k = 0; k < nl; ++k) {
            Slot s;
            s.kind = SlotKind::Wavefront;
            s.order_begin = order_key(rl[k].rank, sub::kWavefront);
            s.order_end = s.order_begin + 1;
            s.layer = rl[k].index;
            slots_.push_back(s);
            s.kind = SlotKind::Routed;
            s.order_begin = order_key(rl[k].rank, sub::kGhost);
            s.order_end = order_key(rl[k].rank, sub::kRoutedEnd);
            slots_.push_back(s);
        }
    }
    {
        Slot s;
        s.kind = SlotKind::Drills;
        s.order_begin = order_key(kRankHoles, sub::kRoutedDrill);
        s.order_end = s.order_begin + 1;
        slots_.push_back(s);
        s.kind = SlotKind::Markers;
        s.order_begin = order_key(kRankOverlay, 0);
        s.order_end = order_key(kRankOverlay + 1, 0);
        slots_.push_back(s);
        s.kind = SlotKind::Dim;
        s.order_begin = order_key(kRankDim, 0);
        s.order_end = order_key(kRankDim + 1, 0);
        slots_.push_back(s);
        s.kind = SlotKind::Highlight;
        s.order_begin = order_key(kRankHighlight, 0);
        s.order_end = order_key(kRankHighlightEnd, 0);
        slots_.push_back(s);
    }
    std::sort(slots_.begin(), slots_.end(), [](const Slot &a, const Slot &b) { return a.order_begin < b.order_begin; });

    ListBuilder lb;
    const std::uint32_t needs = 1u << kClassRouted;
    if (e != kNone32 && have_run) {
        const Run &run = t.runs()[session];
        const Frame fr(run);
        const double cell = run.cell;
        const Event &ev = tl.events()[e];
        const std::vector<CopperGeom> &geo = *in.geometry;
        const int nlayers = rl[1].name == rl[0].name ? 1 : 2;

        // mode 0 routed, 1 ghost (with colour level), 2 highlight of the
        // current commit; 3 and 4: routed and ghost of the selected net, in
        // the selection slot
        auto add_copper = [&](std::uint32_t c, int mode, int level) {
            if (c >= geo.size() || !geo[c].drawable) return;
            const CopperGeom &g = geo[c];
            const std::uint64_t group = mode == 2 ? (kGroupHilite | e) : c / 64;
            const std::uint64_t bit = mode == 2 ? 1 : 1ull << (c % 64);
            auto color_of = [&](const RouterLayer &l, double opacity) -> Rgba {
                if (mode == 0) return with_opacity(l.color, opacity);
                if (mode == 3) return mix_white(with_opacity(l.color, opacity), colors.select_white);
                if (mode == 1 || mode == 4) {
                    const Rgba gc = mix_white(l.color, colors.ghost_white);
                    return with_alpha(mode == 4 ? mix_white(gc, colors.select_white) : gc,
                                      colors.ghost_alpha * level / static_cast<double>(kGhostLevels));
                }
                return with_alpha(mix_white(l.color, colors.hilite_white), 1.0);
            };
            auto order_of = [&](const RouterLayer &l, std::uint32_t routed_sub) {
                if (mode == 0) return order_key(l.rank, routed_sub);
                if (mode == 1) return order_key(l.rank, sub::kGhost);
                if (mode == 3) return highlight_order(order_key(l.rank, routed_sub));
                if (mode == 4) return highlight_order(order_key(l.rank, sub::kGhost));
                return order_key(kRankOverlay, sub::kMarkCopper);
            };
            auto visible = [&](const RouterLayer &l) { return mode == 2 || l.visible; };
            for (const CopperGeom::Line &ln : g.lines) {
                const RouterLayer &l = rl[ln.layer & 1];
                if (!visible(l)) continue;
                LBucket &b = lb.at(order_of(l, sub::kRouted), Prim::Stroke, g.width, kClassRouted,
                                   mode == 2 ? kNone32 : l.index, color_of(l, theme.track_opacity), mode == 2 ? 0 : needs,
                                   group);
                b.line(&g.pts[2 * static_cast<std::size_t>(ln.first)], ln.count, g.width / 2);
                b.mask |= bit;
            }
            for (const CopperGeom::Seg &s : g.stubs) {
                const RouterLayer &l = rl[s.layer & 1];
                if (!visible(l)) continue;
                LBucket &b = lb.at(order_of(l, sub::kRouted), Prim::Segs, s.w, kClassRouted,
                                   mode == 2 ? kNone32 : l.index, color_of(l, theme.track_opacity), mode == 2 ? 0 : needs,
                                   group);
                b.seg(s.x0, s.y0, s.x1, s.y1, s.w / 2);
                b.mask |= bit;
            }
            if (g.vias.empty()) return;
            bool any = false;
            for (int k = 0; k < nlayers; ++k) {
                const RouterLayer &l = rl[k];
                if (!visible(l)) continue;
                any = true;
                if (mode == 2 && k > 0) break; // one highlight disc is enough
                LBucket &b = lb.at(order_of(l, sub::kRoutedVia), Prim::Disc, 0, kClassRouted,
                                   mode == 2 ? kNone32 : l.index, color_of(l, theme.via_opacity), mode == 2 ? 0 : needs,
                                   group);
                for (const auto &v : g.vias) b.circle(v[0], v[1], g.via_dia / 2, 0);
                b.mask |= bit;
            }
            if ((mode == 0 || mode == 3) && any && g.via_drill > 0) {
                const std::uint32_t o = order_key(kRankHoles, sub::kRoutedDrill);
                LBucket &b = lb.at(mode == 3 ? highlight_order(o) : o, Prim::Disc, 0, kClassRouted, kNone32,
                                   theme.via_hole, needs, group);
                for (const auto &v : g.vias) b.circle(v[0], v[1], g.via_drill / 2, 0);
                b.mask |= bit;
            }
        };

        if (in.routed_visible && !in.suppress_routed) {
            std::vector<std::uint32_t> alive;
            tl.alive_copper(e, alive);
            for (std::uint32_t c : alive) add_copper(c, 0, 0);
            std::vector<std::pair<std::uint32_t, double>> gh;
            tl.ghosts(in.time, gh);
            for (const auto &g : gh) add_copper(g.first, 1, ghost_level(g.second));
            // The selected net's copper again, above the dim slot.
            if (in.select_active && in.select_has_net) {
                for (std::uint32_t c : alive)
                    if (t.copper()[c].net == in.select_net) add_copper(c, 3, 0);
                for (const auto &g : gh)
                    if (t.copper()[g.first].net == in.select_net) add_copper(g.first, 4, ghost_level(g.second));
            }
        }

        // Failed pads: a red ring with a cross.
        {
            std::vector<std::uint32_t> fl;
            tl.alive_failures(e, fl);
            const double r = std::max(5 * cell, 0.4), w = std::max(cell, 0.08), d = r * 0.7;
            for (std::uint32_t f : fl) {
                const Failure &fa = tl.failures()[f];
                if (!fa.placed) continue;
                const std::uint64_t group = kGroupMarker | (f / 64);
                LBucket &ring = lb.at(order_key(kRankOverlay, sub::kMarkFailure), Prim::Ring, w, kClassRouted, kNone32,
                                      colors.failure, 0, group);
                ring.circle(fa.x, fa.y, r, w / 2);
                ring.mask |= 1ull << (f % 64);
                LBucket &x = lb.at(order_key(kRankOverlay, sub::kMarkFailure), Prim::Segs, w, kClassRouted, kNone32,
                                   colors.failure, 0, group);
                x.seg(fa.x - d, fa.y - d, fa.x + d, fa.y + d, w / 2);
                x.seg(fa.x - d, fa.y + d, fa.x + d, fa.y - d, w / 2);
                x.mask |= 1ull << (f % 64);
            }
        }

        // The current event.
        const std::uint64_t hg = kGroupHilite | e;
        if (ev.kind == EvKind::Commit) {
            add_copper(t.commits()[ev.record].copper, 2, 0);
        } else if (ev.kind == EvKind::Via) {
            const Via &v = t.vias()[ev.record];
            if (!v.off_grid) {
                const double w = std::max(0.5 * cell, 0.04);
                lb.at(order_key(kRankOverlay, sub::kMarkVia), Prim::Ring, w, kClassRouted, kNone32, colors.via, 0, hg)
                    .circle(fr.cx(v.i), fr.cy(v.j), run.via_dia / 2 + std::max(cell, 0.05), w / 2);
            }
        } else if (ev.kind == EvKind::Search) {
            const Search &s = t.searches()[ev.record];
            const double r = std::max(0.45 * cell, 0.04);
            auto dots = [&](const std::uint32_t *cells, std::uint32_t n, std::uint32_t sb, Rgba color) {
                if (n == 0) return;
                LBucket &b = lb.at(order_key(kRankOverlay, sb), Prim::Disc, 0, kClassRouted, kNone32, color, 0, hg);
                for (std::uint32_t k = 0; k < n; ++k) {
                    const CellPos p = fr.cell(cells[k]);
                    b.circle(fr.cx(p.i), fr.cy(p.j), r, 0);
                }
            };
            // While the wavefront is revealed: sources and targets.  Once
            // settled: the path found, or the targets in red if none was.
            const bool found = s.has(srch_flags::kFound);
            if (in.settled && s.npath >= 1) {
                std::vector<double> pts;
                for (std::uint32_t k = 0; k < s.npath; ++k) {
                    const CellPos p = fr.cell(s.path()[k]);
                    pts.push_back(fr.cx(p.i));
                    pts.push_back(fr.cy(p.j));
                }
                const double w = std::max(0.3 * cell, 0.03);
                lb.at(order_key(kRankOverlay, sub::kMarkCells), Prim::Stroke, w, kClassRouted, kNone32, colors.path, 0, hg)
                    .line(pts.data(), pts.size() / 2, w / 2);
            }
            dots(s.src(), s.nsrc_kept, sub::kMarkCells + 1, colors.src);
            dots(s.tgt(), s.ntgt_kept, sub::kMarkCells + 2, in.settled && !found ? colors.failure : colors.tgt);
        }
        if (ev.route != kNone32 && ev.route < t.routes().size()) {
            const Route &r = t.routes()[ev.route];
            const double rr = std::max(3 * cell, 0.25), w = std::max(0.6 * cell, 0.05);
            LBucket &b =
                lb.at(order_key(kRankOverlay, sub::kMarkPads), Prim::Ring, w, kClassRouted, kNone32, colors.pads, 0, hg);
            for (const RoutePad &p : r.pads) b.circle(fr.x(p.cx), fr.y(p.cy), rr, w / 2);
        }
    }

    // The selection: the dim rectangle and the board's copper of the net.
    if (in.select_active) {
        const std::array<float, 4> &r = in.dim_rect;
        if (r[0] < r[2] && r[1] < r[3]) {
            // M6: the key names the rectangle too (it changes with the board
            // drawn and the session's grid while its colour stays), so a
            // client never keeps the plane or the path of another one.
            std::uint64_t h = 1469598103934665603ull; // FNV-1a over the four floats
            for (float v : r) {
                std::uint32_t bits;
                std::memcpy(&bits, &v, 4);
                for (int k = 0; k < 4; ++k) {
                    h ^= (bits >> (8 * k)) & 0xFFu;
                    h *= 1099511628211ull;
                }
            }
            lb.at(order_key(kRankDim, 0), Prim::Fill, 0, kClassRouted, kNone32,
                  with_alpha(theme.background, colors.dim_alpha), 0, kGroupDim | (h & ((1ull << 40) - 1)))
                .rect(r[0], r[1], r[2], r[3]);
        }
        if (in.select_board != nullptr) {
            const DisplayList &sl = *in.select_board;
            for (std::size_t k = 0; k < sl.batches.size(); ++k) {
                const Batch &b = sl.batches[k];
                if ((b.needs & in.classes) != b.needs) continue;
                if (b.layer != kNone32 && (b.layer >= in.board_layer_visible.size() || !in.board_layer_visible[b.layer]))
                    continue;
                const bool hole = (b.order >> 8) == kRankHoles;
                const std::uint64_t group =
                    kGroupSelect | (static_cast<std::uint64_t>(in.select_build & 0xFFFFFFFu) << 24) | (k & 0xFFFFFFu);
                LBucket &bk = lb.at(highlight_order(b.order), b.prim, b.width, b.cls, b.layer,
                                    hole ? b.color : mix_white(b.color, colors.select_white), b.needs, group);
                for (std::uint32_t q = 0; q < b.count; ++q) bk.copy_item(sl, b, q);
            }
        }
    }

    // A key names (slot order, primitive, width, group, members): copper by
    // its block and the members present, markers likewise, highlights by
    // their event and colour (the commit highlight has one bucket per layer
    // colour at the same order).
    lb.finish(list_, [&](const LKey &k, const LBucket &b) {
        const bool hilite = (k.group & (3ull << 62)) == kGroupHilite;
        return key_for({{(static_cast<std::uint64_t>(k.order) << 8) | k.prim, static_cast<std::uint64_t>(k.width_nm),
                         k.group, hilite ? k.color : b.mask}});
    });

    // Slots: their batches, bounds and generations.
    std::size_t bi = 0;
    for (Slot &s : slots_) {
        while (bi < list_.batches.size() && list_.batches[bi].order < s.order_begin) ++bi;
        s.first_batch = static_cast<std::uint32_t>(bi);
        std::vector<std::pair<std::uint32_t, Rgba>> sig;
        std::array<float, 4> bb{{0, 0, 0, 0}};
        bool any = false;
        while (bi < list_.batches.size() && list_.batches[bi].order < s.order_end) {
            const Batch &b = list_.batches[bi];
            sig.emplace_back(b.key, b.color);
            if (!any) bb = b.bbox;
            else {
                bb[0] = std::min(bb[0], b.bbox[0]);
                bb[1] = std::min(bb[1], b.bbox[1]);
                bb[2] = std::max(bb[2], b.bbox[2]);
                bb[3] = std::max(bb[3], b.bbox[3]);
            }
            any = true;
            ++bi;
        }
        s.batch_count = static_cast<std::uint32_t>(bi - s.first_batch);
        s.bbox = bb;
        auto it = slot_sigs_.find(s.order_begin);
        if (it == slot_sigs_.end() || it->second.first != sig) {
            ++slot_gen_;
            slot_sigs_[s.order_begin] = std::make_pair(sig, slot_gen_);
            s.generation = slot_gen_;
        } else {
            s.generation = it->second.second;
        }
    }
    std::vector<std::array<std::uint32_t, 4>> shape;
    for (const Slot &s : slots_) shape.push_back({{s.order_begin, s.generation, s.batch_count, s.layer}});
    if (shape != old_shape) ++generation_;
}

} // namespace rv
