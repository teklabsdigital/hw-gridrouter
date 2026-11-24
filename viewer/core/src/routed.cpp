// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// routed.cpp: router geometry in board mm, and the no-board outline list.

#include "routed.hpp"

#include "listbuild.hpp"
#include "trace.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace rv {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

void grow(std::array<double, 4> &b, double x, double y, double r) {
    b[0] = std::min(b[0], x - r);
    b[1] = std::min(b[1], y - r);
    b[2] = std::max(b[2], x + r);
    b[3] = std::max(b[3], y + r);
}

CopperGeom empty_geom(const Run &run) {
    CopperGeom g;
    g.via_dia = run.via_dia;
    g.via_drill = run.via_drill;
    g.bbox = {{kInf, kInf, -kInf, -kInf}};
    return g;
}

void finish_bbox(CopperGeom &g) {
    for (const CopperGeom::Line &l : g.lines)
        for (std::uint32_t k = 0; k < l.count; ++k)
            grow(g.bbox, g.pts[2 * (l.first + k)], g.pts[2 * (l.first + k) + 1], g.width / 2);
    for (const CopperGeom::Seg &s : g.stubs) {
        grow(g.bbox, s.x0, s.y0, s.w / 2);
        grow(g.bbox, s.x1, s.y1, s.w / 2);
    }
    for (const auto &v : g.vias) grow(g.bbox, v[0], v[1], g.via_dia / 2);
    if (!(g.bbox[0] <= g.bbox[2])) g.bbox = {{0, 0, 0, 0}};
}

} // namespace

CopperGeom commit_geometry(const Run &run, const Commit &c) {
    CopperGeom g = empty_geom(run);
    const Frame fr(run);
    g.drawable = true;
    g.width = c.width_mm;
    const std::size_t n = c.path.size();
    std::vector<CellPos> cells(n);
    for (std::size_t k = 0; k < n; ++k) cells[k] = fr.cell(c.path[k]);
    std::vector<std::pair<std::int64_t, std::int64_t>> run_pts;
    std::size_t start = 0;
    for (std::size_t k = 1; k <= n; ++k) {
        if (k < n && cells[k].layer == cells[start].layer) continue;
        if (k - start >= 2) {
            run_pts.clear();
            for (std::size_t q = start; q < k; ++q) {
                const std::pair<std::int64_t, std::int64_t> p(cells[q].i, cells[q].j);
                if (!run_pts.empty() && run_pts.back() == p) continue;
                // Drop the previous point if it lies straight between its
                // neighbours (same direction on both sides).
                if (run_pts.size() >= 2) {
                    const auto &a = run_pts[run_pts.size() - 2];
                    const auto &b = run_pts.back();
                    const std::int64_t dx1 = b.first - a.first, dy1 = b.second - a.second;
                    const std::int64_t dx2 = p.first - b.first, dy2 = p.second - b.second;
                    if (dx1 * dy2 - dy1 * dx2 == 0 && dx1 * dx2 + dy1 * dy2 > 0) run_pts.pop_back();
                }
                run_pts.push_back(p);
            }
            CopperGeom::Line l;
            l.layer = cells[start].layer;
            l.first = static_cast<std::uint32_t>(g.pts.size() / 2);
            l.count = static_cast<std::uint32_t>(run_pts.size());
            for (const auto &p : run_pts) {
                g.pts.push_back(fr.cx(static_cast<std::int32_t>(p.first)));
                g.pts.push_back(fr.cy(static_cast<std::int32_t>(p.second)));
            }
            g.lines.push_back(l);
        }
        start = k;
    }
    for (const Stub &s : c.stubs) {
        CopperGeom::Seg sg;
        sg.layer = s.layer;
        sg.x0 = fr.x(s.x0);
        sg.y0 = fr.y(s.y0);
        sg.x1 = fr.x(s.x1);
        sg.y1 = fr.y(s.y1);
        sg.w = s.w;
        g.stubs.push_back(sg);
    }
    for (const GridPoint &v : c.vias)
        if (!v.off_grid) g.vias.push_back({{fr.cx(v.i), fr.cy(v.j)}});
    finish_bbox(g);
    return g;
}

CopperGeom via_geometry(const Run &run, const Via &v) {
    CopperGeom g = empty_geom(run);
    const Frame fr(run);
    g.drawable = true;
    if (!v.off_grid) g.vias.push_back({{fr.cx(v.i), fr.cy(v.j)}});
    finish_bbox(g);
    return g;
}

std::vector<CopperGeom> build_copper_geometry(const Trace &t) {
    std::vector<CopperGeom> out;
    append_copper_geometry(t, out);
    return out;
}

void append_copper_geometry(const Trace &t, std::vector<CopperGeom> &out) {
    const std::size_t from = out.size();
    out.resize(std::max(from, t.copper().size()));
    for (std::size_t k = from; k < out.size(); ++k) {
        const Copper &cu = t.copper()[k];
        if (!cu.valid || cu.session >= t.runs().size() || !t.runs()[cu.session].valid) continue;
        const Run &run = t.runs()[cu.session];
        if (cu.kind == Kind::Cmit && cu.record < t.commits().size())
            out[k] = commit_geometry(run, t.commits()[cu.record]);
        else if (cu.kind == Kind::Via && cu.record < t.vias().size())
            out[k] = via_geometry(run, t.vias()[cu.record]);
        out[k].session = cu.session;
    }
}

