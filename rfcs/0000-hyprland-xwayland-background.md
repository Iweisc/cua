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
XWayland patch is needed.

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

- Clicking an X window covered by another app's X window at that point.
- Relative-mouse input for games the agent drives.
- Text beyond the existing US-keymap key stream.

## Proposal

### Plugin

- `XWAYLAND_TARGET <pid> <hex-address> <capability>` binds an X window through
  the lane's agent seat. Plain `TARGET` keeps refusing X windows, so an older
  Driver never reaches them. The reply adds `seat`, the agent seat's Wayland
  global name that XWayland uses in its device names, and `xid`.
- Human and agent conflicts compare X processes, because XWayland is one
  Wayland client for every X app.
- Pointer actions enter at the action point and send no motion, since XWayland
  turns motion into XI2 raw motion that every raw listener receives. Scroll
  sends an empty axis event first so XI2 clients have a baseline. Coordinates
  use the window's X surface scale.
- Pointer actions refuse with `xwayland_focus_changed` if the person's keyboard
  focus changed after admission, because Hyprland restacks X windows on focus.
  They refuse with `primary_constraint` while the person's pointer is locked
  or confined.
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
  app was handed back.
- Pointer actions check with `TranslateCoordinates` that the root child at the
  action point belongs to the target's X client, and refuse with
  `xwayland_target_covered` otherwise.
- The target app's client pointer moves to the agent pair with
  `XISetClientPointer`, so the app's core grabs, focus requests, and pointer
  queries land on agent devices. Driver hands it back two seconds after the
  last action once the agent pointer holds no grab, at once when the person
  activates that app, and after 60 seconds regardless.
- X windows need the user's `hyprland-apps.txt` entry; the package matrix
  covers native windows only.

## Alternatives considered

| Alternative | Disposition |
| --- | --- |
| Patch XWayland | Rejected. A custom X server, changes in shared dix event paths, and a crash takes every X app. The one capability it adds, clicks on covered windows, does not justify that. |
| XTest or XSendEvent | Rejected. XTest drives the person's core devices. Many toolkits drop `send_event` input. |
| Restack the target through Hyprland's XWM | Rejected. It changes the person's X stacking, so their own clicks would reach a hidden window. |
| Same-client conflicts for all X apps | Rejected. The person could not use any X app while an agent works in another. |
| Probe the person's pointer for grabs | Rejected. A probe grabs the person's device. |

## Compatibility and migration

All additive. An older plugin does not advertise `xwayland_target`, and Driver
then refuses X windows with `xwayland_unavailable`. An older Driver never sends
`XWAYLAND_TARGET`. Agent master pairs persist in the X server and are reused;
XWayland restarts with Hyprland.

## Security, privacy, and telemetry

The plugin cannot see X device state and relies on Driver to isolate the seat
before `XWAYLAND_TARGET`, consistent with RFC 3550's same-user trust model.
Admission is still a compatibility gate, not a sandbox against same-user code.
No new telemetry.

## Known limits

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
  produce no game input; pointer actions refuse with `primary_constraint`.
- Scale 2, with and without `xwayland:force_zero_scaling`: clicks and drags
  land at the screenshot point.
- Plugin CTest 20/20; Driver Hyprland and XWayland unit tests pass.

Before acceptance, also record a physical desktop with a real mouse, two
agents on different X apps at once, and a Proton game.

## Unresolved questions

- Should an optional XWayland change restrict the agent sprite to its Wayland
  focus window, so covered windows become clickable?
- Should the new-window guard also cover native apps and move reaction windows
  to their parent's workspace?

## Decision record

Pending review.
