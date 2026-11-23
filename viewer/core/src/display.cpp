// display.cpp: builds the display list of a board (see display.hpp, rv.h).
//
// Records are sorted into buckets keyed by (order, primitive, width, class,
// layer, colour); each bucket becomes one batch.  Buckets are emitted in key
// order, so the list is sorted bottom to top and deterministic.

#include "display.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace rv {

const char *class_name(std::uint32_t cls) noexcept {
    static const char *const names[kClassCount] = {
        "Zones", "Tracks", "Pads", "Vias", "Holes", "Text", "Footprint graphics", "Board graphics",
        "Rule areas", "Board outline", "Routed copper",
    };
    return cls < kClassCount ? names[cls] : "";
}

double contour_area2(const float *xy, std::uint32_t npts) {
    double a = 0;
    for (std::uint32_t k = 0; k < npts; ++k) {
        const std::uint32_t n = (k + 1 == npts) ? 0 : k + 1;
        a += static_cast<double>(xy[2 * k]) * xy[2 * n + 1] - static_cast<double>(xy[2 * n]) * xy[2 * k + 1];
    }
    return a;
}

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr std::int64_t kHairlineNm = 50000; // 0.05 mm: outlines written without a width
constexpr double kHatchPitchMm = 0.5;       // rule-area hatching

float mm(double nm) { return static_cast<float>(nm * 1e-6); }

std::uint32_t bit(ObjClass c) { return 1u << static_cast<std::uint32_t>(c); }

struct Key {
    std::uint32_t order, prim;
    std::int64_t width_nm;
    std::uint32_t cls, layer;
    Rgba color;
    std::uint32_t needs;
    bool operator<(const Key &o) const {
        return std::tie(order, prim, width_nm, cls, layer, color, needs) <
               std::tie(o.order, o.prim, o.width_nm, o.cls, o.layer, o.color, o.needs);
    }
};

struct Bucket {
    std::vector<float> xy;
    std::vector<std::uint32_t> contours, polys;
    std::vector<float> segs, circles;
    std::uint32_t items = 0;
    float bbox[4] = {kInf, kInf, -kInf, -kInf};

    void grow(float x, float y, float r) {
        bbox[0] = std::min(bbox[0], x - r);
        bbox[1] = std::min(bbox[1], y - r);
        bbox[2] = std::max(bbox[2], x + r);
        bbox[3] = std::max(bbox[3], y + r);
    }
};

class Builder {
public:
    Builder(const BoardInfo &info, const BoardGeometry &geo, const Theme &theme)
        : info_(info), geo_(geo), theme_(theme) {}

    DisplayList run();

private:
    Bucket &bucket(std::uint32_t order, Prim prim, std::int64_t width_nm, ObjClass cls, std::uint32_t layer,
                   Rgba color, std::uint32_t needs) {
        return buckets_[Key{order, static_cast<std::uint32_t>(prim), width_nm, static_cast<std::uint32_t>(cls),
                            layer, color, needs | bit(cls)}];
    }

    void polyset(const Polyset &p);
    void fill_polygon(Bucket &b, const Polyset &p, std::uint32_t poly);
    void stroke_contour(Bucket &b, const Polyset &p, std::uint32_t contour, bool close, float half_width);
    void hatch_polygon(Bucket &b, const Polyset &p, std::uint32_t poly, float half_width);
    void hole(const Hole &h);
    void via(const BoardVia &v);
    void track(const BoardTrack &t);
    void seg(Bucket &b, float x0, float y0, float x1, float y1, float half_width);
    void circle(Bucket &b, float x, float y, float r, float pad);
    void layer_table();

    // Points of contour c of p, as floats, without a repeated closing point
    // when drop_closing is set.
    void read_contour(const Polyset &p, std::uint32_t c, bool drop_closing);

