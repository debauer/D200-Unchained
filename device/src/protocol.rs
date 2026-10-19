//! deckd wire protocol v1 (see docs/protocol.md): framing and message coding.

use std::io::{self, Read, Write};

use crate::canvas::Rgb;
use crate::layout::Key;
use crate::rle;

pub const VERSION: u16 = 5;
pub const PORT: u16 = 7200;
pub const MAX_PAYLOAD: usize = 4 << 20;

pub mod msg {
    pub const HELLO: u8 = 0x01;
    pub const SET_BRIGHTNESS: u8 = 0x02;
    pub const CLEAR: u8 = 0x03;
    pub const FILL_KEY: u8 = 0x04;
    pub const SET_KEY_IMAGE: u8 = 0x05;
    pub const PING: u8 = 0x06;
    pub const SYSINFO: u8 = 0x07;
    pub const OK: u8 = 0x80;
    pub const INFO: u8 = 0x81;
    pub const SYSINFO_REPLY: u8 = 0x82;
    pub const KEY_EVENT: u8 = 0x90;
    pub const ERROR: u8 = 0xFF;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PixelFormat {
    Rgb565,
    Rgb888,
}

impl PixelFormat {
    fn from_u8(v: u8) -> Result<Self, String> {
        match v {
            0 | 2 => Ok(PixelFormat::Rgb565), // 2: run-length coded, decoded when the command is parsed
            1 => Ok(PixelFormat::Rgb888),
            _ => Err(format!("unknown pixel format {v}")),
        }
    }

    pub fn bytes_per_pixel(self) -> usize {
        match self {
            PixelFormat::Rgb565 => 2,
            PixelFormat::Rgb888 => 3,
        }
    }

