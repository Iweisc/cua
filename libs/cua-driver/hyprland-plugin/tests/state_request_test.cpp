#include "state_request.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
using cua::hyprland::agent_request;
using cua::hyprland::state_request;
using cua::hyprland::StateRequest;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}

int main() {
    const std::string human = "human", target = "target", none;

    check(!agent_request(target, human, false, false), "a request from a window the agent never drove is the app's own");
    check(agent_request(target, human, true, false), "a request from the window the agent drives is the agent's");
    check(agent_request(target, human, false, true), "a request from another window of an app the agent just drove is the agent's");
    check(agent_request(target, none, true, false), "a request is the agent's while the human has no focused window");
    check(!agent_request(target, target, true, true), "a request from the window the human is in is theirs, even right after the agent drove it");

    check(state_request(false, true, true) == StateRequest::apply, "the human's or the app's own request applies as usual, even over another fullscreen window");
    check(state_request(true, true, false) == StateRequest::refuse, "an agent request on a workspace the human can see is refused");
    check(state_request(true, false, true) == StateRequest::refuse, "an agent request never ends another window's fullscreen, even on a hidden workspace");
    check(state_request(true, false, false) == StateRequest::apply_in_place, "an agent request on a hidden workspace applies there");
    std::cout << "state request tests passed\n";
    return 0;
}