    const BoardInfo &info_;
    const BoardGeometry &geo_;
    const Theme &theme_;
    std::map<Key, Bucket> buckets_;
    std::vector<LayerEntry> by_index_; // indexed by BRD layer index
    std::vector<float> tmp_;
    float outline_[4] = {kInf, kInf, -kInf, -kInf};
};

void Builder::read_contour(const Polyset &p, std::uint32_t c, bool drop_closing) {
    tmp_.clear();
    const std::uint32_t a = p.contour_start[c], e = p.contour_start[c + 1];
    for (std::uint32_t k = a; k < e; ++k) {
        tmp_.push_back(mm(p.xy[2 * static_cast<std::size_t>(k)]));
        tmp_.push_back(mm(p.xy[2 * static_cast<std::size_t>(k) + 1]));
    }
    if (drop_closing && tmp_.size() >= 6 && tmp_[0] == tmp_[tmp_.size() - 2] && tmp_[1] == tmp_.back()) {
        tmp_.pop_back();
        tmp_.pop_back();
    }
}

void Builder::fill_polygon(Bucket &b, const Polyset &p, std::uint32_t poly) {
    const std::uint32_t c0 = p.poly_start[poly], c1 = p.poly_start[poly + 1];
    const std::uint32_t first_contour = static_cast<std::uint32_t>(b.contours.size() / 2);
    std::uint32_t kept = 0;
    for (std::uint32_t c = c0; c < c1; ++c) {
        read_contour(p, c, true);
        const std::uint32_t n = static_cast<std::uint32_t>(tmp_.size() / 2);
        if (n < 3) {
            if (c == c0) return; // no outline, no polygon
            continue;
        }
        // Orient: the outline positive, every hole negative, so that a
        // whole batch can be filled with one non-zero fill (rv.h).
        const double a = contour_area2(tmp_.data(), n);
        const bool want_positive = (c == c0);
        if ((a < 0 && want_positive) || (a > 0 && !want_positive)) {
            for (std::uint32_t i = 0, j = n - 1; i < j; ++i, --j) {
                std::swap(tmp_[2 * i], tmp_[2 * j]);
                std::swap(tmp_[2 * i + 1], tmp_[2 * j + 1]);
            }
        }
        b.contours.push_back(static_cast<std::uint32_t>(b.xy.size() / 2));
        b.contours.push_back(n);
        for (std::uint32_t k = 0; k < n; ++k) {
            b.xy.push_back(tmp_[2 * k]);
            b.xy.push_back(tmp_[2 * k + 1]);
            if (c == c0) b.grow(tmp_[2 * k], tmp_[2 * k + 1], 0);
        }
        ++kept;
    }
    b.polys.push_back(first_contour);
    b.polys.push_back(kept);
    ++b.items;
}

void Builder::stroke_contour(Bucket &b, const Polyset &p, std::uint32_t contour, bool close, float half_width) {
    read_contour(p, contour, false);
    if (tmp_.empty()) return;
    if (tmp_.size() == 2) { // a single point: a dot
        tmp_.push_back(tmp_[0]);
        tmp_.push_back(tmp_[1]);
    }
    if (close && tmp_.size() >= 6 && !(tmp_[0] == tmp_[tmp_.size() - 2] && tmp_[1] == tmp_.back())) {
        tmp_.push_back(tmp_[0]);
        tmp_.push_back(tmp_[1]);
    }
    const std::uint32_t n = static_cast<std::uint32_t>(tmp_.size() / 2);
    b.contours.push_back(static_cast<std::uint32_t>(b.xy.size() / 2));
    b.contours.push_back(n);
    for (std::uint32_t k = 0; k < n; ++k) {
        b.xy.push_back(tmp_[2 * k]);
        b.xy.push_back(tmp_[2 * k + 1]);
        b.grow(tmp_[2 * k], tmp_[2 * k + 1], half_width);
    }
    ++b.items;
}

