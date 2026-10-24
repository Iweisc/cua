---
title: Background input to single-seat clients on Hyprland
authors:
  - Iweisc
created: 2026-10-19
last_updated: 2026-10-24
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
client's reaction to agent input from taking the person's focus, and removes
the app package gate from background input, as on macOS
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
- Background input reaches any app, with no package gate or app list, as on
  macOS.

## Non-goals

- Same-client input on this route. It borrows the person's own resources, so
  it refuses when the person is on the target client.
- XWayland clients.
- Text/IME. Keys use the same canonical US keycodes as the agent seats.
- Agent clicks inside popups. Pointer actions address only the bound
  top-level surface. Agent keys operate GTK menus.
- Changing the agent-seat route or `FOREGROUND_TARGET`.

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
  packages in `PACKAGES` (`hyprland_compatibility.rs`). Every other executable
  refuses with `client_not_qualified`.
- `TARGET` refuses with `primary_target_busy` while the person's pointer or
  keyboard focus is on the target client.
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

### Admission and shared apps

Driver drops the package gate, so background input reaches any app. macOS has
no app list either. The gate was a compatibility check, not authorization, and
an unlisted app sent agents to foreground input, which takes the person's
focus.

`TARGET` no longer refuses pointer actions when the person is on the target
client. The agent seat has its own pointer, so the agent can click, scroll, and
drag in one window of a native app while the person types in another. Passive
agent pointer focus also stays when the person enters that client. Keys refuse
with `primary_keyboard_busy` while the person's keyboard focus is on any surface
of the target client, checked before every key. Firefox and Qt keep one
keyboard focus per process, so an agent key in one window took the person's
typing in another, and agent keys in the person's own window mix with theirs
in every toolkit. Ghostty, foot, and GTK keep focus per seat but are refused
too, with no per-app exception. `PRIMARY_TARGET` and X windows still refuse
every action with `primary_target_busy`, because their input or the app's
reactions reach the person's own devices.

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
| Grant GTK popup grabs                           | The grab moves keyboard focus to the popup, which takes the person's typing. GTK menus still open without it and take agent keys.                     |
| Keep the package gate with a user list          | macOS has no app gate, the gate was not authorization, and every unlisted app fell back to foreground input.                                          |

## Compatibility and migration

All changes are additive. An older plugin does not advertise
`primary_background`, and Driver keeps its current behavior with it. An older
Driver ignores `agent_bound` and the new `HELLO` field. Driver's public result
contract does not change: the route stays `synthetic_events` for both
background bindings. Apps that refused with `client_not_qualified` now
receive background input. Rollback is unloading the new plugin build.

## Security, privacy, and telemetry

The route sends events only to the target client's own resources, and only
while the person is not on that client. It sends nothing to the person's
focused client. The reaction guards close the two known ways a background
client could take the person's focus or pointer through Hyprland's serial
handling. Removing the package gate does not change the trust model: it was a
compatibility check, and the plugin trusts same-user code, the same as RFC
3550. No new telemetry.

## Implementation plan

1. Plugin: `PRIMARY_TARGET`, the guard, and the status fields, with the route
   binding and guard-order unit tests (commit `75003182b`).
2. Protocol document (commit `a31854e4a`).
3. Driver: route selection and doctor probes.
4. Plugin and Driver: no package gate, and native agent-seat targets may share
   an app with the person.
5. Plugin: refuse agent keys while the person's keyboard focus is in the
   target's app.
6. This RFC.

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
- The release Driver with `CUA_DRIVER_RS_ENABLE_WAYLAND=1`, recorded while an
  earlier user list admitted Chromium: `get_window_state`, `click`, `type_text`, and
  `press_key` on background Chromium A appended ` ok` and `x` to its field
  (title `typed hello! okx okx` after two runs) while the active window and
  cursor stayed on B. Each result is
  `delivery.mode: background` and `effect: unverifiable`. `cua-driver doctor`
  reports
  `background input ready (protocol 3, primary_background true)`.
- Code - OSS (Electron 42) in the background, recorded the same way: Driver `type_text` put `agent ` into its editor
  (`agent start`, tab marked modified) while the human typed `asdxc` into
  Chromium B, and the active window and cursor stayed on B.
- Without a package gate or app list, in a nested Hyprland 0.56.2 under
  headless mutter: one native GTK process with two windows, the person typing
  `hund` into one while the agent clicked and typed `agent` into the other on
  a hidden workspace. Each window received only its own text.
- A GTK combo box and a GTK popup menu opened by agent clicks on a hidden
  workspace took agent Down and Return (`picked two`, `menu alpha`), with the
  popup grab withheld and the person's window still active.

Before acceptance, also record: one more Electron app, and two lanes
targeting different Chromium windows at once.

## Unresolved questions

- Should the reaction window be configurable, or is two seconds enough for
  slow apps?
- Should agent pointer actions reach popups of the bound window?

## Decision record

Pending review.
