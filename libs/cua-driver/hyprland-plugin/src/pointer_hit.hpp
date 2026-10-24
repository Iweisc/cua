#pragma once

#include <string_view>
#include <utility>

namespace cua::hyprland {
enum class PointerHit { none, root, subsurface, popup };

template <typename Surface, typename Point>
struct PointerSurface {
    PointerHit hit = PointerHit::none;
    Surface surface{};
    Point local{};
};

// The surface a background pointer action lands on. Like Hyprland's own
// pointer (CViewHitTester::windowSurfaceAt), it is the window's topmost popup
// at the point, else the surface of its tree there. Popups are not in that
// tree, so a tree hit alone looks through an open menu at the widget under it.
// `popup` and `tree` are a surface (empty when nothing is hit) and the point
// in that surface's coordinates. Both honor input regions, so a click-through
// tooltip does not take the click: `popup` is m_popupHead->at(global, true)
// and `tree` is root->at(local, true).
template <typename Surface, typename Point>
[[nodiscard]] PointerSurface<Surface, Point> classify_pointer_hit(
    const Surface& root, const std::pair<Surface, Point>& popup, const std::pair<Surface, Point>& tree) {
    if (popup.first) return {PointerHit::popup, popup.first, popup.second};
    if (!tree.first) return {};
    return {tree.first == root ? PointerHit::root : PointerHit::subsurface, tree.first, tree.second};
}

// Why a pointer action at a window-local point refuses, or empty to send it to
// its hit. A click or scroll may land on a popup that extends past the window.
// A drag's later motion goes to the window's own surface, so it must not start
// on anything drawn over that surface. `human_grab` says the person's popup
// grab holds the hit popup: that menu is the person's.
[[nodiscard]] constexpr std::string_view pointer_refusal(bool inside, PointerHit hit, bool drag, bool human_grab) {
    if (!inside && (drag || hit != PointerHit::popup)) return "point_outside_window";
    if (drag && hit == PointerHit::popup) return "popup_target";
    if (drag && hit == PointerHit::subsurface) return "subsurface_target";
    if (hit == PointerHit::popup && human_grab) return "primary_grab";
    return {};
}
} // namespace cua::hyprland