// Diagonal hatching of one polygon (outline and holes, even-odd), as KiCad
// marks rule areas.  Lines x - y = c, spaced kHatchPitchMm apart.
void Builder::hatch_polygon(Bucket &b, const Polyset &p, std::uint32_t poly, float half_width) {
    const std::uint32_t c0 = p.poly_start[poly], c1 = p.poly_start[poly + 1];
    std::vector<float> edges; // x0, y0, x1, y1 per edge
    double umin = 1e300, umax = -1e300;
    for (std::uint32_t c = c0; c < c1; ++c) {
        read_contour(p, c, true);
        const std::size_t n = tmp_.size() / 2;
        if (n < 3) continue;
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t m = (k + 1) % n;
            edges.insert(edges.end(), {tmp_[2 * k], tmp_[2 * k + 1], tmp_[2 * m], tmp_[2 * m + 1]});
            const double u = static_cast<double>(tmp_[2 * k]) - tmp_[2 * k + 1];
            umin = std::min(umin, u);
            umax = std::max(umax, u);
        }
    }
    if (edges.empty()) return;
    const double step = kHatchPitchMm * std::sqrt(2.0);
    std::vector<std::pair<double, std::pair<float, float>>> hits;
    for (double u = std::ceil(umin / step) * step; u <= umax; u += step) {
        hits.clear();
        for (std::size_t e = 0; e < edges.size(); e += 4) {
            const double ua = static_cast<double>(edges[e]) - edges[e + 1];
            const double ub = static_cast<double>(edges[e + 2]) - edges[e + 3];
            if ((ua < u) == (ub < u)) continue;
            const double t = (u - ua) / (ub - ua);
            const double x = edges[e] + t * (edges[e + 2] - edges[e]);
            const double y = edges[e + 1] + t * (edges[e + 3] - edges[e + 1]);
            hits.push_back({x + y, {static_cast<float>(x), static_cast<float>(y)}});
        }
        std::sort(hits.begin(), hits.end());
        for (std::size_t k = 0; k + 1 < hits.size(); k += 2)
            seg(b, hits[k].second.first, hits[k].second.second, hits[k + 1].second.first,
                hits[k + 1].second.second, half_width);
    }
}

void Builder::seg(Bucket &b, float x0, float y0, float x1, float y1, float half_width) {
    b.segs.insert(b.segs.end(), {x0, y0, x1, y1});
    b.grow(x0, y0, half_width);
    b.grow(x1, y1, half_width);
    ++b.items;
}

void Builder::circle(Bucket &b, float x, float y, float r, float pad) {
    b.circles.insert(b.circles.end(), {x, y, r});
    b.grow(x, y, r + pad);
    ++b.items;
}

