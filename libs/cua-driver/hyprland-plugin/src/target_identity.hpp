#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace cua::hyprland {
// A production TARGET is `<command> <pid> <address> <capability>`, optionally
// followed by the window's stable id. Hyprland gives a closed window's address
// to the next window, usually at once, so Driver names the window it saw and a
// different window at that address refuses.
[[nodiscard]] constexpr bool production_target_fields(std::size_t count) {
    return count == 4 || count == 5;
}

// `expected` is empty when the TARGET named no stable id.
[[nodiscard]] constexpr bool target_identity_matches(std::optional<std::uint64_t> expected,
                                                     std::uint64_t stable_id) {
    return !expected || *expected == stable_id;
}
} // namespace cua::hyprland
