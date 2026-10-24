#pragma once

namespace cua::hyprland {
// What happens to a client's request to change its own fullscreen or
// maximized state.
enum class StateRequest {
    apply,          // As Hyprland always does.
    apply_in_place, // On the window's own workspace, never moved to an output.
    refuse,         // Hyprland leaves the window as it is.
};

// Hyprland applies such a request by replacing the workspace's fullscreen or
// maximized window, which ends the human's, drops their keyboard focus, and
// covers their window. So a request that reacts to agent input is refused on a
// workspace the human can see, and where another window is fullscreen or
// maximized. On a hidden workspace the app may change its own state, as a
// background app does in its own Space on macOS.
[[nodiscard]] constexpr StateRequest state_request(bool agent, bool visible, bool other_fullscreen) {
    if (!agent) return StateRequest::apply;
    if (visible || other_fullscreen) return StateRequest::refuse;
    return StateRequest::apply_in_place;
}

// A request reacts to agent input when a lane drives the window, or its app
// reacts to the agent as it does for a new window. The human's keyboard focus
// on the window makes it theirs, so their own F11, title-bar double-click, or
// fullscreen button always works.
template <typename Window>
[[nodiscard]] bool agent_request(const Window& window, const Window& human, bool driven, bool app_reaction) {
    return window != human && (driven || app_reaction);
}
} // namespace cua::hyprland