void Builder::polyset(const Polyset &p) {
    const LayerEntry &le = by_index_.at(static_cast<std::size_t>(p.layer_index));
    const bool copper = le.cls.kind == LayerKind::Copper;
    const std::uint32_t npoly = p.poly_start.empty() ? 0 : static_cast<std::uint32_t>(p.poly_start.size() - 1);
    ObjClass cls = p.fp >= 0 ? ObjClass::FpGraphics : ObjClass::Graphics;
    std::uint32_t s = sub::kGraphic;
    Rgba color = le.color;
    bool outline_only = false; // drawn as closed outlines whatever sub says
    switch (p.role) {
    case 0:
        cls = ObjClass::Pads;
        s = sub::kPad;
        if (copper) color = with_opacity(color, theme_.pad_opacity);
        break;
    case 1: break;
    case 2:
        cls = ObjClass::Text;
        s = sub::kText;
        break;
    case 3:
        cls = ObjClass::Zones;
        s = sub::kZone;
        color = with_opacity(color, theme_.zone_opacity);
        break;
    case 4:
        cls = ObjClass::Zones;
        s = sub::kZoneOutline;
        outline_only = true;
        break;
    case 5:
        cls = ObjClass::Outline;
        s = sub::kOutline;
        outline_only = true;
        break;
    case 6:
        cls = ObjClass::RuleAreas;
        s = sub::kRuleArea;
        color = with_opacity(color, theme_.zone_opacity);
        outline_only = true;
        break;
    default: break; // unknown roles draw as graphics
    }
    const std::uint32_t order = order_key(le.rank, s);
    const auto layer = static_cast<std::uint32_t>(p.layer_index);

    if (outline_only) {
        const std::int64_t w = p.sub > 0 ? static_cast<std::int64_t>(p.sub) : kHairlineNm;
        const float hw = mm(static_cast<double>(w)) / 2;
        Bucket &b = bucket(order, Prim::Stroke, w, cls, layer, color, 0);
        for (std::uint32_t q = 0; q < npoly; ++q)
            for (std::uint32_t c = p.poly_start[q]; c < p.poly_start[q + 1]; ++c) {
                stroke_contour(b, p, c, p.sub == 0, hw);
                if (p.role == 5)
                    for (std::uint32_t k = p.contour_start[c]; k < p.contour_start[c + 1]; ++k) {
                        const float x = mm(p.xy[2 * static_cast<std::size_t>(k)]);
                        const float y = mm(p.xy[2 * static_cast<std::size_t>(k) + 1]);
                        outline_[0] = std::min(outline_[0], x);
                        outline_[1] = std::min(outline_[1], y);
                        outline_[2] = std::max(outline_[2], x);
                        outline_[3] = std::max(outline_[3], y);
                    }
            }
        if (p.role == 6) {
            Bucket &h = bucket(order, Prim::Segs, kHairlineNm, cls, layer, color, 0);
            for (std::uint32_t q = 0; q < npoly; ++q) hatch_polygon(h, p, q, mm(kHairlineNm) / 2);
        }
        return;
    }
    if (p.sub == 0) {
        Bucket &b = bucket(order, Prim::Fill, 0, cls, layer, color, 0);
        for (std::uint32_t q = 0; q < npoly; ++q) fill_polygon(b, p, q);
    } else {
        const float hw = mm(p.sub) / 2;
        Bucket &b = bucket(order, Prim::Stroke, p.sub, cls, layer, color, 0);
        for (std::uint32_t q = 0; q < npoly; ++q)
            for (std::uint32_t c = p.poly_start[q]; c < p.poly_start[q + 1]; ++c) stroke_contour(b, p, c, false, hw);
    }
}

void Builder::hole(const Hole &h) {
    const bool round = h.shape == 0 || h.w == h.h;
    const std::uint32_t needs = h.fp >= 0 ? bit(ObjClass::Pads) : 0;
    Rgba color;
    std::uint32_t s;
    if (!h.plated) {
        color = theme_.non_plated_hole;
        s = sub::kNonPlated;
    } else if (h.fp >= 0) {
        color = theme_.plated_hole;
        s = sub::kPadHole;
    } else {
        color = theme_.via_hole; // a free plated hole: a via drill (7.18)
        s = sub::kViaHole;
    }
    const float x = mm(h.x), y = mm(h.y);
    if (round) {
        const float r = mm(std::max(h.w, h.h)) / 2;
        circle(bucket(order_key(kRankHoles, s), Prim::Disc, 0, ObjClass::Holes, kNone32, color, needs), x, y, r, 0);
        return;
    }
    // A slot: a round-capped segment as wide as the slot is narrow, along
    // the hole's long axis rotated by rot_deg (counter-clockwise on screen,
    // y down).
    const double w = std::min(h.w, h.h), len = std::max(h.w, h.h);
    const double a = h.rot_deg * 3.14159265358979323846 / 180.0;
    double dx = std::cos(a), dy = -std::sin(a); // the hole's own x axis
    if (h.h > h.w) {                            // long axis is its own y axis
        const double t = dx;
        dx = -dy;
        dy = t;
    }
    const double half = (len - w) / 2 * 1e-6;
    const auto wn = static_cast<std::int64_t>(w);
    Bucket &b = bucket(order_key(kRankHoles, s), Prim::Segs, wn, ObjClass::Holes, kNone32, color, needs);
    seg(b, static_cast<float>(x - dx * half), static_cast<float>(y - dy * half), static_cast<float>(x + dx * half),
        static_cast<float>(y + dy * half), mm(w) / 2);
}

