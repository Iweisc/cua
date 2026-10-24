#pragma once

#include <algorithm>
#include <vector>

namespace cua::hyprland {
// Returns the window a new top-level window reacts to, or an empty Window
// when it is not a reaction to agent input. `parents` is its parent chain,
// nearest first. The nearest parent that is the human's focused window or a
// window the agent is driving decides, so a dialog of the human's own window
// stays theirs even when the agent drives another window of the same app.
// Without such a parent, `app_target` is the agent's window in the new
// window's app when the human is not in that app, and empty otherwise.
// A reaction opens with its parent, or, having none, with that agent window,
// as a background app's window opens in that app's Space on macOS.
template <typename Window, typename Driven>
[[nodiscard]] Window reaction_anchor(const std::vector<Window>& parents, const Window& human, Driven driven, const Window& app_target) {
    for (const auto& parent : parents) {
        if (parent == human) return {};
        if (driven(parent)) return parents.front();
    }
    if (app_target == Window{}) return {};
    return parents.empty() ? app_target : parents.front();
}

// Hyprland ends a fullscreen or maximized window when a new tiled window
// opens on its workspace, so a reaction window opens floating there.
[[nodiscard]] constexpr bool reaction_floats(bool floating, bool workspace_fullscreen) {
    return floating || workspace_fullscreen;
}

// Moves `window` directly below `human` when it is above it. Every other
// window keeps its place.
template <typename Window>
void stack_below(std::vector<Window>& order, const Window& window, const Window& human) {
    const auto w = std::ranges::find(order, window);
    const auto h = std::ranges::find(order, human);
    if (w != order.end() && h != order.end() && w > h) std::rotate(h, w, w + 1);
}
} // namespace cua::hyprland
