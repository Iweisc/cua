//! Agent input to XWayland windows on Hyprland, with stock XWayland.
//!
//! XWayland attaches the devices of every Wayland seat it binds, the plugin's
//! agent seats included, to the human's core pointer and keyboard. Agent input
//! would then move the human's X pointer and type into the human's X focus.
//! Before each action this gives the agent seat its own XInput2 master pair,
//! focuses that keyboard on the target, and points the target app's client
//! pointer at it, so the app's own grabs and focus requests land on agent
//! devices too. Apps that wait for the window manager to offer focus (ICCCM
//! WM_TAKE_FOCUS: Wine, Java) get that offer as well. Only standard X requests
//! are used; XWayland is not patched.
//!
//! X stacking decides which window a pointer event reaches, and Hyprland
//! leaves windows on hidden workspaces mapped. Pointer actions therefore
//! refuse when another app's X window covers the target at that point, unless
//! the server carries `libs/cua-driver/xwayland/agent-focus.patch`, which keeps
//! agent pointers inside the window the agent entered.

use std::sync::Mutex;
use std::time::{Duration, Instant};

use x11rb::connection::{Connection, RequestConnection};
use x11rb::errors::{ConnectionError, ReplyError};
use x11rb::protocol::xinput::{self, ConnectionExt as _, DeviceType};
use x11rb::protocol::xproto::{self, ConnectionExt as _};
use x11rb::rust_connection::RustConnection;

pub(super) const UNAVAILABLE: &str = "xwayland_unavailable";
pub(super) const SEAT_UNAVAILABLE: &str = "xwayland_seat_unavailable";
pub(super) const COVERED: &str = "xwayland_target_covered";
pub(super) const ISOLATION_FAILED: &str = "xwayland_isolation_failed";
pub(super) const TEXT_KEY_UNMAPPED: &str = "xwayland_text_key_unmapped";
/// Set by `libs/cua-driver/xwayland/build.sh` as the server vendor string.
const AGENT_FOCUS_VENDOR: &[u8] = b"The X.Org Foundation (cua-xwayland-agent-focus)";

pub(super) fn detail(code: &str) -> &'static str {
    match code {
        UNAVAILABLE => "Driver cannot reach the XWayland display with XInput 2.2",
        SEAT_UNAVAILABLE => "XWayland has no devices for this agent seat",
        COVERED => {
            "another app's X window covers this point, and X routes pointer input by \
             X stacking (Hyprland keeps windows on hidden workspaces mapped); keyboard \
             actions still reach this window"
        }
        TEXT_KEY_UNMAPPED => {
            "the X keyboard map has no key for this character without AltGr, so it was not typed"
        }
        _ => "the agent seat could not be moved onto its own X master pair",
    }
}

/// An app usually grabs or refocuses in reaction to input after the plugin has
/// acknowledged it, so its client pointer stays on the agent pair this long.
const HOLD: Duration = Duration::from_secs(2);
/// A menu the agent left open holds an agent grab. Hand the app back anyway.
const MAX_HOLD: Duration = Duration::from_secs(60);
const POLL: Duration = Duration::from_millis(100);
/// Clients rebuild their device lists on XI_HierarchyChanged. SDL dropped a
/// button sent a few milliseconds after the change, so the first action after
/// a change waits this long.
const SETTLE: Duration = Duration::from_millis(200);

#[derive(Debug, PartialEq)]
pub(super) enum Need {
    Keyboard,
    /// Window-local logical point, the coordinates the plugin receives.
    Pointer {
        x: f64,
        y: f64,
    },
}

pub(super) struct Prepared {
    pointer: u16,
}

/// An X keycode for a character of typed text, and whether it needs Shift.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(super) struct TextKey {
    keycode: u8,
    shift: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct Device {
    id: u16,
    name: String,
    kind: DeviceType,
    attachment: u16,
}

struct Pending {
    window: u32,
    pointer: u16,
    core: u16,
    last: Instant,
}

