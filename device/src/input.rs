//! evdev key events from `/dev/input/eventN`.

use std::fs::File;
use std::io::{self, Read};
use std::path::Path;

/// Size of `struct input_event` on a 32-bit ARM kernel:
/// sec u32, usec u32, type u16, code u16, value i32.
pub const EVENT_SIZE: usize = 16;
const EV_KEY: u16 = 0x01;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct KeyEvent {
    pub code: u16,
    pub pressed: bool,
}

/// Decode one raw event; returns `None` for non-key events and autorepeat.
pub fn parse_event(raw: &[u8; EVENT_SIZE]) -> Option<KeyEvent> {
    let ev_type = u16::from_le_bytes([raw[8], raw[9]]);
    let code = u16::from_le_bytes([raw[10], raw[11]]);
    let value = i32::from_le_bytes([raw[12], raw[13], raw[14], raw[15]]);
    match (ev_type, value) {
        (EV_KEY, 0) => Some(KeyEvent { code, pressed: false }),
        (EV_KEY, 1) => Some(KeyEvent { code, pressed: true }),
        _ => None,
    }
}

pub struct Keys {
    file: File,
}

impl Keys {
    pub fn open(dev: &Path) -> io::Result<Self> {
        let file = File::open(dev)
            .map_err(|e| io::Error::new(e.kind(), format!("{}: {e}", dev.display())))?;
        Ok(Keys { file })
    }

    /// Block until the next key press or release.
    pub fn next(&mut self) -> io::Result<KeyEvent> {
        let mut raw = [0u8; EVENT_SIZE];
        loop {
            self.file.read_exact(&mut raw)?;
            if let Some(ev) = parse_event(&raw) {
                return Ok(ev);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn raw(ev_type: u16, code: u16, value: i32) -> [u8; EVENT_SIZE] {
        let mut r = [0u8; EVENT_SIZE];
        r[8..10].copy_from_slice(&ev_type.to_le_bytes());
        r[10..12].copy_from_slice(&code.to_le_bytes());
        r[12..16].copy_from_slice(&value.to_le_bytes());
        r
    }

    #[test]
    fn decodes_press_and_release() {
        assert_eq!(parse_event(&raw(1, 29, 1)), Some(KeyEvent { code: 29, pressed: true }));
        assert_eq!(parse_event(&raw(1, 7, 0)), Some(KeyEvent { code: 7, pressed: false }));
    }

    #[test]
    fn ignores_sync_msc_and_repeat() {
        assert_eq!(parse_event(&raw(0, 0, 0)), None);
        assert_eq!(parse_event(&raw(4, 4, 7)), None);
        assert_eq!(parse_event(&raw(1, 7, 2)), None);
    }
}
