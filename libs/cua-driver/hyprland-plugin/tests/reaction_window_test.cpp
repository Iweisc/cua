#include "reaction_window.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {
using cua::hyprland::agent_commanded_last;
using cua::hyprland::agent_grab;
using cua::hyprland::AgentSerials;
using cua::hyprland::GrabSignals;
using cua::hyprland::reaction_anchor;
using cua::hyprland::reaction_floats;
using cua::hyprland::stack_below;
using Windows = std::vector<std::string>;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}

int main() {
    const std::string human = "human", target = "target", other = "other", dialog = "dialog", none;
    const auto driven = [&](const std::string& w) { return w == target; };

    check(reaction_anchor(Windows{}, human, driven, none).empty(), "a window with no parent from an app the agent is not driving is the human's");
    check(reaction_anchor(Windows{}, human, driven, target) == target,
        "a window with no parent from an app the agent drives and the human is not in opens with the agent's window");
    check(reaction_anchor(Windows{target}, human, driven, none) == target,
        "a dialog of the agent's window is the agent's even when the human is in the same app, and opens with it");
    check(reaction_anchor(Windows{human}, human, driven, target).empty(), "a dialog of the human's own window is the human's");
    check(reaction_anchor(Windows{target}, target, driven, target).empty(), "a dialog of the agent's window is the human's once the human is in it");
    check(reaction_anchor(Windows{dialog, target}, human, driven, none) == dialog,
        "a dialog of an agent dialog is the agent's and opens with its own parent");
    check(reaction_anchor(Windows{human, target}, human, driven, target).empty(), "a dialog of an agent dialog the human is now using is the human's");
    check(reaction_anchor(Windows{target, human}, human, driven, none) == target, "the nearest parent decides");
    check(reaction_anchor(Windows{other}, human, driven, target) == other, "with an unrelated parent the app decides, and the window opens with its parent");
    check(reaction_anchor(Windows{other}, human, driven, none).empty(), "an unrelated parent in an app the agent is not driving is the human's");

    check(agent_commanded_last(5, std::optional<int>{}), "with no human command the new window is the agent's");
    check(agent_commanded_last(5, std::optional<int>{4}), "a human command before the agent's leaves the new window the agent's");
    check(!agent_commanded_last(5, std::optional<int>{6}), "a human command after the agent's makes the new window theirs");

    check(reaction_floats(false, false, true), "a tiled reaction window floats where a window is fullscreen");
    check(reaction_floats(false, true, false), "a tiled reaction window floats on a workspace the human can see");
    check(!reaction_floats(false, false, false), "a tiled reaction window stays tiled on a hidden workspace");
    check(reaction_floats(true, false, false), "a floating reaction window stays floating");

    check(agent_grab({.agent_seat = true, .human_window = true}), "a grab on an agent seat is the agent's, even in the human's window");
    check(agent_grab({.agent_serial = true, .human_window = true}), "a grab quoting an agent serial is the agent's, even in the human's window");
    check(!agent_grab({.human_window = true, .agent_window = true, .agent_app = true}), "otherwise a grab in the human's window is theirs");
    check(agent_grab({.agent_window = true}), "otherwise a grab in a window the agent drives is the agent's");
    check(agent_grab({.agent_app = true}), "otherwise a grab in an app the agent drives and the human is not in is the agent's");
    check(!agent_grab({}), "a grab with no agent signal is the human's");

    AgentSerials serials;
    check(!serials.contains(7), "no serial is the agent's before it sends any");
    serials.note(7, 7);
    serials.note(8, 8);
    serials.note(12, 12);
    check(serials.contains(7) && serials.contains(8) && serials.contains(12), "every serial the agent sent is the agent's");
    check(!serials.contains(9) && !serials.contains(11) && !serials.contains(13), "serials between and after them are not");
    for (std::uint32_t s = 100; s < 200; s += 2) serials.note(s, s);
    check(!serials.contains(7) && serials.contains(198) && !serials.contains(197), "only the latest ranges are kept");
    AgentSerials burst;
    for (std::uint32_t s = 1000; s < 1040; ++s) burst.note(s, s);
    check(burst.contains(1000) && burst.contains(1039), "a burst of consecutive serials stays whole");
    AgentSerials wrapped;
    wrapped.note(0xfffffffe, 0xffffffff);
    wrapped.note(0, 1);
    check(wrapped.contains(0xffffffff) && wrapped.contains(0) && wrapped.contains(1) && !wrapped.contains(2), "ranges survive the 32-bit wrap");

    Windows order{"a", human, "b", "new"};
    stack_below(order, std::string("new"), human);
    check((order == Windows{"a", "new", human, "b"}), "a new window lands directly under the human's and nothing else moves");
    stack_below(order, std::string("new"), human);
    check((order == Windows{"a", "new", human, "b"}), "a window already under the human's stays");
    stack_below(order, human, human);
    check((order == Windows{"a", "new", human, "b"}), "the human's own window stays");
    stack_below(order, std::string("new"), std::string("gone"));
    check((order == Windows{"a", "new", human, "b"}), "nothing moves when the human has no window");
    return 0;
}
