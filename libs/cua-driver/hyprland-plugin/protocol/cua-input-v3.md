# Local input protocol v3 candidate

This opt-in candidate connects Driver's existing per-action admission to two
independent compositor seats. The accepted extension adds a distinct exact-target
primary-seat foreground route. It is not native certification or a release
announcement. The default plugin build remains discovery-only, and discovery
protocol v2 is unchanged.

Driver completes common permission, resource, and lifecycle checks before
each action. The plugin trusts the desktop account
under a local trust model. Same-UID transport checks prevent accidental
cross-account use; they do not authenticate human consent or contain hostile
code running as that user. There is no external signer, `APPROVE` command,
challenge, or separate per-window permission workflow in v3.

## Transport and lane ownership

The private Hyprland instance directory contains `cua-input-v3.sock` and
`cua-input-v3-2.sock`. Each endpoint uses Linux `SOCK_SEQPACKET`, checks
`SO_PEERCRED` for the compositor UID, and owns one lane. Socket permissions are
`0600`; the instance directory must be owned by that user and private. Driver
also verifies the peer against the selected compositor instance.

Packets contain printable ASCII fields separated by exactly one space, with
no newline. The maximum packet size is 2048 bytes. Responses are JSON packets.
Unknown commands, extra fields, integer overflow, nonfinite coordinates, and
out-of-range values refuse. Decimal integers are unsigned; the target address,
window stable id, epoch, and target token are hexadecimal. Epochs and tokens contain 32 lowercase
hexadecimal characters.

Each socket accepts at most eight connections. A connection must send `HELLO`
within five seconds and must not remain idle for more than 60 seconds. A
successful `CLAIM` reserves the endpoint for that connection until EOF,
timeout, or a desktop/configuration lifecycle transition. Repeated `CLAIM` on
the same connection is idempotent. Driver may try the second endpoint only
after an explicit `lane_busy` reply, before target selection or dispatch.
Connection failures and unknown delivery results do not permit retries.

The v3 seats are `Cua-Agent` and `Cua-Agent-2`. Driver excludes these from its
foreground virtual-input routes. Seat ownership does not come from public
session labels. The signed experiment uses separate sockets, protocol 0, and
its original seat names.

## Requests and responses

The request sequence for every Driver-admitted action is:

1. Send `HELLO` once per connection. The response is
   `{"ok":true,"protocol":3,"epoch":"<epoch>","foreground_target":true,"primary_background":true,"xwayland_target":true,"stable_id":true,"popup_input":true}`
   for a production plugin implementing the foreground, primary-background,
   and XWayland extensions, the stable-id target field, and
   [pointer targets](#pointer-targets) on the window's popups. Driver must
   require each explicit capability before using its route or field.
2. Send `CLAIM`. The response is `{"ok":true,"lane":0}` or lane `1`.
3. For background input, send `TARGET <pid> <hex-address> <capability>`.
   For foreground input, send
   `FOREGROUND_TARGET <pid> <hex-address> <capability>`. The capability is exactly
   one of `1` (click), `2` (key), `4` (scroll), or `8` (drag); foreground selection
   also permits `16` (activate). Combined masks refuse.
   The response includes `ok`, `target`, `revision`, `width`, and `height`.
   A `TARGET` response also includes `agent_bound`, which is `false` when the
   target client holds no pointer or keyboard on this lane's agent seat. Such a
   client can only be reached through `PRIMARY_TARGET`, described below.
   `TARGET` refuses X windows with `stale_target`; they need `XWAYLAND_TARGET`.
   Every target request may end with one more field, the window's hex stable
   id (`stableId` in `hyprctl -j clients`), for example
   `TARGET 811 5d12f9b62540 2 18000004`. See
   [Window identity](#window-identity).
4. Send the one matching bounded operation using that token and revision.
   A subsequent action requires fresh route-specific target selection, even on
   a cached connection. The binding fixes the route; an operation cannot turn
   a background grant into a foreground grant.

`TARGET` binds the exact live native top-level surface, generates a fresh token,
and grants at most five seconds of steady-clock technical lifetime. It first
retires any unused previous grant. It refuses unavailable desktops and another
lane targeting the target's Wayland client. Human focus on that client does
not refuse pointer actions, because the agent seat has its own pointer. Keys
refuse while the human's keyboard focus is on that client, as described in
[Keys in the person's app](#keys-in-the-persons-app).
Background seats always publish a private canonical `evdev`/`pc105`/`us`
keymap, so user layout options and remaps do not alter agent key semantics.
They advertise the user seat's key repeat rate and delay, falling back to
Hyprland's 25 Hz and 600 ms defaults until a keyboard is active. A `KEY`
request presses and releases every key within one compositor dispatch, so no
agent key is held long enough to repeat. A discovered address is an input to
attestation, not a surface lifetime token.
Surface unmap, destruction, or replacement invalidates the binding. Geometry
changes increment the revision and refuse stale actions.

### Window identity

Hyprland gives a closed window's address to the next window it creates,
usually at once and across apps, X windows included. An address therefore
names whichever window holds it now, not the window Driver discovered. The
optional last field of `TARGET`, `PRIMARY_TARGET`, `FOREGROUND_TARGET`, and
`XWAYLAND_TARGET` is the stable id Driver read with that address, which
Hyprland never reuses within a compositor run. When the window at the address
carries another stable id, the request refuses with `stale_target` and binds
nothing. A malformed id refuses with `invalid_request`. Without the field,
target selection binds by address alone. Driver sends the field only when
`HELLO` reports `stable_id`, and checks the id itself before connecting; the
plugin check covers a window that takes the address between the two.

The complete operation requests are:

| Request | Limits |
| --- | --- |
| `ACTIVATE <sequence> <target> <revision>` | Capability 16; foreground bindings only. |
| `CLICK <sequence> <target> <revision> <x> <y> <button> <count>` | Evdev buttons 272-274; count 1-2. |
| `KEY <sequence> <target> <revision> <key> <modifiers>` | Evdev key 1-247 except lock keys 58, 69, and 70. Modifier bits: shift=1, ctrl=2, alt=4, super=8. |
| `SCROLL <sequence> <target> <revision> <x> <y> <axis> <value>` | Axis 0=vertical, 1=horizontal; nonzero value in [-1000,1000]. |
| `DRAG <sequence> <target> <revision> <x1> <y1> <x2> <y2> <duration_ms>` | Left button; duration 50-2000 ms; the grant must cover the duration plus 50 ms. |

Coordinates are finite logical values relative to the window's top-left
corner. A point outside `0 <= x < width` and `0 <= y < height`, the end of a
drag included, refuses with `point_outside_window`, except a `CLICK` or
`SCROLL` point on one of the window's popups; see
[Pointer targets](#pointer-targets). Sequence numbers increase
strictly across the connection, including fresh target selections. Repeated
or lower sequences return `replay`; no operation is replayed automatically.

### Pointer targets

A background pointer action goes to the window's topmost mapped `xdg_popup`
whose input region holds its point, else to the window's surface or the
subsurface of it that takes input there, in that order, as with Hyprland's own
pointer. The plugin sends the point in that surface's coordinates. Popups are
not in the window's surface tree, so testing that tree alone would press the
widget under an open menu. A popup may extend past the window, and a `CLICK` or `SCROLL` there is
not outside it. Driver sends such a point only when `HELLO` reports
`popup_input`, and only on `TARGET` and `PRIMARY_TARGET` bindings. A `CLICK`
or `SCROLL` on a popup that the human's popup grab holds refuses with
`primary_grab`: that menu is the human's.

A `DRAG` starts only on the window's own surface. It refuses with
`popup_target` on a popup and with `subsurface_target` on a subsurface. Its
motion stays on that surface while it runs, popups drawn over it included, and
it ends when the point crosses a subsurface. `FOREGROUND_TARGET` focuses the
window's own surface for every pointer action, so it refuses a point on a
popup or subsurface with `primary_target_busy` and detail
`foreground_pointer_target` before activation. X windows have no
`xdg_popup`; their menus are X windows, which XWayland hit-tests itself.

The plugin repeats target, geometry, desktop, keymap, and conflict checks on
the compositor thread at
dispatch. It consumes the grant before the first synthetic focus/input event.
Completed background operations release their held buttons, keys, keyboard focus, and
remaining technical authority. A drag keeps its bounded lifetime while
running; consuming its grant does not permit another action. A dispatch without
fresh authority returns `action_not_admitted`. Malformed or refused requests
never imply application rollback.

Background pointer focus has a separate lifetime. After a successful background pointer operation,
the lane keeps passive pointer focus on the exact live surface it entered: the
window's own, one of its subsurfaces, or one of its popups. This internal
state is separate from the visible Driver overlay, whose idle fade and
session-end removal remain unchanged. Fresh
`TARGET` admission on the same surface and unchanged geometry preserves this
focus while issuing a new token and one-operation grant. A key action releases
its keyboard focus without removing a previously retained pointer focus.
Retaining pointer focus prevents an unnecessary `wl_pointer.leave` directly
after drag release; some clients coalesce motion and requeue the release.
There is no fixed completion delay or trace-only timing behavior.

Passive focus has no held input or authority, but still participates in
inter-lane conflicts. Owner cancellation, EOF, and the
connection's idle timeout release held buttons and all keyboard state and
revoke authority without requiring pointer leave. Passive target references
are weak and independent of the transport owner; a disconnected owner retains
no lane reservation. Fresh admission may reuse unchanged focus on the same
live surface without inheriting old authority.

Because Driver claims a free lane before selecting its target, fresh valid
`TARGET` admission may retire matching hover on a different unreserved lane,
or a lane whose new claimant has never successfully bound a target. A bare
`CLAIM` reserves capacity; it does not adopt a previous owner's hover. This is
allowed only when that peer has no lease, drag, held input, keyboard focus,
capabilities, or remaining grant/expiry state. After a connection's first
successful `TARGET`, its reservation protects its hover even after STOP,
CANCEL, or target invalidation. Existing target owners and active peers still
cause `agent_target_busy`; ordinary dispatch and conflict checks never evict
them. This permits opposite-order reuse when both new connections reserve
their lanes before either selects a target, without retrying any input.

Target replacement, unmap, destruction, and geometry change clear passive
focus, and so does unmapping the popup or subsurface that holds it. Desktop, keymap, and configuration transitions revoke authority and
release held input, but keep passive focus on an unchanged live target, as
after normal completion.
An observed physical keymap transition cancels existing authority, but fresh
background admission after that transition uses the unchanged private agent
keymap.
The five-second action grant does not extend to passive focus. These boundaries
may still end focus before a stalled client processes its events; dispatch
acknowledgement is not an application-processing fence. Verify the application's
effect independently.

Successful background dispatch returns
`{"ok":true,"effect":"unverifiable","route":"synthetic_events"}`. This
acknowledges synthetic delivery, not an application outcome. A drag first
returns `{"ok":true,"phase":"started"}` and later its final result. After
that first acknowledgement, cancellation can leave partial application
effects. EOF or a missing final acknowledgement means delivery is unknown;
Driver must not report that nothing happened or replay the operation.

Driver maps a refusal before an acknowledged start to `effect:refused`, with
no delivery field. After a drag-start acknowledgement, an explicit cancellation
maps to `effect:partial` with `delivery.mode:background` and
`delivery.delivered_count:1`. This count measures acknowledged gesture phases
(the start), not pointer events or application changes. A lost or malformed
final reply preserves that count but sets `delivery.mode:unknown`; later input
may have landed without acknowledgement. A missing initial reply has unknown
delivery with no count. Neither case permits replay.

Driver sends text and multi-packet scrolls as a sequence of `KEY` or `SCROLL`
actions, one key per character. A refusal or a lost connection after some of
them were acknowledged maps to `effect:partial` with that many in
`delivery.delivered_count`, on either route.

Refusals use `{"ok":false,"code":"<code>","detail":"<code>"}`. Codes
include `lane_busy`, `lane_not_claimed`, `stale_target`, `stale_geometry`,
`session_unavailable`, `unsupported_layout`, `primary_target_busy`,
`primary_keyboard_busy`, `agent_target_busy`, `lease_expired`,
`action_not_admitted`, `lease_busy`, `client_not_bound`, `replay`,
`point_outside_window`, `popup_target`, `subsurface_target`, `primary_grab`,
`unsupported`, and `invalid_request`.

### Keys in the person's app

On a native `TARGET` binding, capability 2 and every `KEY` refuse with
`primary_keyboard_busy` while the human's keyboard focus is on any surface of
the target's Wayland client, the target itself included. Click, scroll, and
drag are unaffected. The check runs on the compositor thread before the agent
seat's `wl_keyboard.enter`, and a `KEY` enters, types, and leaves within one
dispatch, so no agent key reaches the client after the human's keyboard focus
does. Driver sends each key of a text as its own `TARGET` and `KEY`, so a text
stops at the first key after the human enters the app. Driver reports the
keys already acknowledged as a partial result with `delivery.delivered_count`.

Each `KEY` sends a keyboard enter on the agent seat. Firefox and Qt keep one
keyboard focus for the whole process, so that enter takes focus from the
human's window in the same app. Firefox then routes the human's typing into
the agent's window until the human clicks their own, and Qt deactivates the
human's window and drops its shortcuts. In every toolkit, agent keys sent to
the window the human is typing in mix with the human's own keys. Ghostty, foot,
GTK3, and GTK4 keep keyboard focus per seat and could take agent keys in one
window while the human types in another, but they are refused too. This is a
deliberate limit with no per-app or per-toolkit exception: a client's keyboard
model is not visible to the compositor.

## Primary-seat background extension

Chromium/Ozone and Electron bind only the first `wl_seat`, so they never see
the agent seats and `TARGET` reports `agent_bound:false`. `PRIMARY_TARGET
<pid> <hex-address> <capability>` binds the same exact live surface to
`primary_background`. Click, key, scroll, and drag are delivered to the target
client's own primary-seat pointer and keyboard resources without changing the
compositor's pointer focus, keyboard focus, or cursor position. `ACTIVATE`
refuses with `unsupported`. Driver chooses this route explicitly after
`agent_bound:false`; the plugin never changes route on its own. A connection
may alternate `TARGET` and `PRIMARY_TARGET`, but never either with
`FOREGROUND_TARGET`.

Admission and every dispatch refuse with these codes:

- `primary_binding`: the client does not hold exactly one primary seat binding.
- `primary_target_busy`: human pointer or keyboard focus is on that client.
- `agent_target_busy`: another lane targets that client.
- `primary_grab`, `primary_dnd`, `primary_constraint`: a seat grab, layout
  drag, drag-and-drop, or pointer constraint is active.
- `subsurface_target`: a drag starts on a subsurface, or no surface of the
  window takes input at the point. Clicks and scrolls reach popups and
  subsurfaces as described in [Pointer targets](#pointer-targets).
- `unsupported_layout`: a key chord means something different under the human's
  keymap than on a neutral US keyboard. Keys are sent with neutral modifiers, so
  the human's Num Lock does not refuse.

The borrowed resources are left and released after each action, except when
human focus has since reached that client: Hyprland has already moved them to
the human's surface. Human focus reaching the target cancels the lease
synchronously, as on the agent-seat route. An agent button press clears the
seat's pointer-button serial at once, so the client cannot use it to start an
interactive move or resize that would follow the human's pointer.

Successful dispatch returns
`{"ok":true,"effect":"unverifiable","route":"primary_background"}`.

## Reactions to background input

Hyprland accepts `xdg_popup.grab` and `wl_data_device.start_drag` without
checking which seat the serial came from. A client reacting to agent input on
either background route could therefore take the human's keyboard focus or tie
a drop to the human's next button release. The plugin withholds the popup grab,
leaving the popup mapped, and cancels the drag source when the requesting
client is under an agent lease or drag, or received background dispatch in the
last two seconds, and the human's focus is not on that client. GTK still maps
such a menu and handles agent keys in it, and agent clicks and scrolls reach
it as described in [Pointer targets](#pointer-targets).
`hyprctl -j cua:status` reports `input.primary_background`,
`input.refused_popup_grabs`, and `input.refused_drag_starts`.

Hyprland also opens every new top-level window on the human's workspace,
gives it keyboard focus, raises a new floating window over the others and over
a fullscreen window, ends a fullscreen or maximized window when a new tiled
window opens on its workspace, and makes a new tiled window the visible tab of
the human's focused group. A new window is a reaction to agent input when the
nearest window in its parent chain that is either the human's focused window
or a window a lane leases or delivered to in the last two seconds is the
lane's. With neither in its parent chain, it is a reaction when it comes from
the process of such a lane window and the human's pointer and keyboard focus
are not in that process. The rule compares processes, not clients, because
Electron shows its GTK dialogs over a second Wayland connection. So a dialog
of the agent's window is a reaction even while the human uses another window
of the same app, and a dialog of the human's own window never is. A reaction
window opens on the workspace of its parent, or with no parent, of the lane
window in its app, as Hyprland's `workspace N silent` rule would place it.
That workspace may be hidden or special. The plugin then sends `movewindow`
and `movewindowv2`, because Hyprland has already sent `openwindow` naming the
human's workspace. The window opens without initial focus, stays out of the
human's group, and floats when its workspace has a fullscreen or maximized
window. On the human's workspace it stacks directly under the human's window,
also when that window is fullscreen, but Hyprland draws floating windows over
tiled ones. `input.withheld_initial_focus` counts these windows.

## Agent cursor stacking

Driver draws its agent cursors in full-output `zwlr_layer_shell_v1` overlay
layers. When the plugin's input sockets exist, cursors pinned to a window go
in a layer per window and output named `cua-agent-cursor@<hex window address>`,
followed by `.<hex stable id>` when Driver knows which window it returned that
address for. While that window is mapped, the plugin leaves the layer out of Hyprland's
layer pass and draws it right after the window's main pass, at the window's
workspace offset. Windows stacked above the target and the target's own
popups cover the cursor, and a hidden workspace hides it. Single-window
captures never include it. Hyprland snapshots a window only to animate its
close, and that snapshot includes the cursor, so it fades out with the window.
A pinned layer is never drawn above every window. The plain
`cua-agent-cursor` layer stays above every window.

Hyprland reuses a freed window's address for the next window it creates, so
the plugin binds each pinned layer to the window at its address when the
layer maps, and draws it only after that window. A layer whose name carries a
stable id binds only to a window with that id. A layer that maps after its
target closed, even once another window took the address, binds to no window,
and the plugin sends it `zwlr_layer_surface_v1.closed` at once, so Driver's
next pin opens a layer named for the window it then has at that address.
Driver places the cursor
relative to the window's top-left corner from the box it read when it acted,
and does not see the window move, so the plugin also records that box. When
the window is destroyed, even on a hidden workspace, or its corner moves or
it shrinks (a move, layout reflow, swap, or fullscreen, float, or group
toggle), the plugin sends the layer `zwlr_layer_surface_v1.closed`. Driver
then stops drawing the cursors pinned to that address and opens no layer for
them until it pins them again. A cursor therefore never moves onto a window
that took its target's address, and never stays at a point its moved target
left. The cursor fades with the window's fade alpha, including a special
workspace fading in or out, but not with its opacity rules.

## XWayland extension

XWayland binds every Wayland seat, the agent seats included, and attaches each
seat's X input devices to the human's core pointer and keyboard. Agent input to
an X window would then move the human's X pointer and type into the human's X
focus. `XWAYLAND_TARGET <pid> <hex-address> <capability>` binds an X window
through this lane's agent seat. Driver sends it only after moving the seat's
XWayland devices onto their own XInput2 master pair. The plugin cannot see X
device state and relies on Driver for that step, which is why plain `TARGET`
keeps refusing X windows.

The response adds `"route":"xwayland"`, `seat`, and `xid`. `seat` is the agent
seat's Wayland global name, which XWayland uses in its device names
(`xwayland-pointer:<seat>`). `xid` is the X window. `client_not_bound` refuses
when XWayland has not bound this lane's seat. Actions acknowledge with
`"route":"synthetic_events"`.

These rules differ from native targets:

- Human focus on the target's X process refuses with `primary_target_busy`.
  An X app has one client pointer for all its windows, so its reactions to
  agent input would reach the human's keyboard. Conflicts compare X processes,
  because XWayland is one Wayland client for every X app, so human focus on
  another X app does not refuse.
- Pointer actions enter the target at the action point and send no motion.
  XWayland turns motion into XI2 raw motion, which every raw listener
  receives, including a relative-mouse game the human is playing. Drags still
  move while the button is held.
- Scroll sends an empty axis event before the real one. XI2 clients take the
  first scroll after an enter as their baseline.
- Pointer actions refuse with `xwayland_focus_changed` when the human's keyboard
  focus changed after `XWAYLAND_TARGET`. Hyprland restacks X windows on focus,
  and X stacking decides which window a pointer event reaches. Driver checks
  that the target is uncovered at the action point after admission, unless the
  server carries the optional patch in
  [`../../xwayland`](../../xwayland/README.md), which keeps agent pointers
  inside the window they entered.
- Drags refuse with `primary_constraint` while the human's pointer is locked or
  confined. Every XI2 raw listener sees agent pointer motion, and a game
  holding such a constraint usually reads it. Clicks and scrolls send no
  motion and proceed.
- Pointer coordinates are scaled by the window's X surface scale, as Hyprland
  does for the human's pointer under `xwayland:force_zero_scaling`.
- X apps show menus and dialogs as new windows. A menu or dialog that reacts
  to agent input opens as [Reactions to background input](#reactions-to-background-input)
  describes, without initial focus.

## Exact-target foreground extension

`FOREGROUND_TARGET` binds an ordinary live native top-level surface to
`primary_foreground`. The plugin validates the exact PID, compositor address,
surface lifetime, geometry revision, and route at admission and dispatch on the
compositor thread. `ACTIVATE` intentionally activates that target. Foreground
click, key, scroll, and drag use the same bounded operation shapes with a
foreground binding; primary focus and cursor position may change. This route
makes no promise to restore focus or cursor position after delivery.

Before taking over primary input, the plugin must refuse held physical keys or
buttons, active grabs, pointer constraints, and drag-and-drop. It requires a
single primary seat binding, excluding its own agent seats by resource identity,
and refuses binding or input-resource changes during dispatch. Foreground
keyboard delivery never changes the human keyboard's modifiers or locks, so the
focused client sees no modifier change around the action. It requires layout
group zero and no held or latched modifiers. Num Lock may stay on; Caps Lock and
any other lock refuse. The physical keymap qualifies when every key the KEY
command can press (evdev 1-247 except the lock keys) produces the same keysym as
canonical `evdev`/`pc105`/`us`, at the base level and, for non-modifier keys,
with Shift held. Options that only change Caps Lock or modifier chords, such as
`compose:caps` and `shift:both_capslock_cancel`, qualify; a different layout or
a remapped typing key refuses before activation with `unsupported_layout`.
Before consuming the grant or changing focus, the plugin also simulates the
whole KEY chord under the live keymap and lock state and admits it only when it
means what the wire chord means on a neutral canonical US keyboard: the same
symbols, consumed modifiers, and resulting modifier state. These refusals use
`primary_target_busy` before activation, with these details:

- `foreground_keyboard_caps_lock`: Caps Lock is on.
- `foreground_keyboard_numlock_keypad`: Num Lock is on and changes what a
  keypad key in the chord means, such as `KP_End` becoming `KP_1`. Text,
  digits, navigation keys, and keypad operators are unaffected.
- `foreground_unsupported_layout`: the chord, such as a remapped Ctrl, Alt, or
  Super shortcut, means something else under the live keymap.
- `foreground_keyboard_locked`, `foreground_keyboard_depressed`,
  `foreground_keyboard_latched`, `foreground_keyboard_group`: another lock
  modifier, a held or latched modifier, or a nonzero layout group.

A modifier, lock, or group change during the action refuses the rest of it
with `foreground_partial_unknown` and detail `foreground_keyboard_state`; the
unwind restores the human keyboard's current state. Foreground pointer-only
actions do not depend on the physical keyboard layout. Foreground drag
cancellation on primary-input and focus transitions remains subject to review
and native verification; do not infer background isolation from this route.
Foreground results use foreground delivery metadata. A drag cancellation after
its start is partial, and a missing acknowledgement is unknown, under the same
no-replay rules as background delivery.

Discovery or activation read-back followed by global `wtype` input does not
establish exact-target delivery and is not a fallback. Background refusal never
escalates to foreground. Both routes use Driver's existing permission and
lifecycle admission, without a separate approval UI.

## Cancellation and recovery

`CANCEL` and `STOP` both require the connection's lane claim. They revoke only
that endpoint's pending/current action, invalidate its target, and release its
synthetic held state. They preserve the reservation and other lane. A drag
receives its cancellation result before the command acknowledgement. A new
unclaimed control connection cannot cancel a runtime's lane. V3 has no global
socket stop command; plugin disable or unload explicitly stops both lanes.

These commands belong to the private plugin wire, not the public MCP tool
surface. Likewise, EOF below means the owned plugin connection closes. Closing
MCP stdin is not necessarily immediate plugin EOF: direct stdio finishes its
current request before reading the next request or EOF. See the
[Driver lifecycle boundary](host-authority-boundary.md#cancellation-and-recovery)
for the separate cancellation paths and transport limits.

EOF cancels only the departing connection's work and frees its reservation.
Lock/unlock, DPMS, session activity, and monitor transitions revoke authority.
Keymap/layout changes revoke authority, including synchronous layout and
active-keyboard keymap notifications. For `PRIMARY_TARGET` and X bindings,
changing primary focus to a target client cancels that lane synchronously. A
native `TARGET` grant stays, and its next `KEY` refuses with
`primary_keyboard_busy`. Dispatch and timer checks supplement these listeners.
None of these paths wake the display or unlock the session.

Config disable closes input transports but preserves client-owned seats and
resources. Re-enable opens fresh transports with new epochs. Each later
action must pass fresh Driver admission and target checks. Plugin replacement
requires a desktop restart. The shared seat-lifetime marker rejects loading
replacement modules in the same compositor instance. Unload retires globals
and retains inert callbacks for late client cleanup.

Cancellation releases synthetic state; it does not undo application effects.
Cleanup depends on a responsive compositor event loop. A stall is not evidence
of bounded cleanup latency.

## Compatibility and build gates

Background `TARGET`, `PRIMARY_TARGET`, and `XWAYLAND_TARGET` have no app or
package gate, as on macOS. Recorded native app evidence covers Calc and
Inkscape. Foreground `FOREGROUND_TARGET` covers ordinary native top-level
surfaces. Native GTK3, Electron, and Tauri
foreground coverage is planned and still requires certification. The plugin checks
native surface identity, geometry, client conflicts, and exact compiled XKB
content against the default `evdev`/`pc105`/`us` keymap. Variants, options,
remaps, multiple groups, missing keyboards, Unicode, IME input,
arbitrary held-key streams, and modified pointer gestures are outside this
candidate. Layout names alone never establish eligibility. Existing semantic
Driver routes retain their own behavior.

Driver expands bounded ASCII text into complete key operations under the exact
US keymap. The protocol adds no text or IME stream.

Build against the exact Hyprland 0.56.2 ABI with `CUA_HYPRLAND_INPUT=ON`.
This option defaults to `OFF` and is mutually exclusive with
`CUA_HYPRLAND_TEST_INPUT`. The production build does not link an external
signer or OpenSSL. `CUA_HYPRLAND_INPUT_TRACE=ON` adds test instrumentation for
certification; it defaults to `OFF` and requires v3 input. Uninstrumented
production does not expose `TRACE_START`, `TRACE_STOP`, or `TRACE_READ`.

Portable grant, lifecycle, discovery, and transport tests do not prove native
seat delivery or keymap qualification. Certification must build the actual
native implementation and verify each supported app/operation, both lanes,
independent primary interaction, stale/refusal paths, partial delivery,
cleanup, and surviving-client recovery at the exact candidate SHA. Include
both instrumented evidence and an uninstrumented production-package smoke.
The foreground extension must also pass the existing complete canonical Linux
Rust suite in native Hyprland at the candidate SHA, preserving its runner,
required cells, assertions, and evidence checks. No foreground certification
result is recorded here.

The independent-seat design adapts Dillon DuPont's Hyprland prototype. Earlier
signed-experiment results remain historical evidence for that source only.