struct X {
    conn: RustConnection,
    root: u32,
    resource_mask: u32,
    active_window: u32,
    wm_protocols: u32,
    wm_take_focus: u32,
    agent_focus: bool,
}

struct State {
    x: Option<X>,
    pending: Vec<Pending>,
    in_flight: Vec<u16>,
    poller: bool,
}

static STATE: Mutex<State> = Mutex::new(State {
    x: None,
    pending: Vec::new(),
    in_flight: Vec::new(),
    poller: false,
});

struct Fail {
    code: &'static str,
    lost: bool,
}

impl From<ConnectionError> for Fail {
    fn from(_: ConnectionError) -> Self {
        Self {
            code: UNAVAILABLE,
            lost: true,
        }
    }
}

impl From<ReplyError> for Fail {
    fn from(error: ReplyError) -> Self {
        match error {
            ReplyError::ConnectionError(_) => Self {
                code: UNAVAILABLE,
                lost: true,
            },
            ReplyError::X11Error(_) => refuse(ISOLATION_FAILED),
        }
    }
}

fn refuse(code: &'static str) -> Fail {
    Fail { code, lost: false }
}

fn master_name(seat: u32) -> String {
    format!("cua-xwayland-{seat}")
}

/// A master pointer `isolate` added for some seat, never another client's.
fn agent_pointer(device: &Device) -> bool {
    device.kind == DeviceType::MASTER_POINTER
        && device
            .name
            .strip_prefix("cua-xwayland-")
            .and_then(|rest| rest.strip_suffix(" pointer"))
            .and_then(|seat| seat.parse().ok())
            .is_some_and(|seat| device.name == format!("{} pointer", master_name(seat)))
}

fn named<'a>(devices: &'a [Device], name: &str, kinds: &[DeviceType]) -> Option<&'a Device> {
    devices
        .iter()
        .find(|device| device.name == name && kinds.contains(&device.kind))
}

/// The seat's own master pair, once it exists and is paired.
fn master_pair(devices: &[Device], seat: u32) -> Option<(u16, u16)> {
    let name = master_name(seat);
    let pointer = named(
        devices,
        &format!("{name} pointer"),
        &[DeviceType::MASTER_POINTER],
    )?;
    let keyboard = named(
        devices,
        &format!("{name} keyboard"),
        &[DeviceType::MASTER_KEYBOARD],
    )?;
    (pointer.attachment == keyboard.id && keyboard.attachment == pointer.id)
        .then_some((pointer.id, keyboard.id))
}

/// Slave moves that put every XWayland device of `seat` on its own pair.
/// XWayland sends a seat's pointer input through its relative-pointer device
/// whenever that exists, so every one present must move, not only the two
/// that are required.
fn attachments(
    devices: &[Device],
    seat: u32,
    (pointer, keyboard): (u16, u16),
) -> Result<Vec<(u16, u16)>, &'static str> {
    let slaves = [
        DeviceType::SLAVE_POINTER,
        DeviceType::SLAVE_KEYBOARD,
        DeviceType::FLOATING_SLAVE,
    ];
    let mut moves = Vec::new();
    for (kind, master, required) in [
        ("pointer", pointer, true),
        ("relative-pointer", pointer, false),
        ("pointer-gestures", pointer, false),
        ("keyboard", keyboard, true),
    ] {
        match named(devices, &format!("xwayland-{kind}:{seat}"), &slaves) {
            Some(device)
                if device.kind == DeviceType::FLOATING_SLAVE || device.attachment != master =>
            {
                moves.push((device.id, master));
            }
            Some(_) => {}
            None if required => return Err(SEAT_UNAVAILABLE),
            None => {}
        }
    }
    Ok(moves)
}

fn core_pointer(devices: &[Device]) -> Option<u16> {
    named(
        devices,
        "Virtual core pointer",
        &[DeviceType::MASTER_POINTER],
    )
    .map(|device| device.id)
}

/// X resource ids carry their owning client in the bits above the mask.
fn same_client(a: u32, b: u32, resource_mask: u32) -> bool {
    a != 0 && b != 0 && a & !resource_mask == b & !resource_mask
}

