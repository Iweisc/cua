#include "reaction_window.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
using cua::hyprland::opened_by_agent;
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
    const std::string human = "human", target = "target", other = "other";
    const auto driven = [&](const std::string& w) { return w == target; };

    check(!opened_by_agent(Windows{}, human, driven, false), "a window with no parent from an app the agent is not driving is the human's");
    check(opened_by_agent(Windows{}, human, driven, true), "a window with no parent from an app the agent drives and the human is not in is the agent's");
    check(opened_by_agent(Windows{target}, human, driven, false),
        "a dialog of the agent's window is the agent's even when the human is in the same app");
    check(!opened_by_agent(Windows{human}, human, driven, true), "a dialog of the human's own window is the human's");
    check(!opened_by_agent(Windows{target}, target, driven, true), "a dialog of the agent's window is the human's once the human is in it");
    check(opened_by_agent(Windows{other, target}, human, driven, false), "a dialog of a dialog of the agent's window is the agent's");
    check(!opened_by_agent(Windows{human, target}, human, driven, true),
        "a dialog of an agent dialog the human is now using is the human's");
    check(opened_by_agent(Windows{target, human}, human, driven, false), "the nearest parent decides");
    check(opened_by_agent(Windows{other}, human, driven, true) && !opened_by_agent(Windows{other}, human, driven, false),
        "an unrelated parent leaves the decision to the app");

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
