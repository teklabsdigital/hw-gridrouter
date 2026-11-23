// scene.cpp: board selection, visibility and the cached display list.

#include "scene.hpp"

#include "trace.hpp"

#include <stdexcept>

namespace rv {

Scene::Scene() {
    class_visible_.fill(true);
}

void Scene::reset() {
    ++epoch_;
    selection_ = kNone32;
    built_ = false;
    filtered_ = false;
    list_ = DisplayList();
    visible_.clear();
    built_board_ = kNone32;
    ++geometry_generation_;
    ++generation_;
}

std::uint32_t Scene::effective_board(const Trace *t) const {
    if (t == nullptr || t->boards().empty()) return kNone32;
    const auto n = static_cast<std::uint32_t>(t->boards().size());
    if (selection_ == kNone32) return n - 1;
    return selection_ < n ? selection_ : kNone32;
}

bool Scene::layer_visible(const std::string &name) const {
    auto it = layer_override_.find(name);
    if (it != layer_override_.end()) return it->second;
    return default_layer_visible(classify_layer(name));
}

void Scene::set_layer_visible(const std::string &name, bool visible) {
    if (layer_visible(name) == visible && layer_override_.count(name) != 0) return;
    layer_override_[name] = visible;
    filtered_ = false;
}

void Scene::set_class_visible(std::uint32_t cls, bool visible) {
    if (cls >= kClassCount)
        throw std::invalid_argument("invalid argument: no object class " + std::to_string(cls));
    if (class_visible_[cls] == visible) return;
    class_visible_[cls] = visible;
    filtered_ = false;
}

void Scene::ensure(Trace *t) {
    const std::uint32_t board = effective_board(t);
    if (!built_ || built_epoch_ != epoch_ || built_board_ != board) {
        if (board == kNone32) {
            list_ = DisplayList();
        } else {
            std::shared_ptr<const BoardGeometry> geo = t->board_geometry(board);
            list_ = build_display_list(t->boards()[board], *geo, theme_);
        }
        built_ = true;
        built_epoch_ = epoch_;
        built_board_ = board;
        filtered_ = false;
        ++geometry_generation_;
    }
    if (!filtered_) {
        std::vector<bool> by_index;
        for (const LayerEntry &le : list_.layers) {
            if (by_index.size() <= le.index) by_index.resize(le.index + 1, true);
            by_index[le.index] = layer_visible(le.name);
        }
        std::uint32_t classes = 0;
        for (std::uint32_t c = 0; c < kClassCount; ++c)
            if (class_visible_[c]) classes |= 1u << c;
        visible_.clear();
        for (const Batch &b : list_.batches) {
            if ((b.needs & classes) != b.needs) continue;
            if (b.layer != kNone32 && b.layer < by_index.size() && !by_index[b.layer]) continue;
            visible_.push_back(b);
        }
        filtered_ = true;
        ++generation_;
    }
}

const DisplayList &Scene::full(Trace *t) {
    ensure(t);
    return list_;
}

const std::vector<Batch> &Scene::visible(Trace *t) {
    ensure(t);
    return visible_;
}

} // namespace rv
