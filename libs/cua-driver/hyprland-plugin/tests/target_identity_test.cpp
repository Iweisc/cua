#include "target_identity.hpp"

#include <cstdlib>

namespace {
void check(bool condition) { if (!condition) std::abort(); }
}

int main() {
    using cua::hyprland::production_target_fields;
    using cua::hyprland::target_identity_matches;
    check(!production_target_fields(3));
    check(production_target_fields(4));
    check(production_target_fields(5));
    check(!production_target_fields(6));

    // A TARGET without the field keeps binding by address alone.
    check(target_identity_matches(std::nullopt, 0x18000004));
    check(target_identity_matches(0x18000004, 0x18000004));
    // The window that took a closed window's address has the next stable id.
    check(!target_identity_matches(0x18000004, 0x18000005));
}
