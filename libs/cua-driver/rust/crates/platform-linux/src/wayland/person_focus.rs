//! When the person's focus lands on a window an agent drives, or leaves it,
//! what the agent last read of that window may no longer hold: they could have
//! clicked, typed, scrolled or closed a dialog there. Hyprland announces every
//! focus change on its event socket, so the driver retires those reads and the
//! agent's next action on the window is refused until it reads it again.

use std::io::BufRead;
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::OnceLock;
use std::time::{Duration, Instant};

/// Foreground input changes focus on purpose. Those changes are the driver's,
/// not the person's.
static DRIVER_FOCUS: AtomicUsize = AtomicUsize::new(0);
/// Hyprland reports a focus change just after it happens, so the driver's own
/// changes stay quiet for a moment after it hands focus back.
static QUIET_UNTIL_MS: AtomicU64 = AtomicU64::new(0);
const QUIET_AFTER: Duration = Duration::from_millis(500);
const RECONNECT_AFTER: Duration = Duration::from_secs(2);

/// Held while the driver itself moves focus.
pub(crate) struct DriverFocus;

impl DriverFocus {
    pub(crate) fn hold() -> Self {
        DRIVER_FOCUS.fetch_add(1, Ordering::SeqCst);
        Self
    }
}

impl Drop for DriverFocus {
    fn drop(&mut self) {
        QUIET_UNTIL_MS.store(now_ms() + QUIET_AFTER.as_millis() as u64, Ordering::SeqCst);
        DRIVER_FOCUS.fetch_sub(1, Ordering::SeqCst);
    }
}

fn now_ms() -> u64 {
    static START: OnceLock<Instant> = OnceLock::new();
    START.get_or_init(Instant::now).elapsed().as_millis() as u64
}

fn driver_moved_focus() -> bool {
    DRIVER_FOCUS.load(Ordering::SeqCst) > 0 || now_ms() < QUIET_UNTIL_MS.load(Ordering::SeqCst)
}

/// Starts watching once per process. Reconnects after Hyprland restarts.
pub(crate) fn start() {
    static STARTED: OnceLock<()> = OnceLock::new();
    STARTED.get_or_init(|| {
        let spawned = std::thread::Builder::new()
            .name("cua-person-focus".into())
            .spawn(|| loop {
                if let Ok(stream) = super::hyprland::event_socket_path()
                    .and_then(|path| Ok(UnixStream::connect(path)?))
                {
                    watch(
                        std::io::BufReader::new(stream),
                        driver_moved_focus,
                        |window| {
                            cua_driver_core::snapshot_store::retire_person_window_everywhere(
                                window,
                            );
                        },
                    );
                }
                std::thread::sleep(RECONNECT_AFTER);
            });
        if let Err(error) = spawned {
            tracing::warn!("person focus watcher did not start: {error}");
        }
    });
}

/// Calls `retire` for the window the person's focus left and the one it
/// reached, on every focus change the driver did not make.
fn watch(events: impl BufRead, driver_moved_focus: impl Fn() -> bool, mut retire: impl FnMut(u64)) {
    let mut focused = None;
    for line in events.lines() {
        let Ok(line) = line else { return };
        // Hyprland repeats the event when the focused window changes title.
        let Some(next) = focused_window(&line).filter(|next| *next != focused) else {
            continue;
        };
        let left = std::mem::replace(&mut focused, next);
        if driver_moved_focus() {
            continue;
        }
        for window in [left, next].into_iter().flatten() {
            retire(window);
        }
    }
}

/// The window an `activewindowv2` event focuses, `Some(None)` when nothing
/// has focus.
fn focused_window(line: &str) -> Option<Option<u64>> {
    let address = line.strip_prefix("activewindowv2>>")?;
    Some(
        u64::from_str_radix(address.trim(), 16)
            .ok()
            .filter(|a| *a != 0),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn retired(events: &str, driver_moved_focus: bool) -> Vec<u64> {
        let mut retired = Vec::new();
        watch(
            events.as_bytes(),
            || driver_moved_focus,
            |window| retired.push(window),
        );
        retired
    }

    #[test]
    fn a_visit_retires_the_window_on_arrival_and_on_leaving() {
        let events = "activewindow>>ghostty,~\n\
                      activewindowv2>>a1\n\
                      workspace>>4\n\
                      activewindowv2>>55daa5f8e810\n\
                      windowtitlev2>>55daa5f8e810,Discord\n\
                      activewindowv2>>55daa5f8e810\n\
                      activewindowv2>>a1\n";
        assert_eq!(
            retired(events, false),
            [0xa1, 0xa1, 0x55da_a5f8_e810, 0x55da_a5f8_e810, 0xa1]
        );
    }

    #[test]
    fn an_empty_workspace_has_no_window_to_retire() {
        assert_eq!(
            retired(
                "activewindowv2>>a1\nactivewindowv2>>\nactivewindowv2>>b2\n",
                false
            ),
            [0xa1, 0xa1, 0xb2]
        );
    }

    #[test]
    fn focus_the_driver_moves_retires_nothing() {
        assert!(retired("activewindowv2>>a1\nactivewindowv2>>b2\n", true).is_empty());
    }
}
