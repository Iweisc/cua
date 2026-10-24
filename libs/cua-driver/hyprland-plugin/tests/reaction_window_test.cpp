#include "reaction_window.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
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

    check(reaction_floats(false, true), "a tiled reaction window floats where a window is fullscreen");
    check(!reaction_floats(false, false), "a tiled reaction window stays tiled elsewhere");
    check(reaction_floats(true, false), "a floating reaction window stays floating");

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
