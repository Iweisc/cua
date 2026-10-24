#pragma once

#include "target_identity.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace cua::hyprland {
struct PinnedCursorWindow {
    std::uintptr_t address;
    std::optional<std::uint64_t> stable_id;
};

// Driver names an agent-cursor layer after the window its cursor is pinned
// above, as `cua-agent-cursor@<hex window address>`, followed by
// `.<hex stable id>` when Driver knows which window it returned that address
// for. Its plain `cua-agent-cursor` layer is not pinned and stays above every
// window.
[[nodiscard]] inline std::optional<PinnedCursorWindow> pinned_cursor_window(std::string_view name) {
    constexpr std::string_view prefix = "cua-agent-cursor@";
    if (!name.starts_with(prefix)) return std::nullopt;
    name.remove_prefix(prefix.size());
    const auto* const last = name.data() + name.size();
    std::uintptr_t address = 0;
    const auto [end, error] = std::from_chars(name.data(), last, address, 16);
    if (error != std::errc{} || address == 0) return std::nullopt;
    if (end == last) return PinnedCursorWindow{address, std::nullopt};
    std::uint64_t stable_id = 0;
    if (*end != '.') return std::nullopt;
    const auto [id_end, id_error] = std::from_chars(end + 1, last, stable_id, 16);
    if (id_error != std::errc{} || id_end != last) return std::nullopt;
    return PinnedCursorWindow{address, stable_id};
}

// Driver places the cursor where it acted, relative to the window's top-left
// corner, from the box it read then. The cursor still points at that spot
// while the window keeps its corner and does not shrink.
template <typename Box>
[[nodiscard]] bool cursor_still_placed(const Box& placed, const Box& now) {
    return now.x == placed.x && now.y == placed.y && now.w >= placed.w && now.h >= placed.h;
}

// Hyprland frees a closed window and hands its address to the next window it
// creates, so the address alone does not name a pinned layer's window. A
// layer belongs to the window at its address when the layer opened, if that
// window carries the stable id the name gives, and to no other window after
// that one is gone. It also stops matching once the window moves or shrinks,
// and a layer that matches no window when it opens is closed at once. Driver
// opens a new layer when it pins a cursor again. References are weak, so a
// pin never keeps either alive.
template <typename LayerRef, typename WindowRef, typename Box>
class CursorPins {
  public:
    template <typename Layer, typename Windows, typename Address, typename StableId, typename BoxOf, typename Close>
    void open(const Layer& layer, std::string_view name, const Windows& windows, Address address,
              StableId stable_id, BoxOf box, Close close) {
        const auto pinned = pinned_cursor_window(name);
        if (!pinned) return;
        std::erase_if(pins_, [](const Pin& pin) { return pin.layer.expired(); });
        for (const auto& window : windows)
            if (address(window) == pinned->address && target_identity_matches(pinned->stable_id, stable_id(window))) {
                pins_.push_back({layer, window, box(window)});
                return;
            }
        // The layer opened after its window was gone, maybe once another
        // window took the address. Driver keeps one layer per address, so this
        // one would keep the cursor it pins to that window from being drawn.
        // Once it is closed, Driver's next action opens a layer named for the
        // window it now has there.
        close(layer);
    }
    template <typename Layer, typename Window>
    [[nodiscard]] bool above(const Layer& layer, const Window& window) const {
        return std::ranges::any_of(pins_, [&](const Pin& pin) { return pin.layer.lock() == layer && pin.window.lock() == window; });
    }
    // Hands each live layer whose window is gone to `close`, once.
    template <typename Close>
    void release(Close close) {
        std::erase_if(pins_, [&](const Pin& pin) {
            if (!pin.window.expired()) return false;
            if (const auto layer = pin.layer.lock()) close(layer);
            return true;
        });
    }
    // Hands each live layer pinned to `window` to `close`, once, if the
    // window has moved or shrunk to `box` since the layer opened.
    template <typename Window, typename Close>
    void release_moved(const Window& window, const Box& box, Close close) {
        std::erase_if(pins_, [&](const Pin& pin) {
            if (pin.window.lock() != window || cursor_still_placed(pin.box, box)) return false;
            if (const auto layer = pin.layer.lock()) close(layer);
            return true;
        });
    }

  private:
    struct Pin {
        LayerRef layer;
        WindowRef window;
        Box box;
    };
    std::vector<Pin> pins_;
};
} // namespace cua::hyprland
