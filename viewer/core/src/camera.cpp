// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// camera.cpp: the board-mm to view transform (see camera.hpp).

#include "camera.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rv {

namespace {

bool finite_positive(double v) { return std::isfinite(v) && v > 0; }

} // namespace

double Camera::clamp_scale(double s) {
    return std::min(kMaxScale, std::max(kMinScale, s));
}

void Camera::set_size(double width, double height, double backing) {
    if (!std::isfinite(width) || !std::isfinite(height) || width < 0 || height < 0)
        throw std::invalid_argument("invalid argument: view size must be finite and not negative");
    if (!finite_positive(backing)) throw std::invalid_argument("invalid argument: backing scale must be positive");
    w_ = width;
    h_ = height;
    backing_ = backing;
    if (fit_pending_ && w_ > 0 && h_ > 0) fit(pending_extent_, pending_has_extent_);
}

double Camera::fit_scale(const std::array<float, 4> &e, bool has_extent) const {
    if (!has_extent || w_ <= 0 || h_ <= 0) return scale_;
    const double bw = std::max(1e-3, static_cast<double>(e[2]) - e[0]);
    const double bh = std::max(1e-3, static_cast<double>(e[3]) - e[1]);
    const double usable = 1.0 - 2.0 * kFitMargin;
    return clamp_scale(std::min(w_ * usable / bw, h_ * usable / bh));
}

void Camera::fit(const std::array<float, 4> &e, bool has_extent) {
    if (w_ <= 0 || h_ <= 0) {
        fit_pending_ = true;
        pending_extent_ = e;
        pending_has_extent_ = has_extent;
        return;
    }
    fit_pending_ = false;
    if (!has_extent) {
        scale_ = 10;
        ox_ = -w_ / 2 / scale_;
        oy_ = -h_ / 2 / scale_;
        return;
    }
    scale_ = fit_scale(e, true);
    const double cx = (static_cast<double>(e[0]) + e[2]) / 2, cy = (static_cast<double>(e[1]) + e[3]) / 2;
    ox_ = cx - w_ / 2 / scale_;
    oy_ = cy - h_ / 2 / scale_;
}

void Camera::zoom(double factor, double vx, double vy) {
    if (!finite_positive(factor)) throw std::invalid_argument("invalid argument: zoom factor must be positive");
    if (!std::isfinite(vx) || !std::isfinite(vy)) throw std::invalid_argument("invalid argument: zoom centre");
    double bx, by;
    to_board(vx, vy, bx, by);
    scale_ = clamp_scale(scale_ * factor);
    ox_ = bx - vx / scale_;
    oy_ = by - vy / scale_;
}

void Camera::pan(double dx, double dy) {
    if (!std::isfinite(dx) || !std::isfinite(dy)) throw std::invalid_argument("invalid argument: pan delta");
    ox_ -= dx / scale_;
    oy_ -= dy / scale_;
}

} // namespace rv
