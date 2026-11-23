// scene.cpp: the board under the position, visibility and the cached static
// display list.

#include "scene.hpp"

#include "routed.hpp"
#include "trace.hpp"

#include <stdexcept>

namespace rv {

namespace {
const DisplayList &empty_list() {
    static const DisplayList empty;
    return empty;
}
} // namespace

Scene::Scene() {
    class_visible_.fill(true);
}

void Scene::reset() {
    ++epoch_;
    selection_ = kNone32;
    follow_board_ = kFollowLatest;
    follow_session_ = kNone32;
    built_ = false;
    filtered_ = false;
    list_.reset();
    cache_.clear();
    visible_.clear();
    built_board_ = kNone32;
    built_session_ = kNone32;
    geometry_generation_ = ++next_geometry_generation_;
    ++generation_;
}

std::uint32_t Scene::effective_board(const Trace *t) const {
    if (t == nullptr || t->boards().empty()) return kNone32;
    const auto n = static_cast<std::uint32_t>(t->boards().size());
    std::uint32_t b = selection_ == kNone32 ? follow_board_ : selection_;
    if (b == kFollowLatest) b = n - 1;
    return b < n ? b : kNone32;
}

std::uint32_t Scene::effective_outline(const Trace *t) const {
    if (t == nullptr || effective_board(t) != kNone32 || selection_ != kNone32) return kNone32;
    return follow_session_ < t->runs().size() ? follow_session_ : kNone32;
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
    ++visibility_generation_;
}

void Scene::set_class_visible(std::uint32_t cls, bool visible) {
    if (cls >= kClassCount)
        throw std::invalid_argument("invalid argument: no object class " + std::to_string(cls));
    if (class_visible_[cls] == visible) return;
    class_visible_[cls] = visible;
    filtered_ = false;
    ++visibility_generation_;
}

void Scene::ensure(Trace *t) {
    const std::uint32_t board = effective_board(t);
    const std::uint32_t session = effective_outline(t);
    if (!built_ || built_epoch_ != epoch_ || built_board_ != board || built_session_ != session) {
        Built found;
        for (const Built &b : cache_)
            if (b.epoch == epoch_ && b.board == board && b.session == session) found = b;
        if (!found.list) {
            found.board = board;
            found.session = session;
            found.epoch = epoch_;
            if (board != kNone32) {
                std::shared_ptr<const BoardGeometry> geo = t->board_geometry(board);
                found.list = std::make_shared<const DisplayList>(build_display_list(t->boards()[board], *geo, theme_));
            } else if (session != kNone32) {
                found.list = std::make_shared<const DisplayList>(build_outline_list(*t, session, theme_));
            } else {
                found.list = std::make_shared<const DisplayList>();
            }
            found.geometry_generation = ++next_geometry_generation_;
            ++builds_;
        }
        // Most recent first, at most two.
        std::vector<Built> next{found};
        for (const Built &b : cache_)
            if (next.size() < 2 && !(b.epoch == found.epoch && b.board == found.board && b.session == found.session))
                next.push_back(b);
        cache_ = next;
        list_ = found.list;
        built_ = true;
        built_epoch_ = epoch_;
        built_board_ = board;
        built_session_ = session;
        filtered_ = false;
        geometry_generation_ = found.geometry_generation;
    }
    if (!filtered_) {
        const DisplayList &l = list_ ? *list_ : empty_list();
        std::vector<bool> by_index;
        for (const LayerEntry &le : l.layers) {
            if (by_index.size() <= le.index) by_index.resize(le.index + 1, true);
            by_index[le.index] = layer_visible(le.name);
        }
        std::uint32_t classes = 0;
        for (std::uint32_t c = 0; c < kClassCount; ++c)
            if (class_visible_[c]) classes |= 1u << c;
        visible_.clear();
        for (const Batch &b : l.batches) {
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
    return list_ ? *list_ : empty_list();
}

const std::vector<Batch> &Scene::visible(Trace *t) {
    ensure(t);
    return visible_;
}

} // namespace rv
