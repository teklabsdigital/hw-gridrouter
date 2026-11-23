// overlay.cpp: the dynamic display list at a playback position (overlay.hpp).

#include "overlay.hpp"

#include "listbuild.hpp"
#include "timeline.hpp"
#include "trace.hpp"

#include <algorithm>
#include <cmath>

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

struct RouterLayer {
    std::string name;
    std::uint32_t index = kNone32; // board layer index
    std::uint32_t rank = 0;
    Rgba color = 0;
    bool visible = true;
};

} // namespace

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

    // Router layers and slots.
    RouterLayer rl[2];
    if (have_run) {
        const Run &run = t.runs()[session];
        for (int k = 0; k < 2; ++k) {
            RouterLayer &l = rl[k];
            l.name = run.layer_name[k];
            l.rank = draw_rank(classify_layer(l.name));
            if (in.board != nullptr)
                for (const LayerEntry &le : in.board->layers)
                    if (le.name == l.name) {
                        l.index = le.index;
                        l.rank = le.rank;
                    }
            l.color = theme.layer_color(l.name);
            for (const auto &v : in.layer_visible)
                if (v.first == l.name) l.visible = v.second;
        }
        for (int k = 0; k < 2; ++k) {
            if (k == 1 && rl[1].name == rl[0].name) break;
            Slot s;
            s.kind = SlotKind::Routed;
            s.order_begin = order_key(rl[k].rank, sub::kGhost);
            s.order_end = order_key(rl[k].rank, sub::kRoutedEnd);
            s.layer = rl[k].index;
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

        // mode 0 routed, 1 ghost (with colour level), 2 highlight
        auto add_copper = [&](std::uint32_t c, int mode, int level) {
            if (c >= geo.size() || !geo[c].drawable) return;
            const CopperGeom &g = geo[c];
            const std::uint64_t group = mode == 2 ? (kGroupHilite | e) : c / 64;
            const std::uint64_t bit = mode == 2 ? 1 : 1ull << (c % 64);
            auto color_of = [&](const RouterLayer &l, double opacity) -> Rgba {
                if (mode == 0) return with_opacity(l.color, opacity);
                if (mode == 1)
                    return with_alpha(mix_white(l.color, colors.ghost_white),
                                      colors.ghost_alpha * level / static_cast<double>(kGhostLevels));
                return with_alpha(mix_white(l.color, colors.hilite_white), 1.0);
            };
            auto order_of = [&](const RouterLayer &l, std::uint32_t routed_sub) {
                if (mode == 0) return order_key(l.rank, routed_sub);
                if (mode == 1) return order_key(l.rank, sub::kGhost);
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
            if (mode == 0 && any && g.via_drill > 0) {
                LBucket &b = lb.at(order_key(kRankHoles, sub::kRoutedDrill), Prim::Disc, 0, kClassRouted, kNone32,
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
            if (s.npath >= 1) {
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
            dots(s.tgt(), s.ntgt_kept, sub::kMarkCells + 2, colors.tgt);
        }
        if (ev.route != kNone32 && ev.route < t.routes().size()) {
            const Route &r = t.routes()[ev.route];
            const double rr = std::max(3 * cell, 0.25), w = std::max(0.6 * cell, 0.05);
            LBucket &b =
                lb.at(order_key(kRankOverlay, sub::kMarkPads), Prim::Ring, w, kClassRouted, kNone32, colors.pads, 0, hg);
            for (const RoutePad &p : r.pads) b.circle(fr.x(p.cx), fr.y(p.cy), rr, w / 2);
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