    /// Colour of pixel `i` in `data` (caller guarantees the length).
    pub fn pixel(self, data: &[u8], i: usize) -> Rgb {
        match self {
            PixelFormat::Rgb565 => {
                let v = u16::from_le_bytes([data[2 * i], data[2 * i + 1]]);
                let (r, g, b) = ((v >> 11) as u8 & 0x1F, (v >> 5) as u8 & 0x3F, v as u8 & 0x1F);
                Rgb(r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)
            }
            PixelFormat::Rgb888 => Rgb(data[3 * i], data[3 * i + 1], data[3 * i + 2]),
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum Command {
    Hello,
    SetBrightness(u8),
    Clear(Rgb),
    FillKey { index: u16, color: Rgb },
    SetKeyImage { index: u16, format: PixelFormat, w: usize, h: usize, pixels: Vec<u8> },
    Ping,
    SysInfo,
}

fn need(p: &[u8], n: usize, what: &str) -> Result<(), String> {
    if p.len() == n {
        Ok(())
    } else {
        Err(format!("{what}: payload must be {n} bytes, got {}", p.len()))
    }
}

fn u16_at(p: &[u8], i: usize) -> u16 {
    u16::from_le_bytes([p[i], p[i + 1]])
}

pub fn parse_command(ty: u8, p: &[u8]) -> Result<Command, String> {
    match ty {
        msg::HELLO => need(p, 0, "HELLO").map(|_| Command::Hello),
        msg::PING => need(p, 0, "PING").map(|_| Command::Ping),
        msg::SYSINFO => need(p, 0, "SYSINFO").map(|_| Command::SysInfo),
        msg::SET_BRIGHTNESS => need(p, 1, "SET_BRIGHTNESS").map(|_| Command::SetBrightness(p[0])),
        msg::CLEAR => need(p, 3, "CLEAR").map(|_| Command::Clear(Rgb(p[0], p[1], p[2]))),
        msg::FILL_KEY => need(p, 5, "FILL_KEY")
            .map(|_| Command::FillKey { index: u16_at(p, 0), color: Rgb(p[2], p[3], p[4]) }),
        msg::SET_KEY_IMAGE => {
            if p.len() < 7 {
                return Err(format!("SET_KEY_IMAGE: header needs 7 bytes, got {}", p.len()));
            }
            let format = PixelFormat::from_u8(p[2])?;
            let (w, h) = (u16_at(p, 3) as usize, u16_at(p, 5) as usize);
            let pixels = &p[7..];
            let expected = w * h * format.bytes_per_pixel();
            let pixels = if p[2] == 2 {
                rle::decode(pixels, w * h).map_err(|e| format!("SET_KEY_IMAGE: {e}"))?
            } else if pixels.len() == expected {
                pixels.to_vec()
            } else {
                return Err(format!("SET_KEY_IMAGE: {w}x{h} needs {expected} pixel bytes, got {}", pixels.len()));
            };
            Ok(Command::SetKeyImage { index: u16_at(p, 0), format, w, h, pixels })
        }
        _ => Err(format!("unknown message type 0x{ty:02x}")),
    }
}

/// Read one frame; payloads above `MAX_PAYLOAD` are an error.
pub fn read_frame(r: &mut impl Read) -> io::Result<(u8, Vec<u8>)> {
    let mut head = [0u8; 5];
    r.read_exact(&mut head)?;
    let len = u32::from_le_bytes([head[1], head[2], head[3], head[4]]) as usize;
    if len > MAX_PAYLOAD {
        return Err(io::Error::new(io::ErrorKind::InvalidData, format!("frame of {len} bytes exceeds limit")));
    }
    let mut payload = vec![0u8; len];
    r.read_exact(&mut payload)?;
    Ok((head[0], payload))
}

/// Encode a frame into one buffer so it can be written atomically.
pub fn frame(ty: u8, payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(5 + payload.len());
    out.push(ty);
    out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    out.extend_from_slice(payload);
    out
}

pub fn write_frame(w: &mut impl Write, ty: u8, payload: &[u8]) -> io::Result<()> {
    w.write_all(&frame(ty, payload))
}

pub fn info_payload(width: usize, height: usize, keys: &[Key]) -> Vec<u8> {
    let mut p = Vec::new();
    for v in [VERSION, width as u16, height as u16] {
        p.extend_from_slice(&v.to_le_bytes());
    }
    p.push(keys.len() as u8);
    for k in keys {
        let r = k.rect;
        for v in [k.index, k.code, r.x as u16, r.y as u16, r.w as u16, r.h as u16] {
            p.extend_from_slice(&v.to_le_bytes());
        }
    }
    p
}

pub fn key_event_payload(index: u16, pressed: bool) -> Vec<u8> {
    let c = index.to_le_bytes();
    vec![c[0], c[1], pressed as u8]
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::GRID;

    #[test]
    fn parses_simple_commands() {
        assert_eq!(parse_command(msg::HELLO, &[]), Ok(Command::Hello));
        assert_eq!(parse_command(msg::SYSINFO, &[]), Ok(Command::SysInfo));
        assert!(parse_command(msg::SYSINFO, &[0]).is_err());
        assert_eq!(parse_command(msg::SET_BRIGHTNESS, &[200]), Ok(Command::SetBrightness(200)));
        assert_eq!(parse_command(msg::CLEAR, &[1, 2, 3]), Ok(Command::Clear(Rgb(1, 2, 3))));
        assert_eq!(
            parse_command(msg::FILL_KEY, &[30, 0, 9, 8, 7]),
            Ok(Command::FillKey { index: 30, color: Rgb(9, 8, 7) })
        );
        assert!(parse_command(msg::CLEAR, &[1, 2]).is_err());
        assert!(parse_command(0x42, &[]).unwrap_err().contains("0x42"));
    }

    #[test]
    fn parses_key_image_and_checks_length() {
        let mut p = vec![7, 0, 0, 2, 0, 1, 0]; // index 7, RGB565, 2x1
        p.extend_from_slice(&[0x00, 0xF8, 0xE0, 0x07]); // red, green
        let cmd = parse_command(msg::SET_KEY_IMAGE, &p).unwrap();
        let Command::SetKeyImage { index, format, w, h, pixels } = cmd else { panic!() };
        assert_eq!((index, format, w, h), (7, PixelFormat::Rgb565, 2, 1));
        assert_eq!(format.pixel(&pixels, 0), Rgb(255, 0, 0));
        assert_eq!(format.pixel(&pixels, 1), Rgb(0, 255, 0));
        p.pop();
        assert!(parse_command(msg::SET_KEY_IMAGE, &p).unwrap_err().contains("needs 4 pixel bytes"));
    }

    #[test]
    fn parses_run_length_coded_key_image() {
        let mut p = vec![3, 0, 2, 3, 0, 1, 0]; // index 3, format 2, 3x1
        p.extend_from_slice(&[128, 0x00, 0xF8, 0, 0xE0, 0x07]); // red twice, one green
        let Command::SetKeyImage { format, w, h, pixels, .. } = parse_command(msg::SET_KEY_IMAGE, &p).unwrap() else {
            panic!()
        };
        assert_eq!((format, w, h), (PixelFormat::Rgb565, 3, 1));
        assert_eq!(pixels, [0x00, 0xF8, 0x00, 0xF8, 0xE0, 0x07]);
        p.truncate(p.len() - 3); // only two of three pixels
        assert!(parse_command(msg::SET_KEY_IMAGE, &p).unwrap_err().contains("2 of the 3 pixels"));
    }

    #[test]
    fn rgb565_white_and_rgb888() {
        assert_eq!(PixelFormat::Rgb565.pixel(&[0xFF, 0xFF], 0), Rgb(255, 255, 255));
        assert_eq!(PixelFormat::Rgb888.pixel(&[1, 2, 3, 4, 5, 6], 1), Rgb(4, 5, 6));
    }

    #[test]
    fn frame_roundtrip_and_limit() {
        let bytes = frame(msg::ERROR, b"boom");
        assert_eq!(read_frame(&mut &bytes[..]).unwrap(), (msg::ERROR, b"boom".to_vec()));
        let mut huge = vec![msg::PING];
        huge.extend_from_slice(&((MAX_PAYLOAD + 1) as u32).to_le_bytes());
        assert!(read_frame(&mut &huge[..]).is_err());
    }

    #[test]
    fn info_lists_all_keys() {
        let keys = GRID.keys();
        let p = info_payload(1280, 720, &keys);
        assert_eq!(p.len(), 7 + 12 * keys.len());
        assert_eq!(u16_at(&p, 0), VERSION);
        assert_eq!((u16_at(&p, 2), u16_at(&p, 4), p[6]), (1280, 720, 14));
        assert_eq!(u16_at(&p, 7), keys[0].index);
        assert_eq!(u16_at(&p, 9), keys[0].code);
        assert_eq!(key_event_payload(30, true), vec![30, 0, 1]);
    }
}