DisplayList build_outline_list(const Trace &t, std::uint32_t session, const Theme &theme) {
    DisplayList out;
    if (session >= t.runs().size() || !t.runs()[session].valid) return out;
    const Run &run = t.runs()[session];
    const Frame fr(run);

    // Layer table: the router layers, then Edge.Cuts for the grid extent.
    std::vector<std::string> names;
    for (const std::string &n : run.layer_name)
        if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    names.push_back("Edge.Cuts");
    std::vector<LayerEntry> layers(names.size());
    for (std::size_t k = 0; k < names.size(); ++k) {
        LayerEntry &e = layers[k];
        e.index = static_cast<std::uint32_t>(k);
        e.name = names[k];
        e.cls = classify_layer(names[k]);
        e.color = theme.layer_color(names[k]);
        e.rank = draw_rank(e.cls);
        e.panel = panel_rank(e.cls);
        e.type = e.cls.kind == LayerKind::Copper ? 0 : -1;
    }
    auto router_layer = [&](std::uint32_t k) -> std::uint32_t {
        return static_cast<std::uint32_t>(std::find(names.begin(), names.end(), run.layer_name[k & 1]) - names.begin());
    };
    const std::uint32_t edge = static_cast<std::uint32_t>(names.size() - 1);
    const double hair = 0.05;
    const std::uint32_t outline_bit = 1u << static_cast<std::uint32_t>(ObjClass::Outline);
    const std::uint32_t pads_bit = 1u << static_cast<std::uint32_t>(ObjClass::Pads);
    const std::uint32_t rules_bit = 1u << static_cast<std::uint32_t>(ObjClass::RuleAreas);

    ListBuilder lb;
    auto rect = [&](LBucket &b, double x0, double y0, double x1, double y1) {
        const double p[10] = {x0, y0, x1, y0, x1, y1, x0, y1, x0, y0};
        b.line(p, 5, hair / 2);
    };
    const double h = run.cell / 2;
    const double gx0 = fr.x(-h), gy0 = fr.y(-h), gx1 = fr.x((run.nx - 1) * run.cell + h),
                 gy1 = fr.y((run.ny - 1) * run.cell + h);
    rect(lb.at(order_key(layers[edge].rank, sub::kOutline), Prim::Stroke, hair, static_cast<std::uint32_t>(ObjClass::Outline),
               edge, layers[edge].color, outline_bit),
         gx0, gy0, gx1, gy1);
    ++layers[edge].items;

    // The session's chunks only (M6: an outline is rebuilt as the trace grows).
    const std::uint32_t end = std::min<std::uint32_t>(run.end_chunk, static_cast<std::uint32_t>(t.chunks().size()));
    for (std::uint32_t k = std::min(run.chunk, end); k < end; ++k) {
        const Chunk &c = t.chunks()[k];
        if (c.session != session || c.corrupt || c.record == kNone32) continue;
        if (c.kind == Kind::Pad) {
            const Pad &p = t.pads()[c.record];
            for (std::uint32_t k = 0; k < 2; ++k) {
                if ((p.layers & (1u << k)) == 0) continue;
                const std::uint32_t li = router_layer(k);
                rect(lb.at(order_key(layers[li].rank, sub::kPad), Prim::Stroke, hair,
                           static_cast<std::uint32_t>(ObjClass::Pads), li, layers[li].color, pads_bit),
                     fr.x(p.x0), fr.y(p.y0), fr.x(p.x1), fr.y(p.y1));
                ++layers[li].items;
            }
        } else if (c.kind == Kind::Rect) {
            const Rect &r = t.rects()[c.record];
            double x0, y0, x1, y1;
            if (r.op == 2) {
                x0 = fr.x(r.x0 * run.cell - h);
                y0 = fr.y(r.y0 * run.cell - h);
                x1 = fr.x(r.x1 * run.cell + h);
                y1 = fr.y(r.y1 * run.cell + h);
            } else {
                x0 = fr.x(r.x0);
                y0 = fr.y(r.y0);
                x1 = fr.x(r.x1);
                y1 = fr.y(r.y1);
            }
            for (std::uint32_t k = 0; k < 2; ++k) {
                if ((r.layers & (1u << k)) == 0) continue;
                const std::uint32_t li = router_layer(k);
                rect(lb.at(order_key(layers[li].rank, sub::kRuleArea), Prim::Stroke, hair,
                           static_cast<std::uint32_t>(ObjClass::RuleAreas), li,
                           with_opacity(layers[li].color, theme.zone_opacity), rules_bit),
                     x0, y0, x1, y1);
                ++layers[li].items;
            }
        } else if (c.kind == Kind::Disc) {
            const Disc &d = t.discs()[c.record];
            const std::uint32_t li = router_layer(static_cast<std::uint32_t>(d.layer));
            lb.at(order_key(layers[li].rank, sub::kRuleArea), Prim::Ring, hair, static_cast<std::uint32_t>(ObjClass::RuleAreas),
                  li, with_opacity(layers[li].color, theme.zone_opacity), rules_bit)
                .circle(fr.cx(d.i), fr.cy(d.j), std::max(d.r_t, 0) * run.cell, hair / 2);
            ++layers[li].items;
        }
    }

    std::uint32_t next_key = 0;
    const std::array<float, 4> all = lb.finish(out, [&](const LKey &, const LBucket &) { return next_key++; });
    out.has_extent = true;
    out.extent = {{static_cast<float>(gx0), static_cast<float>(gy0), static_cast<float>(gx1), static_cast<float>(gy1)}};
    out.content = all;
    out.layers = layers;
    std::stable_sort(out.layers.begin(), out.layers.end(),
                     [](const LayerEntry &a, const LayerEntry &b) { return a.panel < b.panel; });
    return out;
}

} // namespace rv
