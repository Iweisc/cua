#pragma once

#include <algorithm>
#include <vector>

namespace cua::hyprland {
// Decides whether a new top-level window is a reaction to agent input.
// `parents` is its parent chain, nearest first. The nearest parent that is
// the human's focused window or a window the agent is driving decides, so a
// dialog from the human's own window stays theirs even when the agent drives
// another window of the same app. Without such a parent, `agent_app` says
// whether the window's app is one the agent drives and the human is not in.
template <typename Window, typename Driven>
[[nodiscard]] bool opened_by_agent(const std::vector<Window>& parents, const Window& human, Driven driven, bool agent_app) {
    for (const auto& parent : parents) {
        if (parent == human) return false;
        if (driven(parent)) return true;
    }
    return agent_app;
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
