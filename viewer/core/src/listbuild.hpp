// listbuild.hpp: a small display-list builder for router content (the
// outline list drawn without a board, and the overlay of routed copper,
// ghosts, markers and highlights).
//
// Items go into buckets keyed by (order, primitive, width, class, layer,
// colour, needs, group); finish() emits one batch per non-empty bucket in
// key order, so the list is sorted bottom to top and deterministic.  `group`
// splits otherwise equal buckets (the overlay groups copper in blocks so a
// client can cache each block's path).
#ifndef RV_LISTBUILD_HPP
#define RV_LISTBUILD_HPP

#include "display.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <tuple>
#include <vector>

namespace rv {

struct LKey {
    std::uint32_t order = 0, prim = 0;
    std::int64_t width_nm = 0;
    std::uint32_t cls = 0, layer = kNone32;
    Rgba color = 0;
    std::uint32_t needs = 0;
    std::uint64_t group = 0;
    bool operator<(const LKey &o) const {
        return std::tie(order, prim, width_nm, cls, layer, color, needs, group) <
               std::tie(o.order, o.prim, o.width_nm, o.cls, o.layer, o.color, o.needs, o.group);
    }
};

struct LBucket {
    std::vector<float> xy;
    std::vector<std::uint32_t> contours; // first point (local), count
    std::vector<float> segs, circles;
    std::uint32_t items = 0;
    std::uint64_t mask = 0; // free for the caller: which members of the group are in it
    float bbox[4] = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};

    void grow(double x, double y, double r) {
        bbox[0] = std::min(bbox[0], static_cast<float>(x - r));
        bbox[1] = std::min(bbox[1], static_cast<float>(y - r));
        bbox[2] = std::max(bbox[2], static_cast<float>(x + r));
        bbox[3] = std::max(bbox[3], static_cast<float>(y + r));
    }
    // An open polyline of n points (xy pairs); one point becomes a dot.
    void line(const double *p, std::size_t n, double half_width) {
        if (n == 0) return;
        contours.push_back(static_cast<std::uint32_t>(xy.size() / 2));
        contours.push_back(static_cast<std::uint32_t>(n == 1 ? 2 : n));
        for (std::size_t k = 0; k < n; ++k) {
            xy.push_back(static_cast<float>(p[2 * k]));
            xy.push_back(static_cast<float>(p[2 * k + 1]));
            grow(p[2 * k], p[2 * k + 1], half_width);
        }
        if (n == 1) {
            xy.push_back(static_cast<float>(p[0]));
            xy.push_back(static_cast<float>(p[1]));
        }
        ++items;
    }
    void seg(double x0, double y0, double x1, double y1, double half_width) {
        segs.insert(segs.end(), {static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1),
                                 static_cast<float>(y1)});
        grow(x0, y0, half_width);
        grow(x1, y1, half_width);
        ++items;
    }
    void circle(double x, double y, double r, double pad) {
        circles.insert(circles.end(), {static_cast<float>(x), static_cast<float>(y), static_cast<float>(r)});
        grow(x, y, r + pad);
        ++items;
    }
};

class ListBuilder {
public:
    LBucket &at(std::uint32_t order, Prim prim, double width_mm, std::uint32_t cls, std::uint32_t layer, Rgba color,
                std::uint32_t needs, std::uint64_t group = 0) {
        LKey k;
        k.order = order;
        k.prim = static_cast<std::uint32_t>(prim);
        k.width_nm = std::llround(width_mm * 1e6);
        k.cls = cls;
        k.layer = layer;
        k.color = color;
        k.needs = needs;
        k.group = group;
        return buckets_[k];
    }

    // Emit the batches into out (appending to its arrays).  key(k, b) gives
    // each batch's key.  Returns the union of the bboxes (inf if none).
    template <class KeyFn>
    std::array<float, 4> finish(DisplayList &out, KeyFn key) const {
        std::array<float, 4> all{{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                                  -std::numeric_limits<float>::infinity(),
                                  -std::numeric_limits<float>::infinity()}};
        for (const auto &kv : buckets_) {
            const LKey &k = kv.first;
            const LBucket &b = kv.second;
            if (b.items == 0) continue;
            Batch bt;
            bt.prim = static_cast<Prim>(k.prim);
            bt.cls = k.cls;
            bt.layer = k.layer;
            bt.order = k.order;
            bt.color = k.color;
            bt.key = key(k, b);
            bt.width = static_cast<float>(static_cast<double>(k.width_nm) * 1e-6);
            bt.needs = k.needs;
            bt.count = b.items;
            for (int i = 0; i < 4; ++i) bt.bbox[static_cast<std::size_t>(i)] = b.bbox[i];
            const auto point_base = static_cast<std::uint32_t>(out.xy.size() / 2);
            const auto contour_base = static_cast<std::uint32_t>(out.contours.size() / 2);
            switch (bt.prim) {
            case Prim::Fill: bt.first = static_cast<std::uint32_t>(out.polys.size() / 2); break; // not produced
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
            for (int i = 0; i < 2; ++i) all[static_cast<std::size_t>(i)] = std::min(all[static_cast<std::size_t>(i)], b.bbox[i]);
            for (int i = 2; i < 4; ++i) all[static_cast<std::size_t>(i)] = std::max(all[static_cast<std::size_t>(i)], b.bbox[i]);
            out.batches.push_back(bt);
        }
        return all;
    }

    void clear() { buckets_.clear(); }

private:
    std::map<LKey, LBucket> buckets_;
};

} // namespace rv

#endif // RV_LISTBUILD_HPP
