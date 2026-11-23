// routed.hpp: router geometry in board millimetres.
//
//   - Frame: router cells and router-grid mm to board mm (spec 9).
//   - CopperGeom: what one copper item draws (spec 7.12 for a CMIT, 7.6 for
//     a VIA), kept in doubles.
//   - build_outline_list: the static list drawn when there is no board: the
//     grid extent and the session's pads, claims, keep-outs and discs as
//     outlines.
#ifndef RV_ROUTED_HPP
#define RV_ROUTED_HPP

#include "display.hpp"
#include "records.hpp"
#include "theme.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace rv {

class Trace;

struct CellPos {
    std::uint32_t layer = 0;
    std::int32_t i = 0, j = 0;
};

// Coordinates of one router session (spec 9).  Router-grid mm x becomes
// board nm origin_x_nm + llround(x * 1e6), with origin_x_nm =
// llround(RUN.origin_x * 1e6); board mm is that times 1e-6.
class Frame {
public:
    explicit Frame(const Run &r)
        : ox_(std::llround(r.origin_x * 1e6)), oy_(std::llround(r.origin_y * 1e6)), cell_(r.cell), nx_(r.nx),
          nc_(r.nc()) {}
    double x(double router_mm) const { return static_cast<double>(ox_ + std::llround(router_mm * 1e6)) * 1e-6; }
    double y(double router_mm) const { return static_cast<double>(oy_ + std::llround(router_mm * 1e6)) * 1e-6; }
    double cx(std::int32_t i) const { return x(i * cell_); }
    double cy(std::int32_t j) const { return y(j * cell_); }
    double cell() const { return cell_; }
    CellPos cell(std::uint32_t idx) const {
        CellPos p;
        if (nc_ == 0 || nx_ <= 0) return p;
        p.layer = static_cast<std::uint32_t>(idx / nc_);
        const std::uint64_t rem = idx % nc_;
        p.j = static_cast<std::int32_t>(rem / static_cast<std::uint64_t>(nx_));
        p.i = static_cast<std::int32_t>(rem % static_cast<std::uint64_t>(nx_));
        return p;
    }

private:
    long long ox_, oy_;
    double cell_;
    std::int32_t nx_;
    std::uint64_t nc_;
};

// What one copper item draws, in board mm.
struct CopperGeom {
    struct Line {
        std::uint32_t layer = 0;           // router layer
        std::uint32_t first = 0, count = 0; // points pts[2 * first ...]; count 1 is a dot
    };
    struct Seg {
        std::uint32_t layer = 0;
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0, w = 0;
    };
    bool drawable = false;
    std::uint32_t session = kNone32;
    double width = 0;                  // track width of the path, mm
    double via_dia = 0, via_drill = 0; // from the RUN
    std::vector<double> pts;           // x, y
    std::vector<Line> lines;           // the path: one polyline per run of cells on one layer
    std::vector<Seg> stubs;
    std::vector<std::array<double, 2>> vias; // drawn on both router layers
    std::array<double, 4> bbox{{0, 0, 0, 0}};
};

// A CMIT's geometry: each maximal run of consecutive path cells on one layer
// becomes one polyline through their centres (collinear interior points
// dropped; a run whose cells are all one point becomes a dot); a layer
// change draws nothing itself.  Stubs as segments, listed vias inside the
// grid as discs.
CopperGeom commit_geometry(const Run &run, const Commit &c);
// A standalone VIA's geometry (nothing if it lies outside the grid).
CopperGeom via_geometry(const Run &run, const Via &v);
// Geometry of every copper item of the trace, indexed like Trace::copper().
std::vector<CopperGeom> build_copper_geometry(const Trace &t);

// The list drawn with no board: layers are the session's two router layers
// (indices 0 and 1, or one if both have the same name) and "Edge.Cuts" for
// the grid extent.
DisplayList build_outline_list(const Trace &t, std::uint32_t session, const Theme &theme);

} // namespace rv

#endif // RV_ROUTED_HPP
