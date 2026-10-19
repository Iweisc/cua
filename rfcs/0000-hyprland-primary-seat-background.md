---
title: Background input to single-seat clients on Hyprland
authors:
  - Iweisc
created: 2026-10-19
last_updated: 2026-10-19
status: draft
discussion: TBD (rename this file to <issue>-hyprland-primary-seat-background.md)
rfc_pr:
implementation:
  - https://github.com/Iweisc/cua/tree/feat/hyprland-primary-background
supersedes:
superseded_by:
---

# RFC: Background input to single-seat clients on Hyprland

## Summary

Extend the Hyprland input v3 plugin from
[RFC 3550](3550-hyprland-isolated-input.md) with a second background binding,
`PRIMARY_TARGET`, for clients that bind only the first `wl_seat` and never see
the agent seats. The plugin delivers click, key, scroll, and drag to that
client's own primary-seat `wl_pointer` and `wl_keyboard` resources. Compositor
pointer focus, keyboard focus, and the cursor stay where they were. The route
refuses whenever the person is on that client or holds a grab, drag, or pointer
constraint. Driver chooses it only after the plugin reports `agent_bound:false`,
and requires the plugin to acknowledge every action on that route as
`primary_background`, so an agent-seat acknowledgement never stands in for it.
This RFC also adds two guards that stop a background
client's reaction to agent input from taking the person's focus, and a
user-maintained admission list for apps outside the qualified package matrix
([#4779](https://github.com/trycua/cua/issues/4779)).

## Motivation

Chromium/Ozone and every Electron app bind only the first `wl_seat` they see.
On Hyprland that is the person's seat, so the agent seats from RFC 3550 never
have pointer or keyboard resources in those clients. `TARGET` succeeds, and
every action then refuses with `client_not_bound`. In practice, background
input does not work for browsers, VS Code, Slack, Discord, Obsidian, and most
other apps people want agents to use.

RFC 3550 lists "direct `wl_pointer`/keyboard resource delivery" as a retained
alternative that must not be silently substituted under the agent-seat claim.
It rejects "borrow primary-seat focus and restore it" as the isolation
contract. This RFC proposes the first, as a separately named route with its own
refusal codes. It does not propose the second.

## Goals

- Agent click, key, scroll, and drag reach a background Chromium/Electron
  window without changing compositor focus, cursor position, or stacking.
- The person's own input keeps reaching their focused window during and
  between agent actions.
- Every refusal has an exact code, and Driver never retries on another route
  after a dispatch has an unknown outcome.
- A client reacting to agent input cannot take keyboard focus through
  `xdg_popup.grab`, or tie a drop to the person's next button release through
  `wl_data_device.start_drag`.
- Users can admit an app themselves, and result summaries say the user
  qualified it.

## Non-goals

- Same-client multi-user input. The route refuses when the person is on the
  target client.
- XWayland clients.
- Text/IME. Keys use the same canonical US keycodes as the agent seats.
- Showing GTK menus in the background. GTK requires the popup grab this RFC
  withholds.
- Changing the agent-seat route or `FOREGROUND_TARGET`.
- Admission as a security boundary against same-user code. Like the package
  list, the user list is a compatibility gate.

## Terminology

| Term                   | Meaning in this proposal                                                                                   |
| ---------------------- | ---------------------------------------------------------------------------------------------------------- |
| Single-seat client     | A client that binds exactly one `wl_seat`, the primary seat. Chromium/Ozone and Electron do this.          |
| Primary-seat resources | The `wl_pointer` and `wl_keyboard` objects a client created from the primary seat.                         |
| Borrowed resources     | Primary-seat resources that receive `enter`, input, and `leave` for one agent action outside compositor focus. |
| Reaction window        | The two seconds after background dispatch to a client, during which its popup grabs and drag starts are guarded. |

## Current state

- The plugin's `TARGET` reply carries `agent_bound`. It is `false` when the
  target client holds no agent-seat pointer or keyboard
  ([`input_experiment.cpp`](../libs/cua-driver/hyprland-plugin/src/input_experiment.cpp)).
- Agent-seat dispatch to such a client refuses with `client_not_bound`.
- Background admission accepts only the native LibreOffice Calc and Inkscape
  packages in `PACKAGES`
  ([`hyprland_compatibility.rs`](../libs/cua-driver/rust/crates/platform-linux/src/wayland/hyprland_compatibility.rs)).
  Every other executable refuses with `client_not_qualified`.
- Hyprland 0.56.2 accepts `xdg_popup.grab` and `wl_data_device.start_drag`
  serials without checking which seat issued them.

## Proposal

### Plugin binding

`PRIMARY_TARGET <pid> <hex-address> <capability>` binds the same exact live
surface as `TARGET`, with route `primary_background`. `HELLO` advertises
`"primary_background":true`. A connection may alternate `TARGET` and
`PRIMARY_TARGET`, but never either with `FOREGROUND_TARGET`. `ACTIVATE`
refuses with `unsupported`.

For each action the plugin sends `enter` to the client's primary-seat pointer
and keyboard resources at the target point, then the input events, then
`leave`. It does not call the seat's focus setters, so Hyprland's own pointer
and keyboard focus, the cursor, and the person's held keys are untouched. The
`enter`, `leave`, and button serials come from the compositor's serial counter
like any other event.

Admission and every dispatch refuse, in this order, with:

| Code                  | Condition                                                                 |
| --------------------- | ------------------------------------------------------------------------- |
| `primary_binding`     | The client does not hold exactly one primary seat binding.                |
| `primary_target_busy` | Human pointer or keyboard focus is on that client.                        |
| `agent_target_busy`   | Another lane targets that client.                                         |
| `primary_grab`        | A seat grab or layout drag is active.                                     |
| `primary_dnd`         | A drag-and-drop is active.                                                |
| `primary_constraint`  | A pointer constraint is active.                                           |
| `subsurface_target`   | The point lands on a subsurface.                                          |
| `unsupported_layout`  | A key chord means something different under the person's keymap than on a neutral US keyboard. |

When human focus reaches the target client during a lease, the plugin cancels
the lease synchronously, as on the agent-seat route. An agent button press
clears the seat's pointer-button serial at once, so the client cannot use it to
start an interactive move or resize that would follow the person's pointer.

Successful dispatch returns
`{"ok":true,"effect":"unverifiable","route":"primary_background"}`.

### Reaction guards

The plugin hooks `CXDGShellProtocol::addOrStartGrab` and
`CWLDataDeviceProtocol::initiateDrag`.
For a client under an agent lease or drag, or in its reaction window, where the
person's focus is not on that client:

- a popup grab is withheld and the popup stays mapped;
- a drag source is cancelled.

These guards apply to both background routes. `cua:status` reports
`input.refused_popup_grabs` and `input.refused_drag_starts`.

### Driver route selection

Driver keeps a single `Background` delivery route. For each action it sends
`TARGET`. If the plugin advertised `primary_background` and the reply has
`agent_bound:false`, Driver sends `PRIMARY_TARGET` once for the same action and
requires the binding and the action acknowledgement to carry
`route:"primary_background"`. A plugin that omits `agent_bound` or does not
advertise the extension keeps the existing `client_not_bound` refusal. An
acknowledgement with the wrong route is an unknown outcome, never a retry.

### User admission

`$XDG_CONFIG_HOME/cua-driver/hyprland-apps.txt` lists one absolute executable
path per line. Blank lines and `#` comments are ignored. Entries are
canonicalized, so a symlink admits its real binary. Package qualification is
checked first. The summary of a successful action admitted by the list says
the user qualified the app in `hyprland-apps.txt`.

### Doctor

On Hyprland, `cua-driver doctor` reports whether the Wayland backend is
enabled, whether the plugin is loaded with a matching ABI and a ready
transport, and whether it advertises `primary_background`. The AT-SPI probe
now reaches the registry on the address `org.a11y.Bus` hands out, because the
bus name alone answers while that address is a dead socket.

## Alternatives considered

| Alternative                                     | Disposition                                                                                                                                         |
| ----------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| Borrow primary-seat focus and restore it        | Rejected by RFC 3550 and here. It moves the person's focus and cannot prove no grab or key state changed in between.                                  |
| Make Chromium bind the agent seats              | Needs changes in Chromium's Ozone backend and every Electron build. Not in Cua's control.                                                              |
| `FOREGROUND_TARGET` for single-seat clients     | Works today, but takes the person's focus. Background is the user-facing goal.                                                                        |
| Fall back to `PRIMARY_TARGET` inside the plugin | Hides the route change from Driver and from results. Driver chooses explicitly so the result names the route.                                         |
| Show GTK menus by granting the grab             | The grab moves keyboard focus to the popup, which takes the person's typing. Withholding it is the safe default.                                      |
| User admission by app ID or window class        | Window classes are client-chosen and shared by unrelated processes. An executable path matches what the package gate already checks.                  |

## Compatibility and migration

All changes are additive. An older plugin does not advertise
`primary_background`, and Driver keeps its current behavior with it. An older
Driver ignores `agent_bound` and the new `HELLO` field. Driver's public result
contract does not change: the route stays `synthetic_events` for both
background bindings, and only the summary text names a user-qualified app. Rollback is unloading the new plugin build, or removing the
user list.

## Security, privacy, and telemetry

The route sends events only to the target client's own resources, and only
while the person is not on that client. It sends nothing to the person's
focused client. The reaction guards close the two known ways a background
client could take the person's focus or pointer through Hyprland's serial
handling. Admission remains a compatibility gate and does not protect against
same-user code, the same as RFC 3550. No new telemetry.

## Implementation plan

1. Plugin: `PRIMARY_TARGET`, the guard, and the status fields, with the route
   binding and guard-order unit tests (commit `75003182b`).
2. Protocol document (commit `a31854e4a`).
3. Driver: route selection, user admission, result text, doctor probes.
4. This RFC.

## Test and acceptance plan

Recorded on Hyprland 0.56.2 (`efb50993`) with real DRM output (vkms) in a
disposable container, plus a nested instance:

- Plugin CTest 20/20, Driver Hyprland unit tests 128/128.
- Chromium A in the background, Chromium B focused by a virtual human
  pointer/keyboard. `TARGET` on A returns `agent_bound:false` and click refuses
  with `client_not_bound`. `PRIMARY_TARGET` click on A's button sets its title
  to `clicked 1`. Typing gives `typed hello!`. Active window and cursor are
  unchanged before and after. Human typing before and after the agent action
  lands in B (`typed asdas`).
- `PRIMARY_TARGET` on the human-focused window refuses with
  `primary_target_busy`. The human moving onto A between bind and dispatch
  refuses with `stale_target`.
- A GTK3 menu button clicked through the agent seat: popup grab withheld
  (`refused_popup_grabs: 1`), focus stays on B.
- A Chromium HTML5 drag started by agent input: drag source cancelled
  (`refused_drag_starts: 1`, page title `dragend`), and human typing still
  lands in B.
- The release Driver with `CUA_DRIVER_RS_ENABLE_WAYLAND=1` and Chromium listed
  in `hyprland-apps.txt`: `get_window_state`, `click`, `type_text`, and
  `press_key` on background Chromium A appended ` ok` and `x` to its field
  (title `typed hello! okx okx` after two runs) while the active window and
  cursor stayed on B. Each result is
  `delivery.mode: background`, `effect: unverifiable`, and its summary names
  the user list. `cua-driver doctor` reports
  `background input ready (protocol 3, primary_background true)`.
- Code - OSS (Electron 42) in the background with Electron listed in
  `hyprland-apps.txt`: Driver `type_text` put `agent ` into its editor
  (`agent start`, tab marked modified) while the human typed `asdxc` into
  Chromium B, and the active window and cursor stayed on B.

Before acceptance, also record: one more Electron app, and two lanes
targeting different Chromium windows at once.

## Unresolved questions

- Should the reaction window be configurable, or is two seconds enough for
  slow apps?
- Should Driver report the GTK no-menu case as a refusal instead of an
  unverifiable success?
- Should `hyprland-apps.txt` support per-operation entries (click only, no
  drag) to match the package matrix?

## Decision record

Pending review.
