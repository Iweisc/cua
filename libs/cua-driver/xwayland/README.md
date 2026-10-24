# Optional Xwayland build for clicks on covered X windows

Cua Driver reaches X windows on Hyprland with stock Xwayland. It gives each
agent seat its own XInput2 master pair, so agent input never moves your X
pointer or focus. One case needs the X server itself: X picks the window under
a pointer by X stacking, and Hyprland leaves windows on hidden workspaces
mapped. An agent click on an X window that another app's X window covers in X
stacking would reach the covering window, so Driver refuses it with
`xwayland_target_covered`.

`agent-focus.patch` changes one function in `hw/xwayland/xwayland-input.c`.
For a pointer whose Xwayland seat is attached to a master other than the core
pointer, which is how Driver attaches agent seats, Xwayland resolves the
pointer inside the window that seat entered through Wayland instead of by X
stacking. If the window layout is unexpected, the event goes to the root
window, never to another window. Your pointer stays on the core master and
never takes this path. Nothing else in Xwayland changes.

With this build, Driver allows pointer actions on covered X windows. It
recognizes the build by its vendor string.

## Build

Needs the build dependencies of the distribution's `xorg-xwayland` package,
plus `meson`, `ninja`, `patch`, and `curl`. The script refuses a system
Xwayland of a different release and installs only the `Xwayland` binary:

```sh
./build.sh "$HOME/.local/opt/cua-xwayland"
```

## Use it for Hyprland only

Hyprland starts Xwayland with `/bin/sh -c "exec Xwayland ..."`, so it uses the
first `Xwayland` on Hyprland's `PATH`. Put `$HOME/.local/opt/cua-xwayland/bin`
first in the environment Hyprland starts with, for example in the script or
session entry that launches it, then log out and back in. `/usr/bin/Xwayland`
is not touched.

Check it from a terminal in the new session:

```sh
xdpyinfo | grep 'vendor string'
```

The vendor string ends with `(cua-xwayland-agent-focus)`.

To go back, remove that `PATH` entry and log out and back in. Rebuild after a
system Xwayland update; the script stops if the releases differ.
