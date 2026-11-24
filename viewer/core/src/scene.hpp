// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// scene.hpp: what a session shows of its trace: the board under the
// position, layer and object visibility, the theme, the camera, and the
// cached static display list.
//
// The static list is the board drawn (a BRD chunk) or, when there is no
// board at the position, the router outline of the session (routed.hpp).
// It is rebuilt only when that source changes (a new trace, another BRD
// selected or reached by the position) or the theme changes; the last two
// built lists are kept, so moving back and forth across a BRD chunk does not
// rebuild either.  Visibility only refilters the batches.  The camera never
// touches the display list.  Router copper is in the overlay (overlay.hpp),
// not here.
#ifndef RV_SCENE_HPP
#define RV_SCENE_HPP

#include "camera.hpp"
#include "display.hpp"
#include "theme.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rv {

class Trace;

class Scene {
public:
    Scene();

    // A new trace was opened or the trace was closed: forget the cached
    // lists and follow the position again.  Visibility and camera stay.
    void reset();
    // M6: the outline of router session s gained items (the trace grew):
    // forget its cached list.
    void invalidate_outline(std::uint32_t session);

    // Board selection: a BRD index, or kNone32 to follow the position.
    void select(std::uint32_t index) { selection_ = index; }
    std::uint32_t selection() const { return selection_; }
    // What the position shows when following it: a BRD index (kNone32 if
    // none, kFollowLatest for the latest one, the default until a session
    // sets it), and the router session whose outline is drawn when there
    // is no board (kNone32 for nothing).
    static constexpr std::uint32_t kFollowLatest = 0xFFFFFFFEu;
    void set_follow(std::uint32_t board, std::uint32_t outline_session) {
        follow_board_ = board;
        follow_session_ = outline_session;
    }
    // The BRD index drawn for this trace (kNone32 if none).
    std::uint32_t effective_board(const Trace *t) const;
    // The router session whose outline is drawn (kNone32 if a board or
    // nothing is drawn).
    std::uint32_t effective_outline(const Trace *t) const;

    // Visibility.  Layers are keyed by canonical name and keep their state
    // across boards; a name never set uses the default (plan B4).
    bool layer_visible(const std::string &name) const;
    void set_layer_visible(const std::string &name, bool visible);
    bool class_visible(std::uint32_t cls) const { return cls < kClassCount && class_visible_[cls]; }
    void set_class_visible(std::uint32_t cls, bool visible);
    // Increments on every visibility change.
    std::uint32_t visibility_generation() const { return visibility_generation_; }

    // The full list and the visible batches for trace t (nullptr: none).
    // Built or refiltered on demand.
    const DisplayList &full(Trace *t);
    const std::vector<Batch> &visible(Trace *t);

    // Names the geometry of the current list: within one value a batch key
    // always names the same geometry.
    std::uint32_t geometry_generation() const { return geometry_generation_; }
    // Increments whenever the visible list may differ.
    std::uint32_t generation() const { return generation_; }
    // The BRD index the current list was built from (kNone32 if none).
    std::uint32_t built_board() const { return built_board_; }
    // Lists built so far (for tests: a cached list is not rebuilt).
    std::uint32_t builds() const { return builds_; }

    const Theme &theme() const { return theme_; }

    Camera camera;

private:
    struct Built {
        std::uint32_t board = kNone32, session = kNone32;
        std::uint64_t epoch = 0;
        std::uint32_t geometry_generation = 0;
        std::shared_ptr<const DisplayList> list;
    };
    void ensure(Trace *t);

    Theme theme_;
    std::uint32_t selection_ = 0xFFFFFFFFu;
    std::uint32_t follow_board_ = kFollowLatest;
    std::uint32_t follow_session_ = 0xFFFFFFFFu;
    std::map<std::string, bool> layer_override_;
    std::array<bool, kClassCount> class_visible_{};
    std::uint32_t visibility_generation_ = 0;

    std::shared_ptr<const DisplayList> list_;
    std::vector<Built> cache_;    // the last two lists built, most recent first
    std::vector<Batch> visible_;
    bool built_ = false;          // list_ matches (epoch_, built_board_, built_session_)
    bool filtered_ = false;       // visible_ matches list_ and the visibility
    std::uint64_t epoch_ = 1;     // bumped by reset()
    std::uint64_t built_epoch_ = 0;
    std::uint32_t built_board_ = 0xFFFFFFFFu;
    std::uint32_t built_session_ = 0xFFFFFFFFu;
    std::uint32_t geometry_generation_ = 0;
    std::uint32_t next_geometry_generation_ = 0;
    std::uint32_t generation_ = 0;
    std::uint32_t builds_ = 0;
};

} // namespace rv

#endif // RV_SCENE_HPP
