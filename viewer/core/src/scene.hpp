// scene.hpp: what a session shows of its trace: the selected board, layer
// and object visibility, the theme, the camera, and the cached display list.
//
// The full display list is rebuilt only when the board drawn changes (a new
// trace, another BRD selected, or a new latest BRD after a refresh) or the
// theme changes.  Visibility only refilters the batches.  The camera never
// touches the display list.
#ifndef RV_SCENE_HPP
#define RV_SCENE_HPP

#include "camera.hpp"
#include "display.hpp"
#include "theme.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace rv {

class Trace;

class Scene {
public:
    Scene();

    // A new trace was opened or the trace was closed: forget the cached
    // list and follow the latest board again.  Visibility and camera stay.
    void reset();

    // Board selection: a BRD index, or kNone32 for "the latest board".
    void select(std::uint32_t index) {
        selection_ = index;
    }
    std::uint32_t selection() const { return selection_; }
    // The BRD index drawn for this trace (kNone32 if it has no board).
    std::uint32_t effective_board(const Trace *t) const;

    // Visibility.  Layers are keyed by canonical name and keep their state
    // across boards; a name never set uses the default (plan B4).
    bool layer_visible(const std::string &name) const;
    void set_layer_visible(const std::string &name, bool visible);
    bool class_visible(std::uint32_t cls) const { return cls < kClassCount && class_visible_[cls]; }
    void set_class_visible(std::uint32_t cls, bool visible);

    // The full list and the visible batches for trace t (nullptr: none).
    // Built or refiltered on demand.
    const DisplayList &full(Trace *t);
    const std::vector<Batch> &visible(Trace *t);

    // Increments when the full list is rebuilt (geometry may differ).
    std::uint32_t geometry_generation() const { return geometry_generation_; }
    // Increments whenever the visible list may differ.
    std::uint32_t generation() const { return generation_; }
    // The BRD index the cached list was built from (kNone32 if none).
    std::uint32_t built_board() const { return built_board_; }

    const Theme &theme() const { return theme_; }

    Camera camera;

private:
    void ensure(Trace *t);

    Theme theme_;
    std::uint32_t selection_ = 0xFFFFFFFFu;
    std::map<std::string, bool> layer_override_;
    std::array<bool, kClassCount> class_visible_{};

    DisplayList list_;
    std::vector<Batch> visible_;
    bool built_ = false;          // list_ matches (epoch_, built_board_)
    bool filtered_ = false;       // visible_ matches list_ and the visibility
    std::uint64_t epoch_ = 1;     // bumped by reset()
    std::uint64_t built_epoch_ = 0;
    std::uint32_t built_board_ = 0xFFFFFFFFu;
    std::uint32_t geometry_generation_ = 0;
    std::uint32_t generation_ = 0;
};

} // namespace rv

#endif // RV_SCENE_HPP
