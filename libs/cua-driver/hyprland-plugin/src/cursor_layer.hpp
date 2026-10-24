#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace cua::hyprland {
// Driver names an agent-cursor layer after the window its cursor is pinned
// above, as `cua-agent-cursor@<hex window address>`. Its plain
// `cua-agent-cursor` layer is not pinned and stays above every window.
[[nodiscard]] inline std::optional<std::uintptr_t> pinned_cursor_window(std::string_view name) {
    constexpr std::string_view prefix = "cua-agent-cursor@";
    if (!name.starts_with(prefix)) return std::nullopt;
    name.remove_prefix(prefix.size());
    std::uintptr_t address = 0;
    const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), address, 16);
    if (error != std::errc{} || end != name.data() + name.size() || address == 0) return std::nullopt;
    return address;
}
} // namespace cua::hyprland