void Builder::via(const BoardVia &v) {
    const LayerEntry &top = by_index_.at(static_cast<std::size_t>(v.top));
    const LayerEntry &bot = by_index_.at(static_cast<std::size_t>(v.bottom));
    const int lo = std::min(top.cls.number, bot.cls.number), hi = std::max(top.cls.number, bot.cls.number);
    const float x = mm(v.x), y = mm(v.y), r = mm(v.dia) / 2;
    for (const LayerEntry &le : by_index_) {
        if (le.cls.kind != LayerKind::Copper || le.cls.number < lo || le.cls.number > hi) continue;
        Bucket &b = bucket(order_key(le.rank, sub::kVia), Prim::Disc, 0, ObjClass::Vias, le.index,
                           with_opacity(le.color, theme_.via_opacity), 0);
        circle(b, x, y, r, 0);
    }
    if (v.drill <= 0) return;
    const float hr = mm(v.drill) / 2;
    const std::uint32_t needs = bit(ObjClass::Vias);
    const auto wall_nm = static_cast<std::int64_t>(std::llround(theme_.hole_wall_mm * 1e6));
    const float wall = static_cast<float>(theme_.hole_wall_mm);
    circle(bucket(order_key(kRankHoles, sub::kHoleWall), Prim::Ring, wall_nm, ObjClass::Holes, kNone32,
                  theme_.via_hole_wall, needs),
           x, y, hr + wall / 2, wall / 2);
    circle(bucket(order_key(kRankHoles, sub::kViaHole), Prim::Disc, 0, ObjClass::Holes, kNone32, theme_.via_hole,
                  needs),
           x, y, hr, 0);
}

void Builder::track(const BoardTrack &t) {
    const LayerEntry &le = by_index_.at(static_cast<std::size_t>(t.layer_index));
    const Rgba color =
        le.cls.kind == LayerKind::Copper ? with_opacity(le.color, theme_.track_opacity) : le.color;
    Bucket &b = bucket(order_key(le.rank, sub::kTrack), Prim::Segs, t.w, ObjClass::Tracks, le.index, color, 0);
    seg(b, mm(t.x0), mm(t.y0), mm(t.x1), mm(t.y1), mm(t.w) / 2);
}

void Builder::layer_table() {
    by_index_.resize(info_.layers.size());
    for (std::size_t k = 0; k < info_.layers.size(); ++k) {
        const BoardLayer &l = info_.layers[k];
        LayerEntry &e = by_index_[k];
        e.index = static_cast<std::uint32_t>(k);
        e.name = l.name;
        e.cls = classify_layer(l.name);
        e.color = theme_.layer_color(l.name);
        e.rank = draw_rank(e.cls);
        e.panel = panel_rank(e.cls);
        e.kicad_id = l.kicad_id;
        e.type = l.type;
    }
}

