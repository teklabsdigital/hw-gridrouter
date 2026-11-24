// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// camera.hpp: the board-mm to view transform.
//
// View coordinates are the client's points, origin top left, y down (as
// KiCad's board y).  view = (board - origin) * scale.  The camera knows the
// view size and backing scale (pixels per point) so it can fit the board and
// report a zoom; clients only apply the transform it returns.
#ifndef RV_CAMERA_HPP
#define RV_CAMERA_HPP

#include <array>

namespace rv {

class Camera {
public:
    // Zoom limits, view points per board mm.
    static constexpr double kMinScale = 0.1;
    static constexpr double kMaxScale = 5000.0;
    // Margin around the board when fitting, as a fraction of each side.
    static constexpr double kFitMargin = 0.04;

    // Sets the view size in points and the backing scale.  The board point
    // at the top-left corner stays put.  A fit requested while the view had
    // no size is done now.
    void set_size(double width, double height, double backing);

    // Fit the extent (min x, min y, max x, max y, mm) into the view.  Without
    // a view size yet, the fit is remembered and done by set_size.
    void fit(const std::array<float, 4> &extent, bool has_extent);

    // Multiply the scale by factor, keeping the board point under view
    // point (vx, vy) fixed.  The scale is clamped to the limits.
    void zoom(double factor, double vx, double vy);

    // Move the content by (dx, dy) view points.
    void pan(double dx, double dy);

    double scale() const { return scale_; }
    double tx() const { return -ox_ * scale_; }
    double ty() const { return -oy_ * scale_; }
    double width() const { return w_; }
    double height() const { return h_; }
    double backing() const { return backing_; }
    // The scale a fit of the extent would give now (scale() if unknown).
    double fit_scale(const std::array<float, 4> &extent, bool has_extent) const;

    void to_board(double vx, double vy, double &bx, double &by) const {
        bx = ox_ + vx / scale_;
        by = oy_ + vy / scale_;
    }
    void to_view(double bx, double by, double &vx, double &vy) const {
        vx = (bx - ox_) * scale_;
        vy = (by - oy_) * scale_;
    }

private:
    static double clamp_scale(double s);
    double w_ = 0, h_ = 0, backing_ = 1;
    double scale_ = 10;   // points per mm
    double ox_ = 0, oy_ = 0; // board mm at the view's top-left corner
    bool fit_pending_ = false;
    std::array<float, 4> pending_extent_{{0, 0, 0, 0}};
    bool pending_has_extent_ = false;
};

} // namespace rv

#endif // RV_CAMERA_HPP