/// Root-relative X pixel for a window-local logical point. XWayland may scale
/// windows, so the ratio between X and logical size is applied.
fn root_point(
    origin: (i16, i16),
    x_size: (u16, u16),
    logical_size: (f64, f64),
    (x, y): (f64, f64),
) -> Option<(i16, i16)> {
    let scale_x = f64::from(x_size.0) / logical_size.0;
    let scale_y = f64::from(x_size.1) / logical_size.1;
    let local = ((x * scale_x).floor(), (y * scale_y).floor());
    let inside = local.0 >= 0.0
        && local.1 >= 0.0
        && local.0 < f64::from(x_size.0)
        && local.1 < f64::from(x_size.1);
    inside.then(|| {
        (
            origin.0.saturating_add(local.0 as i16),
            origin.1.saturating_add(local.1 as i16),
        )
    })
}

impl X {
    fn connect() -> Result<Self, Fail> {
        let (conn, screen) = x11rb::connect(None).map_err(|_| refuse(UNAVAILABLE))?;
        if conn
            .extension_information(xinput::X11_EXTENSION_NAME)?
            .is_none()
        {
            return Err(refuse(UNAVAILABLE));
        }
        let version = conn.xinput_xi_query_version(2, 2)?.reply()?;
        if (version.major_version, version.minor_version) < (2, 2) {
            return Err(refuse(UNAVAILABLE));
        }
        let root = conn.setup().roots[screen].root;
        let resource_mask = conn.setup().resource_id_mask;
        let agent_focus = conn.setup().vendor == AGENT_FOCUS_VENDOR;
        let atom =
            |name: &[u8]| -> Result<u32, Fail> { Ok(conn.intern_atom(false, name)?.reply()?.atom) };
        let active_window = atom(b"_NET_ACTIVE_WINDOW")?;
        let wm_protocols = atom(b"WM_PROTOCOLS")?;
        let wm_take_focus = atom(b"WM_TAKE_FOCUS")?;
        Ok(Self {
            conn,
            root,
            resource_mask,
            active_window,
            wm_protocols,
            wm_take_focus,
            agent_focus,
        })
    }

    fn devices(&self) -> Result<Vec<Device>, Fail> {
        Ok(self
            .conn
            .xinput_xi_query_device(xinput::Device::ALL)?
            .reply()?
            .infos
            .into_iter()
            .map(|info| Device {
                id: info.deviceid,
                name: String::from_utf8_lossy(&info.name).into_owned(),
                kind: info.type_,
                attachment: info.attachment,
            })
            .collect())
    }

    fn change(&self, change: xinput::HierarchyChangeData) -> Result<(), Fail> {
        let len = match &change {
            xinput::HierarchyChangeData::AddMaster(add) => (8 + add.name.len()).div_ceil(4),
            _ => 2,
        };
        let change = xinput::HierarchyChange {
            len: len as u16,
            data: change,
        };
        self.conn
            .xinput_xi_change_hierarchy(std::slice::from_ref(&change))?
            .check()?;
        Ok(())
    }

    /// Masters persist in the X server after Driver exits, so a later run
    /// reuses the pair instead of churning the device hierarchy.
    fn isolate(&self, seat: u32) -> Result<(u16, u16, u16), Fail> {
        let mut devices = self.devices()?;
        let mut changed = false;
        if master_pair(&devices, seat).is_none() {
            changed = true;
            self.change(xinput::HierarchyChangeData::AddMaster(
                xinput::HierarchyChangeDataAddMaster {
                    send_core: true,
                    enable: true,
                    name: master_name(seat).into_bytes(),
                },
            ))?;
            devices = self.devices()?;
        }
        let pair = master_pair(&devices, seat).ok_or(refuse(ISOLATION_FAILED))?;
        let moves = attachments(&devices, seat, pair).map_err(refuse)?;
        if !moves.is_empty() {
            changed = true;
            for (deviceid, master) in moves {
                self.change(xinput::HierarchyChangeData::AttachSlave(
                    xinput::HierarchyChangeDataAttachSlave { deviceid, master },
                ))?;
            }
            devices = self.devices()?;
            if !attachments(&devices, seat, pair)
                .map_err(refuse)?
                .is_empty()
            {
                return Err(refuse(ISOLATION_FAILED));
            }
        }
        let core = core_pointer(&devices).ok_or(refuse(ISOLATION_FAILED))?;
        if changed {
            std::thread::sleep(SETTLE);
        }
        Ok((pair.0, pair.1, core))
    }

