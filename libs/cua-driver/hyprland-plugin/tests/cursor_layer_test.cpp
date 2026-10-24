#include "cursor_layer.hpp"

#include <cstdint>
#include <cstdlib>

namespace {
void check(bool condition) { if (!condition) std::abort(); }
}

int main() {
    using cua::hyprland::pinned_cursor_window;
    check(pinned_cursor_window("cua-agent-cursor@55d0c1a2b3f0") == std::uintptr_t{0x55d0c1a2b3f0});
    check(!pinned_cursor_window("cua-agent-cursor"));
    check(!pinned_cursor_window("cua-agent-cursor@"));
    check(!pinned_cursor_window("cua-agent-cursor@0"));
    check(!pinned_cursor_window("cua-agent-cursor@0x55d0"));
    check(!pinned_cursor_window("cua-agent-cursor@55d0 "));
    check(!pinned_cursor_window("cua-agent-cursor@ffffffffffffffffff"));
    check(!pinned_cursor_window("waybar@55d0"));
}
