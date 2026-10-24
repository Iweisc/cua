---
title: Background input to XWayland windows on Hyprland
authors:
  - Iweisc
created: 2026-10-24
last_updated: 2026-10-24
status: draft
discussion: TBD (rename this file to <issue>-hyprland-xwayland-background.md)
rfc_pr:
implementation:
  - https://github.com/Iweisc/cua/tree/feat/hyprland-primary-background
supersedes:
superseded_by:
---

# RFC: Background input to XWayland windows on Hyprland

## Summary

Let the Hyprland background route from [RFC 3550](3550-hyprland-isolated-input.md)
reach X windows on stock XWayland. The plugin admits an X window only through a
new `XWAYLAND_TARGET` request. Before each such action, Driver gives the lane's
agent seat its own XInput2 master pointer and keyboard inside the X server,
using only standard X requests. Agent input then never moves the person's X
pointer, X focus, or held keys. Pointer actions refuse where another app's X
window covers the target, because X routes pointer input by X stacking. No
XWayland patch is needed. An optional one-function XWayland patch removes that
refusal for people who want it.

## Motivation

Many apps people want agents to drive only run on X11: Wine and Proton, Java,
Steam, SDL games, older Electron, and Motif or Xt tools. Most have no usable
accessibility tree, so screenshot coordinates are the only route. The plugin
refuses X windows today, because stock XWayland attaches every Wayland seat's
devices, the agent seats included, to the person's core pointer and keyboard.
Agent keys would go to the person's X focus, and agent clicks would move the
person's X pointer.

An earlier prototype patched XWayland: it changed dix event delivery, device
queries, focus, grabs, and sprite tracing, and needed a custom X server binary.
Its failure scope was every X app and, through the shared event path, every X
client. This RFC replaces it.

## Goals

- Click, key, scroll, and drag reach a background X window, including one on a
  hidden workspace, while the person keeps using native and X apps.
- The person's X pointer, X focus, held keys, and keymap are never changed by
  agent input or by an app's reaction to it.
- Stock XWayland and only standard X protocol.
- Exact refusal codes where isolation cannot be shown.

## Non-goals

- Relative-mouse input for games the agent drives.
- Text beyond the existing US-keymap key stream.

## Proposal

### Plugin

- `XWAYLAND_TARGET <pid> <hex-address> <capability>` binds an X window through
  the lane's agent seat. Plain `TARGET` keeps refusing X windows, so an older
  Driver never reaches them. The reply adds `seat`, the agent seat's Wayland
  global name that XWayland uses in its device names, and `xid`.
- Human focus on the target's X process refuses with `primary_target_busy`.
  An X app has one client pointer for all its windows, so its reactions to
  agent input would reach the person's keyboard. Conflicts compare X
  processes, because XWayland is one Wayland client for every X app.
- Pointer actions enter at the action point and send no motion, since XWayland
  turns motion into XI2 raw motion that every raw listener receives. Scroll
  sends an empty axis event first so XI2 clients have a baseline. Coordinates
  use the window's X surface scale.
- Pointer actions refuse with `xwayland_focus_changed` if the person's keyboard
  focus changed after admission, because Hyprland restacks X windows on focus.
  Drags refuse with `primary_constraint` while the person's pointer is locked
  or confined. Clicks and scrolls send no motion and proceed.
- A new X window from an X process the agent just drove opens without initial
  focus unless the person is in that process.

### Driver

- One master pair per agent seat, named `cua-xwayland-<seat>`, created with
  `XIChangeHierarchy` and reused for the X server's lifetime. The seat's
  `xwayland-pointer`, `xwayland-relative-pointer`, `xwayland-pointer-gestures`,
  and `xwayland-keyboard` devices are attached to it and verified before every
  action. The first action after a change waits 200 ms for clients to rebuild
  their device lists.
- Key actions set the master keyboard's focus with `XISetFocus`. Apps using
  ICCCM `WM_TAKE_FOCUS` (Wine, Java) get that offer when the focus moved or the
  app was handed back, unless X focus is already in that app.
- Pointer actions check with `TranslateCoordinates` that the root child at the
  action point is the target window or a menu its app mapped over it, and
  refuse with `xwayland_target_covered` otherwise. Another window of the same
  app counts as covering, since the person may be using it.
- The target app's client pointer moves to the agent pair with
  `XISetClientPointer`, so the app's core grabs, focus requests, and pointer
  queries land on agent devices. Driver hands it back two seconds after the
  last action once the agent pointer holds no grab, at once when the person
  activates that app, and after 60 seconds regardless.

### Optional XWayland build