    fn within(&self, mut window: u32, ancestor: u32) -> Result<bool, Fail> {
        for _ in 0..64 {
            if window == ancestor {
                return Ok(true);
            }
            // 0 is None and 1 is PointerRoot.
            if window <= 1 || window == self.root {
                return Ok(false);
            }
            window = self.conn.query_tree(window)?.reply()?.parent;
        }
        Ok(false)
    }

    /// True when this call moved the agent keyboard's focus.
    fn focus(&self, keyboard: u16, window: u32) -> Result<bool, Fail> {
        // An app may have moved this focus to its own child (Java's focus proxy).
        let focus = self.conn.xinput_xi_get_focus(keyboard)?.reply()?.focus;
        if self.within(focus, window)? {
            return Ok(false);
        }
        self.conn
            .xinput_xi_set_focus(window, x11rb::CURRENT_TIME, keyboard)?
            .check()?;
        let focus = self.conn.xinput_xi_get_focus(keyboard)?.reply()?.focus;
        if focus == window {
            Ok(true)
        } else {
            Err(refuse(ISOLATION_FAILED))
        }
    }

    fn takes_focus(&self, window: u32) -> Result<bool, Fail> {
        let protocols = self
            .conn
            .get_property(
                false,
                window,
                self.wm_protocols,
                xproto::AtomEnum::ATOM,
                0,
                32,
            )?
            .reply()?;
        Ok(protocols
            .value32()
            .is_some_and(|mut atoms| atoms.any(|atom| atom == self.wm_take_focus)))
    }

    /// The message Hyprland's window manager would send when focusing it.
    fn offer_focus(&self, window: u32) -> Result<(), Fail> {
        let message = xproto::ClientMessageEvent::new(
            32,
            window,
            self.wm_protocols,
            [self.wm_take_focus, x11rb::CURRENT_TIME, 0, 0, 0],
        );
        self.conn
            .send_event(false, window, xproto::EventMask::NO_EVENT, message)?
            .check()?;
        Ok(())
    }

    fn uncovered(
        &self,
        window: u32,
        logical_size: (f64, f64),
        point: (f64, f64),
    ) -> Result<(), Fail> {
        let geometry = self.conn.get_geometry(window)?.reply()?;
        let origin = self
            .conn
            .translate_coordinates(window, self.root, 0, 0)?
            .reply()?;
        let (x, y) = root_point(
            (origin.dst_x, origin.dst_y),
            (geometry.width, geometry.height),
            logical_size,
            point,
        )
        .ok_or(refuse(COVERED))?;
        if self.agent_focus {
            return Ok(());
        }
        let child = self
            .conn
            .translate_coordinates(self.root, self.root, x, y)?
            .reply()?
            .child;
        // A menu the target app mapped over its window still belongs to the
        // target. Another of its windows may be the one the human is using.
        let own_menu = child != window
            && same_client(child, window, self.resource_mask)
            && self
                .conn
                .get_window_attributes(child)?
                .reply()?
                .override_redirect;
        if child == window || own_menu {
            Ok(())
        } else {
            Err(refuse(COVERED))
        }
    }

