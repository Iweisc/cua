#include "pointer_hit.hpp"

#include <cstdlib>
#include <memory>
#include <utility>

namespace {
void check(bool condition) { if (!condition) std::abort(); }

struct Point { double x = 0, y = 0; };
using Surface = std::shared_ptr<int>;
using Hit = std::pair<Surface, Point>;
} // namespace

int main() {
    using namespace cua::hyprland;
    const auto root = std::make_shared<int>(1);
    const auto subsurface = std::make_shared<int>(2);
    const auto menu = std::make_shared<int>(3);
    const Hit nothing{};

    // An open menu wins over the widget the tree finds under it, and gets the
    // point in its own coordinates.
    const auto on_menu = classify_pointer_hit(root, Hit{menu, {4, 5}}, Hit{root, {40, 50}});
    check(on_menu.hit == PointerHit::popup && on_menu.surface == menu && on_menu.local.x == 4 && on_menu.local.y == 5);
    check(classify_pointer_hit(root, Hit{menu, {4, 5}}, nothing).hit == PointerHit::popup);
    const auto on_root = classify_pointer_hit(root, nothing, Hit{root, {40, 50}});
    check(on_root.hit == PointerHit::root && on_root.surface == root && on_root.local.x == 40);
    const auto on_subsurface = classify_pointer_hit(root, nothing, Hit{subsurface, {1, 2}});
    check(on_subsurface.hit == PointerHit::subsurface && on_subsurface.surface == subsurface && on_subsurface.local.y == 2);
    check(classify_pointer_hit(root, nothing, nothing).hit == PointerHit::none);
    check(!classify_pointer_hit(root, nothing, nothing).surface);

    // Clicks and scrolls go to whatever is drawn at the point, a menu past
    // the window's edge included.
    for (const auto hit : {PointerHit::root, PointerHit::subsurface, PointerHit::popup})
        check(pointer_refusal(true, hit, false, false).empty());
    check(pointer_refusal(false, PointerHit::popup, false, false).empty());
    for (const auto hit : {PointerHit::none, PointerHit::root, PointerHit::subsurface})
        check(pointer_refusal(false, hit, false, false) == "point_outside_window");

    // A drag stays on the window's own surface.
    check(pointer_refusal(true, PointerHit::root, true, false).empty());
    check(pointer_refusal(true, PointerHit::popup, true, false) == "popup_target");
    check(pointer_refusal(true, PointerHit::subsurface, true, false) == "subsurface_target");
    check(pointer_refusal(false, PointerHit::popup, true, false) == "point_outside_window");

    // A menu the person's grab holds is the person's.
    check(pointer_refusal(true, PointerHit::popup, false, true) == "primary_grab");
    check(pointer_refusal(false, PointerHit::popup, false, true) == "primary_grab");
}