`libs/cua-driver/xwayland/agent-focus.patch` changes only `xwl_xy_to_window`.
For a pointer whose XWayland seat is attached to a master other than the core
pointer, XWayland resolves the pointer inside the toplevel that seat entered
through Wayland, using X's own `miSpriteTrace` from that toplevel. If the
toplevel is not realized or not a root child, the event goes to the root
window. The core pointer never takes the new branch. `build.sh` builds the
system's exact release with the distribution's configuration, installs only
the binary into a private prefix, and sets the server vendor string to
`The X.Org Foundation (cua-xwayland-agent-focus)`. Hyprland finds Xwayland
through its own `PATH`, so the build applies to Hyprland alone. Driver skips
the coverage refusal only when it sees that vendor string.

## Alternatives considered

| Alternative | Disposition |
| --- | --- |
| Patch XWayland broadly | Rejected. The earlier prototype changed shared dix event paths, device queries, focus, and grabs. Only covered-window clicks need the server, so the optional patch changes one XWayland function. |
| XTest or XSendEvent | Rejected. XTest drives the person's core devices. Many toolkits drop `send_event` input. |
| Restack the target through Hyprland's XWM | Rejected. It changes the person's X stacking, so their own clicks would reach a hidden window. |
| Same-client conflicts for all X apps | Rejected. The person could not use any X app while an agent works in another. |
| Allow the agent in the X app the person is using | Rejected. In the recorded run the app moved its X focus in reaction to the agent, and the person's next keys went into the agent's window. |
| Probe the person's pointer for grabs | Rejected. A probe grabs the person's device. |

## Compatibility and migration

All additive. An older plugin does not advertise `xwayland_target`, and Driver
then refuses X windows with `xwayland_unavailable`. An older Driver never sends
`XWAYLAND_TARGET`. Agent master pairs persist in the X server and are reused;
XWayland restarts with Hyprland.

## Security, privacy, and telemetry

The plugin cannot see X device state and relies on Driver to isolate the seat
before `XWAYLAND_TARGET`, consistent with RFC 3550's same-user trust model.
The plugin is not a sandbox against same-user code.
No new telemetry.

## Known limits

- Without the optional build, pointer actions refuse on covered X windows.
- An agent cannot use an X app while the person is in it.
- The pointer-constraint guard covers drags and recognizes games that lock or
  confine the person's pointer. A game reading raw input without either still
  sees agent drags.
- Qt and Chromium grab every X master pointer while one of their menus is open,
  so a menu the agent opens there captures the person's pointer over X windows
  until it closes.
- Reaction windows still appear on the person's current workspace.
- With `misc:focus_on_activate` enabled, an app requesting activation in
  reaction to agent input can take focus.
- Agent key events are visible to XI2 raw key listeners.

## Test and acceptance plan

Recorded in a disposable container with Hyprland 0.56.2 (`efb50993`) and stock
Xwayland 24.1.13, nested under headless mutter, through the release Driver:

- SDL with no accessibility tree on a hidden workspace: click at the exact
  point, keys, scroll of three notches, and drag, with the person's Hyprland
  focus, cursor, X pointer, and X focus unchanged. The first action after the
  master pair is created lands.
- The person types into xterm while the agent clicks and types into the SDL
  window; each receives only its own input.
- A covered point refuses with `xwayland_target_covered`; keys still arrive.
- GTK3, Qt6, Chromium, Java Swing, and Wine Notepad on X11: typing, buttons,
  and combo boxes or menus. While an agent-opened menu is open, the person's
  typing still reaches their own X app. The client pointer returns to the core
  pointer afterwards.
- Wine's Edit menu and Java's combo popup open without taking focus
  (`withheld_initial_focus`).
- A person playing a relative-mouse SDL game: agent clicks, scroll, and typing
  produce no game input; drags refuse with `primary_constraint`.
- One X GTK process with two windows, the person typing in one: the agent
  refuses with `primary_target_busy` in the other and the person's text stays
  intact. With that refusal removed, the person's next keys went into the
  agent's window.
- Scale 2, with and without `xwayland:force_zero_scaling`: clicks and drags
  land at the screenshot point.
- With the optional build: agent clicks reach an SDL window covered by the
  person's xterm and by a second hidden window, which receive nothing, while
  the person's own clicks on that spot still reach the xterm. Stock XWayland
  with the same Driver still refuses those clicks. The app set above and the
  game check repeat with the same results.
- Plugin CTest 20/20; Driver Hyprland and XWayland unit tests pass.

Before acceptance, also record a physical desktop with a real mouse, two
agents on different X apps at once, and a Proton game.

## Unresolved questions

- Should the agent-focus change be proposed to XWayland upstream, so the
  optional build is no longer needed?
- Should the new-window guard also cover native apps and move reaction windows
  to their parent's workspace?

## Decision record

Pending review.