    /// AlreadyGrabbed means an app holds a grab on the agent pointer, usually
    /// the menu it opened in reaction to the agent.
    fn grabbed(&self, pointer: u16) -> Result<bool, Fail> {
        let status = self
            .conn
            .xinput_xi_grab_device(
                self.root,
                x11rb::CURRENT_TIME,
                x11rb::NONE,
                pointer,
                xproto::GrabMode::ASYNC,
                xproto::GrabMode::ASYNC,
                xinput::GrabOwner::NO_OWNER,
                &[0],
            )?
            .reply()?
            .status;
        if status == xproto::GrabStatus::SUCCESS {
            self.conn
                .xinput_xi_ungrab_device(x11rb::CURRENT_TIME, pointer)?
                .check()?;
            return Ok(false);
        }
        Ok(true)
    }

    fn active(&self) -> Result<u32, Fail> {
        let reply = self
            .conn
            .get_property(
                false,
                self.root,
                self.active_window,
                xproto::AtomEnum::WINDOW,
                0,
                1,
            )?
            .reply()?;
        Ok(reply
            .value32()
            .and_then(|mut value| value.next())
            .unwrap_or(0))
    }

    /// True once `pending` no longer needs the agent pair.
    fn release(&self, pending: &Pending, now: Instant) -> Result<bool, Fail> {
        let current = self
            .conn
            .xinput_xi_get_client_pointer(pending.window)?
            .reply()?;
        if current.deviceid != pending.pointer {
            return Ok(true);
        }
        let human_there = same_client(self.active()?, pending.window, self.resource_mask);
        let held = now.duration_since(pending.last);
        if human_there || held >= MAX_HOLD || (held >= HOLD && !self.grabbed(pending.pointer)?) {
            self.conn
                .xinput_xi_set_client_pointer(pending.window, pending.core)?
                .check()?;
            return Ok(true);
        }
        Ok(false)
    }

    /// Hand every app still on an agent pair back to the core pointer, except
    /// those `keep` names. Driver only points an app at a pair through one of
    /// its top-level windows, so walking those finds every such app.
    fn hand_back_all(&self, keep: impl Fn(u32) -> bool) -> Result<(), Fail> {
        let devices = self.devices()?;
        let agents: Vec<u16> = devices
            .iter()
            .filter(|device| agent_pointer(device))
            .map(|device| device.id)
            .collect();
        if agents.is_empty() {
            return Ok(());
        }
        let core = core_pointer(&devices).ok_or(refuse(ISOLATION_FAILED))?;
        for window in self.conn.query_tree(self.root)?.reply()?.children {
            // A window can be destroyed between these requests.
            let current = match self.conn.xinput_xi_get_client_pointer(window)?.reply() {
                Ok(reply) => reply.deviceid,
                Err(ReplyError::X11Error(_)) => continue,
                Err(error) => return Err(error.into()),
            };
            if !agents.contains(&current) || keep(window) {
                continue;
            }
            match self
                .conn
                .xinput_xi_set_client_pointer(window, core)?
                .check()
            {
                Ok(()) | Err(ReplyError::X11Error(_)) => {}
                Err(error) => return Err(error.into()),
            }
        }
        Ok(())
    }
}

impl State {
    fn connection(&mut self) -> Result<&X, &'static str> {
        if self.x.is_none() {
            self.x = Some(X::connect().map_err(|fail| fail.code)?);
        }
        Ok(self.x.as_ref().expect("connected above"))
    }

    fn settle<T>(&mut self, result: Result<T, Fail>) -> Result<T, &'static str> {
        result.map_err(|fail| {
            if fail.lost {
                self.x = None;
            }
            fail.code
        })
    }
}

/// The key that types `ch` in the X server's keyboard map, which is the one
/// Qt translates keys with.
pub(super) fn text_key(ch: char) -> Result<TextKey, &'static str> {
    let mut state = STATE.lock().unwrap_or_else(|poison| poison.into_inner());
    let x = state.connection()?;
    let result = (|| {
        let mapping = x.conn.get_keyboard_mapping(8, 248)?.reply()?;
        let (keycode, shift) =
            crate::input::x_key_for_char(&mapping, ch).ok_or(refuse(TEXT_KEY_UNMAPPED))?;
        Ok(TextKey { keycode, shift })
    })();
    state.settle(result)
}

