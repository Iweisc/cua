#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace cua::hyprland {
// Returns the window a new top-level window reacts to, or an empty Window
// when it is not a reaction to agent input. `parents` is its parent chain,
// nearest first. The nearest parent that is the human's focused window or a
// window the agent is driving decides, so a dialog of the human's own window
// stays theirs even when the agent drives another window of the same app.
// Without such a parent, `app_target` is the agent's window in the new
// window's app when that app is the agent's (see agent_commanded_last), and
// empty otherwise. A reaction opens with its parent, or, having none, with
// that agent window, as a background app's window opens in that app's Space
// on macOS.
template <typename Window, typename Driven>
[[nodiscard]] Window reaction_anchor(const std::vector<Window>& parents, const Window& human, Driven driven, const Window& app_target) {
    for (const auto& parent : parents) {
        if (parent == human) return {};
        if (driven(parent)) return parents.front();
    }
    if (app_target == Window{}) return {};
    return parents.empty() ? app_target : parents.front();
}

// In an app the human also uses, a new window with no parent belongs to
// whoever last gave that app a command: the agent's latest input, or the
// human's latest click or modifier chord. Plain typing is no command, so the
// human typing on in one window does not claim a window the agent opened from
// another, while their own Ctrl+N does.
template <typename Time>
[[nodiscard]] constexpr bool agent_commanded_last(Time agent, std::optional<Time> human) {
    return !human || *human < agent;
}

// A window an app opens after closing the window the agent drove, as login and
// splash flows do right after a click. No driven window is left to compare it
// with, so the app it came from decides, as for a window with no parent.
struct Replacement {
    // It has no parent and comes from the app of the window the agent last drove.
    bool same_app = false;
    // That window has closed.
    bool driven_gone = false;
    // The agent's input there is within the reaction window.
    bool recent = false;
    // The human's focused window is in that app.
    bool human_in_app = false;
    // The agent gave that app a command after the human last did.
    bool agent_last = false;
};

[[nodiscard]] constexpr bool agent_replacement(const Replacement& r) {
    return r.same_app && r.driven_gone && r.recent && (!r.human_in_app || r.agent_last);
}

// A reaction window floats where a new tile would change the human's layout:
// on a workspace they can see, where it would split their tile, and where a
// window is fullscreen or maximized, which Hyprland ends for a new tile.
[[nodiscard]] constexpr bool reaction_floats(bool floating, bool workspace_visible, bool workspace_fullscreen) {
    return floating || workspace_visible || workspace_fullscreen;
}

// What a popup grab or drag start says about who caused it. Clients quote
// the seat and serial of the input they react to.
struct GrabSignals {
    // It quotes a serial an agent seat sent.
    bool agent_serial = false;
    // The human clicked its window, or gave it a modifier chord, after the
    // agent's latest input there.
    bool human_newer = false;
    // It quotes an agent seat.
    bool agent_seat = false;
    // Its window has the human's keyboard focus.
    bool human_window = false;
    // Its window is one the agent drives.
    bool agent_window = false;
    // Its app is one the agent drives and the human is not in.
    bool agent_app = false;
};

// An agent serial makes it the agent's, whoever else uses the app. Then the
// human's newer input in its window makes it theirs. The quoted seat comes
// after that: GTK3 quotes the seat its menu last grabbed with, so the human's
// own menu can quote an agent seat. Then its window decides, then its app.
[[nodiscard]] constexpr bool agent_grab(const GrabSignals& s) {
    if (s.agent_serial) return true;
    if (s.human_newer) return false;
    if (s.agent_seat) return true;
    if (s.human_window) return false;
    return s.agent_window || s.agent_app;
}

// The serials a lane put on its own input events, as ranges. Ranges stay
// short because the human's events take serials from the same counter.
class AgentSerials {
  public:
    // Notes serials first..last, inclusive.
    void note(std::uint32_t first, std::uint32_t last) {
        if (count_ && first == latest().second + 1) { latest().second = last; return; }
        next_ = (next_ + 1) % ranges_.size();
        ranges_[next_] = {first, last};
        count_ = std::min(count_ + 1, ranges_.size());
    }
    [[nodiscard]] bool contains(std::uint32_t serial) const {
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& [first, last] = ranges_[(next_ + ranges_.size() - i) % ranges_.size()];
            // Unsigned differences keep this right across the 32-bit wrap.
            if (serial - first <= last - first) return true;
        }
        return false;
    }

  private:
    std::pair<std::uint32_t, std::uint32_t>& latest() { return ranges_[next_]; }
    std::array<std::pair<std::uint32_t, std::uint32_t>, 32> ranges_{};
    std::size_t next_ = 0, count_ = 0;
};

// Moves `window` directly below `human` when it is above it. Every other
// window keeps its place.
template <typename Window>
void stack_below(std::vector<Window>& order, const Window& window, const Window& human) {
    const auto w = std::ranges::find(order, window);
    const auto h = std::ranges::find(order, human);
    if (w != order.end() && h != order.end() && w > h) std::rotate(h, w, w + 1);
}
} // namespace cua::hyprland