DisplayList Builder::run() {
    layer_table();
    for (const Polyset &p : geo_.polysets) {
        polyset(p);
        ++by_index_.at(static_cast<std::size_t>(p.layer_index)).items;
    }
    for (const BoardTrack &t : geo_.tracks) {
        track(t);
        ++by_index_.at(static_cast<std::size_t>(t.layer_index)).items;
    }
    std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t>> via_drills;
    for (const BoardVia &v : geo_.vias) {
        via(v);
        via_drills.insert(std::make_tuple(v.x, v.y, v.drill));
        for (LayerEntry &le : by_index_) {
            const int lo = std::min(by_index_[static_cast<std::size_t>(v.top)].cls.number,
                                    by_index_[static_cast<std::size_t>(v.bottom)].cls.number);
            const int hi = std::max(by_index_[static_cast<std::size_t>(v.top)].cls.number,
                                    by_index_[static_cast<std::size_t>(v.bottom)].cls.number);
            if (le.cls.kind == LayerKind::Copper && le.cls.number >= lo && le.cls.number <= hi) ++le.items;
        }
    }
    for (const Hole &h : geo_.holes) {
        // A via drill written both as a VIA and as a HOLE is drawn once.
        if (h.fp < 0 && h.plated && h.shape == 0 && via_drills.count(std::make_tuple(h.x, h.y, h.w)) != 0) continue;
        hole(h);
    }

    DisplayList out;
    float content[4] = {kInf, kInf, -kInf, -kInf};
    std::uint32_t key = 0;
    for (auto &kv : buckets_) {
        const Key &k = kv.first;
        Bucket &b = kv.second;
        if (b.items == 0) continue;
        Batch bt;
        bt.prim = static_cast<Prim>(k.prim);
        bt.cls = k.cls;
        bt.layer = k.layer;
        bt.order = k.order;
        bt.color = k.color;
        bt.key = key++;
        bt.width = mm(static_cast<double>(k.width_nm));
        bt.needs = k.needs;
        bt.count = b.items;
        for (int i = 0; i < 4; ++i) bt.bbox[static_cast<std::size_t>(i)] = b.bbox[i];
        const auto point_base = static_cast<std::uint32_t>(out.xy.size() / 2);
        const auto contour_base = static_cast<std::uint32_t>(out.contours.size() / 2);
        switch (bt.prim) {
        case Prim::Fill:
            bt.first = static_cast<std::uint32_t>(out.polys.size() / 2);
            for (std::size_t i = 0; i < b.polys.size(); i += 2) {
                out.polys.push_back(b.polys[i] + contour_base);
                out.polys.push_back(b.polys[i + 1]);
            }
            break;
        case Prim::Stroke: bt.first = contour_base; break;
        case Prim::Segs:
            bt.first = static_cast<std::uint32_t>(out.segs.size() / 4);
            out.segs.insert(out.segs.end(), b.segs.begin(), b.segs.end());
            break;
        case Prim::Disc:
        case Prim::Ring:
            bt.first = static_cast<std::uint32_t>(out.circles.size() / 3);
            out.circles.insert(out.circles.end(), b.circles.begin(), b.circles.end());
            break;
        }
        for (std::size_t i = 0; i < b.contours.size(); i += 2) {
            out.contours.push_back(b.contours[i] + point_base);
            out.contours.push_back(b.contours[i + 1]);
        }
        out.xy.insert(out.xy.end(), b.xy.begin(), b.xy.end());
        content[0] = std::min(content[0], b.bbox[0]);
        content[1] = std::min(content[1], b.bbox[1]);
        content[2] = std::max(content[2], b.bbox[2]);
        content[3] = std::max(content[3], b.bbox[3]);
        out.batches.push_back(bt);
    }
    if (content[0] <= content[2]) {
        for (int i = 0; i < 4; ++i) out.content[static_cast<std::size_t>(i)] = content[i];
        out.has_extent = true;
        if (outline_[0] <= outline_[2])
            for (int i = 0; i < 4; ++i) out.extent[static_cast<std::size_t>(i)] = outline_[i];
        else
            out.extent = out.content;
    }
    out.layers = by_index_;
    std::stable_sort(out.layers.begin(), out.layers.end(),
                     [](const LayerEntry &a, const LayerEntry &b) { return a.panel < b.panel; });
    return out;
}

} // namespace

DisplayList build_display_list(const BoardInfo &info, const BoardGeometry &geo, const Theme &theme) {
    return Builder(info, geo, theme).run();
}

} // namespace rv