/// Sends `key` to `window` as a press and release that carry their own
/// modifiers. Stock Qt 5 builds text with the modifier state of the keyboard it
/// found at startup, the human's, which the agent's X keyboard never changes,
/// but uses the state in a key event another client sent.
pub(super) fn send_text_key(window: u32, key: TextKey) -> Result<(), &'static str> {
    let mut state = STATE.lock().unwrap_or_else(|poison| poison.into_inner());
    let x = state.connection()?;
    let result = (|| {
        let modifiers = if key.shift {
            xproto::KeyButMask::SHIFT
        } else {
            xproto::KeyButMask::from(0u16)
        };
        for (response_type, mask) in [
            (xproto::KEY_PRESS_EVENT, xproto::EventMask::KEY_PRESS),
            (xproto::KEY_RELEASE_EVENT, xproto::EventMask::KEY_RELEASE),
        ] {
            let event = xproto::KeyPressEvent {
                response_type,
                detail: key.keycode,
                sequence: 0,
                time: x11rb::CURRENT_TIME,
                root: x.root,
                event: window,
                child: x11rb::NONE,
                root_x: 0,
                root_y: 0,
                event_x: 0,
                event_y: 0,
                state: modifiers,
                same_screen: true,
            };
            x.conn.send_event(false, window, mask, event)?.check()?;
        }
        Ok(())
    })();
    state.settle(result)
}

/// Isolate `seat`, then focus or check the target as `need` requires. Keep the
/// returned value alive until the action has been dispatched.
pub(super) fn prepare(
    seat: u32,
    window: u32,
    logical_size: (f64, f64),
    need: Need,
) -> Result<Prepared, &'static str> {
    let mut state = STATE.lock().unwrap_or_else(|poison| poison.into_inner());
    let x = state.connection()?;
    let resource_mask = x.resource_mask;
    let result = (|| {
        let (pointer, keyboard, core) = x.isolate(seat)?;
        // After a hand-back the app may have judged focus by the human's keyboard.
        let held = x
            .conn
            .xinput_xi_get_client_pointer(window)?
            .reply()?
            .deviceid
            == pointer;
        let offer = match need {
            Need::Keyboard => x.focus(keyboard, window)? || !held,
            Need::Pointer { x: px, y: py } => {
                x.uncovered(window, logical_size, (px, py))?;
                false
            }
        };
        x.conn
            .xinput_xi_set_client_pointer(window, pointer)?
            .check()?;
        // Sent after the client pointer moved, so the app's own focus request
        // lands on the agent keyboard. Wine and Java switch their active window
        // on it, which would move the human's typing if they are in that app.
        if offer && !same_client(x.active()?, window, x.resource_mask) && x.takes_focus(window)? {
            x.offer_focus(window)?;
        }
        Ok((pointer, core))
    })();
    let (pointer, core) = state.settle(result)?;
    let last = Instant::now();
    // The client pointer belongs to the X client, not to one of its windows.
    match state
        .pending
        .iter_mut()
        .find(|p| same_client(p.window, window, resource_mask))
    {
        Some(pending) => {
            // The newest window is the one most likely to outlive the hold.
            pending.window = window;
            pending.pointer = pointer;
            pending.core = core;
            pending.last = last;
        }
        None => state.pending.push(Pending {
            window,
            pointer,
            core,
            last,
        }),
    }
    state.in_flight.push(pointer);
    Ok(Prepared { pointer })
}

/// The hold that keeps the app's client pointer on the agent pair starts when
/// the action ends.
impl Drop for Prepared {
    fn drop(&mut self) {
        let mut state = STATE.lock().unwrap_or_else(|poison| poison.into_inner());
        if let Some(index) = state.in_flight.iter().position(|&p| p == self.pointer) {
            state.in_flight.swap_remove(index);
        }
        let now = Instant::now();
        for pending in state
            .pending
            .iter_mut()
            .filter(|p| p.pointer == self.pointer)
        {
            pending.last = now;
        }
        if !state.poller && !state.pending.is_empty() {
            state.poller = std::thread::Builder::new()
                .name("cua-xwayland-release".into())
                .spawn(release_loop)
                .is_ok();
        }
    }
}

