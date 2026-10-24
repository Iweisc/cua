#include "cursor_layer.hpp"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {
void check(bool condition) { if (!condition) std::abort(); }

struct Layer {};
struct Box {
    int x, y, w, h;
};
// Hyprland reuses a freed window's memory, so a test window carries the
// address it stands for instead of relying on the allocator.
struct Window {
    std::uintptr_t address;
    Box box;
    std::uint64_t stable_id;
};
// GCC 16 reports a false -Warray-bounds for make_shared of this aggregate at -O3.
std::shared_ptr<Window> make_window(std::uintptr_t address, Box box, std::uint64_t stable_id = 1) {
    return std::shared_ptr<Window>(new Window{address, box, stable_id});
}
using Pins = cua::hyprland::CursorPins<std::weak_ptr<Layer>, std::weak_ptr<Window>, Box>;
const auto address = [](const std::shared_ptr<Window>& window) { return window->address; };
const auto stable_id = [](const std::shared_ptr<Window>& window) { return window->stable_id; };
const auto box = [](const std::shared_ptr<Window>& window) { return window->box; };
}

int main() {
    using cua::hyprland::pinned_cursor_window;
    const auto plain = pinned_cursor_window("cua-agent-cursor@55d0c1a2b3f0");
    check(plain && plain->address == 0x55d0c1a2b3f0 && !plain->stable_id);
    const auto named = pinned_cursor_window("cua-agent-cursor@55d0c1a2b3f0.18000004");
    check(named && named->address == 0x55d0c1a2b3f0 && named->stable_id == 0x18000004u);
    check(!pinned_cursor_window("cua-agent-cursor"));
    check(!pinned_cursor_window("cua-agent-cursor@"));
    check(!pinned_cursor_window("cua-agent-cursor@0"));
    check(!pinned_cursor_window("cua-agent-cursor@0x55d0"));
    check(!pinned_cursor_window("cua-agent-cursor@55d0 "));
    check(!pinned_cursor_window("cua-agent-cursor@55d0."));
    check(!pinned_cursor_window("cua-agent-cursor@55d0.4.5"));
    check(!pinned_cursor_window("cua-agent-cursor@55d0-4"));
    check(!pinned_cursor_window("cua-agent-cursor@ffffffffffffffffff"));
    check(!pinned_cursor_window("waybar@55d0"));

    const Box placed{200, 200, 600, 400};
    Pins pins;
    auto target = make_window(0x55d0, placed, 4);
    const auto neighbour = make_window(0x77e0, placed);
    const auto layer = std::make_shared<Layer>();
    const auto unpinned = std::make_shared<Layer>();
    std::vector<std::shared_ptr<Window>> windows{target, neighbour};
    pins.open(layer, "cua-agent-cursor@55d0", windows, address, stable_id, box);
    pins.open(unpinned, "cua-agent-cursor", windows, address, stable_id, box);
    check(pins.above(layer, target));
    check(!pins.above(layer, neighbour));
    check(!pins.above(unpinned, target) && !pins.above(unpinned, neighbour));

    int closed = 0;
    std::shared_ptr<Layer> expected = layer;
    const auto close = [&](const std::shared_ptr<Layer>& released) { check(released == expected); ++closed; };
    pins.release(close);
    // The spot the cursor points at stays inside a window that keeps its
    // corner and grows, and another window's move is not the target's.
    pins.release_moved(target, Box{200, 200, 900, 700}, close);
    pins.release_moved(target, placed, close);
    pins.release_moved(neighbour, Box{1200, 600, 600, 400}, close);
    check(closed == 0 && pins.above(layer, target));

    // The target closes, maybe on a hidden workspace where it never drew its
    // cursor, and the next window takes its address.
    windows = {neighbour};
    target.reset();
    const auto successor = make_window(0x55d0, placed, 5);
    windows.push_back(successor);
    check(!pins.above(layer, successor));
    pins.release(close);
    pins.release(close);
    check(closed == 1);
    check(!pins.above(layer, successor));

    // Driver opens a new layer when it pins a cursor to the successor.
    expected = std::make_shared<Layer>();
    pins.open(expected, "cua-agent-cursor@55d0", windows, address, stable_id, box);
    check(pins.above(expected, successor));
    check(!pins.above(layer, successor));

    // The successor moves, and the cursor painted for its old box is released.
    pins.release_moved(successor, Box{1200, 600, 600, 400}, close);
    pins.release_moved(successor, Box{1200, 600, 600, 400}, close);
    check(closed == 2);
    check(!pins.above(expected, successor));

    // A shrinking window may no longer hold the spot either.
    expected = std::make_shared<Layer>();
    pins.open(expected, "cua-agent-cursor@55d0", windows, address, stable_id, box);
    pins.release_moved(successor, Box{200, 200, 600, 300}, close);
    check(closed == 3);

    // A layer Driver named for the closed target opens only after the next
    // window took the address, and so belongs to no window.
    const auto stale = std::make_shared<Layer>();
    pins.open(stale, "cua-agent-cursor@55d0.4", windows, address, stable_id, box);
    check(!pins.above(stale, successor));
    const auto current = std::make_shared<Layer>();
    pins.open(current, "cua-agent-cursor@55d0.5", windows, address, stable_id, box);
    check(pins.above(current, successor));

    // A layer that opens after its window is gone belongs to no window.
    const auto late = std::make_shared<Layer>();
    pins.open(late, "cua-agent-cursor@99f0", windows, address, stable_id, box);
    windows.push_back(make_window(0x99f0, placed));
    check(!pins.above(late, windows.back()));
}