/// Only the process that moved an app onto an agent pair hands it back. A
/// Driver killed within the hold leaves the app there, so its pointer queries
/// and grabs keep using the agent pointer. A new daemon repairs that once at
/// startup. The pairs themselves stay for reuse.
pub(crate) fn recover() {
    let mut state = STATE.lock().unwrap_or_else(|poison| poison.into_inner());
    // An action this daemon already started holds its app on purpose.
    let held: Vec<u32> = state.pending.iter().map(|p| p.window).collect();
    let Ok(x) = state.connection() else {
        return;
    };
    let mask = x.resource_mask;
    let result = x.hand_back_all(|window| held.iter().any(|&p| same_client(p, window, mask)));
    if let Err(code) = state.settle(result) {
        tracing::warn!(code, "could not hand X apps back from stale agent pointers");
    }
}

fn release_loop() {
    loop {
        std::thread::sleep(POLL);
        let mut state = STATE.lock().unwrap_or_else(|poison| poison.into_inner());
        let now = Instant::now();
        let State {
            x,
            pending,
            in_flight,
            poller,
        } = &mut *state;
        let Some(conn) = x.as_ref() else {
            pending.clear();
            *poller = false;
            return;
        };
        let mut lost = false;
        pending.retain(|p| {
            // Probing grabs an agent pointer that an action may be using.
            if lost || in_flight.contains(&p.pointer) {
                return true;
            }
            match conn.release(p, now) {
                Ok(done) => !done,
                Err(fail) => {
                    lost |= fail.lost;
                    // The window is gone, so the app no longer needs its client pointer.
                    false
                }
            }
        });
        if lost {
            *x = None;
            pending.clear();
        }
        if pending.is_empty() {
            *poller = false;
            return;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn device(id: u16, name: &str, kind: DeviceType, attachment: u16) -> Device {
        Device {
            id,
            name: name.into(),
            kind,
            attachment,
        }
    }

    /// The device list Xwayland 24.1 reports with the human seat 1 and agent
    /// seat 71 before Driver touched anything, plus a seat-71 master pair.
    fn stock(with_pair: bool) -> Vec<Device> {
        let mut devices = vec![
            device(2, "Virtual core pointer", DeviceType::MASTER_POINTER, 3),
            device(3, "Virtual core keyboard", DeviceType::MASTER_KEYBOARD, 2),
            device(6, "xwayland-pointer:71", DeviceType::SLAVE_POINTER, 2),
            device(
                7,
                "xwayland-relative-pointer:71",
                DeviceType::SLAVE_POINTER,
                2,
            ),
            device(
                8,
                "xwayland-pointer-gestures:71",
                DeviceType::SLAVE_POINTER,
                2,
            ),
            device(9, "xwayland-keyboard:71", DeviceType::SLAVE_KEYBOARD, 3),
            device(14, "xwayland-pointer:1", DeviceType::SLAVE_POINTER, 2),
            device(17, "xwayland-keyboard:1", DeviceType::SLAVE_KEYBOARD, 3),
        ];
        if with_pair {
            devices.push(device(
                18,
                "cua-xwayland-71 pointer",
                DeviceType::MASTER_POINTER,
                19,
            ));
            devices.push(device(
                19,
                "cua-xwayland-71 keyboard",
                DeviceType::MASTER_KEYBOARD,
                18,
            ));
        }
        devices
    }

    #[test]
    fn agent_seat_devices_move_to_their_own_pair_and_the_human_seat_stays() {
        assert_eq!(master_pair(&stock(false), 71), None);
        let devices = stock(true);
        let pair = master_pair(&devices, 71).unwrap();
        assert_eq!(pair, (18, 19));
        assert_eq!(
            attachments(&devices, 71, pair).unwrap(),
            vec![(6, 18), (7, 18), (8, 18), (9, 19)]
        );
        assert_eq!(core_pointer(&devices), Some(2));
    }

    #[test]
    fn attached_and_floating_devices_are_judged_by_their_master() {
        let mut devices = stock(true);
        for (id, master) in [(6, 18), (7, 18), (9, 19)] {
            devices.iter_mut().find(|d| d.id == id).unwrap().attachment = master;
        }
        devices.iter_mut().find(|d| d.id == 8).unwrap().kind = DeviceType::FLOATING_SLAVE;
        assert_eq!(attachments(&devices, 71, (18, 19)).unwrap(), vec![(8, 18)]);
        devices.iter_mut().find(|d| d.id == 8).unwrap().kind = DeviceType::SLAVE_POINTER;
        devices.iter_mut().find(|d| d.id == 8).unwrap().attachment = 18;
        assert!(attachments(&devices, 71, (18, 19)).unwrap().is_empty());
    }

    #[test]
    fn a_seat_xwayland_has_not_bound_refuses() {
        let devices = stock(true);
        assert_eq!(attachments(&devices, 72, (18, 19)), Err(SEAT_UNAVAILABLE));
        let without_keyboard: Vec<_> = devices.into_iter().filter(|d| d.id != 9).collect();
        assert_eq!(
            attachments(&without_keyboard, 71, (18, 19)),
            Err(SEAT_UNAVAILABLE)
        );
    }

    #[test]
    fn an_unpaired_master_is_not_the_seat_pair() {
        let mut devices = stock(true);
        devices.iter_mut().find(|d| d.id == 18).unwrap().attachment = 3;
        assert_eq!(master_pair(&devices, 71), None);
    }

    #[test]
    fn recovery_recognizes_only_the_masters_isolate_adds() {
        let mut devices = stock(true);
        devices.push(device(
            30,
            "cua-xwayland-72 pointer",
            DeviceType::MASTER_POINTER,
            31,
        ));
        let agents: Vec<u16> = devices
            .iter()
            .filter(|device| agent_pointer(device))
            .map(|device| device.id)
            .collect();
        assert_eq!(agents, vec![18, 30]);
        for (name, kind) in [
            ("cua-xwayland-71 keyboard", DeviceType::MASTER_KEYBOARD),
            ("cua-xwayland-71 XTEST pointer", DeviceType::SLAVE_POINTER),
            ("cua-xwayland-71 pointer", DeviceType::SLAVE_POINTER),
            ("cua-xwayland-071 pointer", DeviceType::MASTER_POINTER),
            ("cua-xwayland-+71 pointer", DeviceType::MASTER_POINTER),
            ("cua-xwayland- pointer", DeviceType::MASTER_POINTER),
            ("xcua-xwayland-71 pointer", DeviceType::MASTER_POINTER),
            ("CUA v1.AAAA.AAAA.AAAA pointer", DeviceType::MASTER_POINTER),
        ] {
            assert!(!agent_pointer(&device(40, name, kind, 41)), "{name}");
        }
    }

    #[test]
    fn coverage_compares_the_owning_client() {
        let mask = 0x001f_ffff;
        assert!(same_client(0x0040_0037, 0x0040_0037, mask));
        assert!(same_client(0x0040_0037, 0x0040_1000, mask));
        assert!(!same_client(0x0080_000c, 0x0040_0037, mask));
        assert!(!same_client(0, 0x0040_0037, mask));
    }

    #[test]
    fn logical_points_map_to_x_pixels_inside_the_window() {
        assert_eq!(
            root_point((100, 400), (400, 300), (400.0, 300.0), (200.0, 150.0)),
            Some((300, 550))
        );
        assert_eq!(
            root_point((0, 0), (800, 600), (400.0, 300.0), (200.5, 150.0)),
            Some((401, 300))
        );
        assert_eq!(
            root_point((0, 0), (400, 300), (400.0, 300.0), (400.0, 10.0)),
            None
        );
        assert_eq!(
            root_point((0, 0), (400, 300), (400.0, 300.0), (-1.0, 10.0)),
            None
        );
    }
}
