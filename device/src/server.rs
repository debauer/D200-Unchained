//! deckd server: several TCP clients; commands are serialised on the device,
//! replies go to the sender, key events to every client after HELLO.

use std::collections::HashMap;
use std::io::{self, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use crate::backlight::Backlight;
use crate::canvas::Canvas;
use crate::fb::Framebuffer;
use crate::input::Keys;
use crate::layout::{index_for_code, key_for_index, Key};
use crate::protocol::{self, msg, Command};
use crate::screen::Screen;
use crate::sysinfo;

/// What the caller has to do after a command changed the canvas.
#[derive(Debug, PartialEq, Eq)]
pub enum Effect {
    None,
    /// Flush this framebuffer-space region (x, y, w, h).
    Flush(usize, usize, usize, usize),
    FlushAll,
    Brightness(u8),
}

/// Execute a drawing/brightness command on the canvas (no I/O).
pub fn apply(canvas: &mut Canvas, keys: &[Key], cmd: &Command) -> Result<Effect, String> {
    let mut s = Screen::new(canvas);
    let key = |index: u16| key_for_index(keys, index).ok_or_else(|| format!("unknown key {index}"));
    match cmd {
        Command::Hello | Command::Ping | Command::SysInfo => Ok(Effect::None),
        Command::SetBrightness(v) => Ok(Effect::Brightness(*v)),
        Command::Clear(c) => {
            let (w, h) = (s.width(), s.height());
            s.fill_rect(0, 0, w, h, *c);
            Ok(Effect::FlushAll)
        }
        Command::FillKey { index, color } => {
            let r = key(*index)?.rect;
            s.fill_rect(r.x, r.y, r.w, r.h, *color);
            let (x, y, w, h) = s.fb_region(r.x, r.y, r.w, r.h);
            Ok(Effect::Flush(x, y, w, h))
        }
        Command::SetKeyImage { index, format, w, h, pixels } => {
            let r = key(*index)?.rect;
            if *w > r.w || *h > r.h {
                return Err(format!("image {w}x{h} larger than key {index} window {}x{}", r.w, r.h));
            }
            let (x0, y0) = (r.x + (r.w - w) / 2, r.y + (r.h - h) / 2);
            s.blit(x0, y0, *w, *h, |col, row| format.pixel(pixels, row * w + col));
            let (x, y, fw, fh) = s.fb_region(x0, y0, *w, *h);
            Ok(Effect::Flush(x, y, fw, fh))
        }
    }
}

/// Upper bound on simultaneous connections; further clients get an ERROR and are closed.
pub const MAX_CLIENTS: usize = 8;
/// A client that cannot take a frame within this time is dropped.
const WRITE_TIMEOUT: Duration = Duration::from_secs(2);

struct Client {
    stream: TcpStream,
    hello_done: bool,
}

type Clients = Arc<Mutex<HashMap<u64, Client>>>;

struct Device {
    fb: Framebuffer,
    backlight: Backlight,
}

/// Send a frame to client `id`; false if it is gone or the write failed (then it is dropped).
fn send(clients: &Clients, id: u64, ty: u8, payload: &[u8]) -> bool {
    let mut map = clients.lock().unwrap();
    let Some(c) = map.get_mut(&id) else { return false };
    match protocol::write_frame(&mut c.stream, ty, payload) {
        Ok(()) => true,
        Err(e) => {
            eprintln!("client {id}: write failed: {e}");
            drop_client(&mut map, id);
            false
        }
    }
}

/// Send a frame to every client that completed HELLO; failing clients are dropped.
fn broadcast(clients: &Clients, ty: u8, payload: &[u8]) {
    let mut map = clients.lock().unwrap();
    let frame = protocol::frame(ty, payload);
    let failed: Vec<u64> = map
        .iter_mut()
        .filter(|(_, c)| c.hello_done)
        .filter_map(|(id, c)| c.stream.write_all(&frame).err().map(|e| (*id, e)))
        .map(|(id, e)| {
            eprintln!("client {id}: event write failed: {e}");
            id
        })
        .collect();
    for id in failed {
        drop_client(&mut map, id);
    }
}

fn drop_client(map: &mut HashMap<u64, Client>, id: u64) {
    if let Some(c) = map.remove(&id) {
        // Ignore errors: the socket may already be closed by the peer.
        let _ = c.stream.shutdown(Shutdown::Both);
    }
}

fn execute(dev: &Mutex<Device>, keys: &[Key], cmd: &Command) -> Result<(), String> {
    let mut d = dev.lock().unwrap();
    let io_err = |e: io::Error| e.to_string();
    match apply(&mut d.fb.canvas, keys, cmd)? {
        Effect::None => Ok(()),
        Effect::Flush(x, y, w, h) => d.fb.flush_region(x, y, w, h).map_err(io_err),
        Effect::FlushAll => d.fb.flush().map_err(io_err),
        Effect::Brightness(v) => {
            let value = v as u32 * d.backlight.max / 255;
            d.backlight.set(value).map_err(io_err)
        }
    }
}

fn serve_client(id: u64, mut stream: TcpStream, clients: Clients, dev: Arc<Mutex<Device>>, keys: Arc<Vec<Key>>) {
    loop {
        let (ty, payload) = match protocol::read_frame(&mut stream) {
            Ok(f) => f,
            Err(e) => {
                eprintln!("client {id}: disconnected ({e})");
                break;
            }
        };
        let ok = match protocol::parse_command(ty, &payload) {
            Ok(Command::Hello) => {
                let (w, h) = {
                    let mut d = dev.lock().unwrap();
                    let s = Screen::new(&mut d.fb.canvas);
                    (s.width(), s.height())
                };
                let sent = send(&clients, id, msg::INFO, &protocol::info_payload(w, h, &keys));
                if let Some(c) = clients.lock().unwrap().get_mut(&id) {
                    c.hello_done = true;
                }
                sent
            }
            Ok(Command::SysInfo) => send(&clients, id, msg::SYSINFO_REPLY, sysinfo::collect().as_bytes()),
            Ok(cmd) => match execute(&dev, &keys, &cmd) {
                Ok(()) => send(&clients, id, msg::OK, &[]),
                Err(why) => send(&clients, id, msg::ERROR, why.as_bytes()),
            },
            Err(why) => send(&clients, id, msg::ERROR, why.as_bytes()),
        };
        if !ok {
            break;
        }
    }
    drop_client(&mut clients.lock().unwrap(), id);
}

fn forward_keys(mut keys: Keys, clients: Clients, layout: Arc<Vec<Key>>) {
    loop {
        let ev = match keys.next() {
            Ok(ev) => ev,
            Err(e) => {
                eprintln!("key input failed: {e}");
                std::process::exit(1);
            }
        };
        if let Some(index) = index_for_code(&layout, ev.code) {
            broadcast(&clients, msg::KEY_EVENT, &protocol::key_event_payload(index, ev.pressed));
        }
    }
}

/// Register a new connection, or refuse it when `MAX_CLIENTS` are connected.
fn admit(clients: &Clients, id: u64, stream: &TcpStream) -> io::Result<bool> {
    stream.set_nodelay(true)?;
    stream.set_write_timeout(Some(WRITE_TIMEOUT))?;
    let mut writer = stream.try_clone()?;
    let mut map = clients.lock().unwrap();
    if map.len() >= MAX_CLIENTS {
        let why = format!("too many clients (max {MAX_CLIENTS})");
        eprintln!("client {id}: refused, {why}");
        // Best effort: the refused peer may not read the error before the close.
        let _ = protocol::write_frame(&mut writer, msg::ERROR, why.as_bytes());
        let _ = writer.shutdown(Shutdown::Both);
        return Ok(false);
    }
    map.insert(id, Client { stream: writer, hello_done: false });
    eprintln!("client {id}: connected from {:?} ({} clients)", stream.peer_addr().ok(), map.len());
    Ok(true)
}

pub struct Config {
    pub fb: PathBuf,
    pub fb_sysfs: PathBuf,
    pub input: PathBuf,
    pub backlight: PathBuf,
    pub listen: String,
}

pub fn run(cfg: &Config, layout: Vec<Key>) -> io::Result<()> {
    let mut fb = Framebuffer::open_with_sysfs(&cfg.fb, &cfg.fb_sysfs)?;
    fb.canvas.clear(crate::canvas::Rgb(0, 0, 0));
    fb.flush()?;
    let dev = Arc::new(Mutex::new(Device { fb, backlight: Backlight::open(&cfg.backlight)? }));
    let keys = Arc::new(layout);
    let clients: Clients = Arc::new(Mutex::new(HashMap::new()));

    let input = Keys::open(&cfg.input)?;
    let (c, k) = (Arc::clone(&clients), Arc::clone(&keys));
    thread::spawn(move || forward_keys(input, c, k));

    let listener = TcpListener::bind(&cfg.listen)?;
    eprintln!("deckd: listening on {}", cfg.listen);
    for (id, conn) in (1u64..).zip(listener.incoming()) {
        let stream = match conn {
            Ok(s) => s,
            Err(e) => {
                eprintln!("accept failed: {e}");
                continue;
            }
        };
        if !admit(&clients, id, &stream)? {
            continue;
        }
        let (c, d, k) = (Arc::clone(&clients), Arc::clone(&dev), Arc::clone(&keys));
        thread::spawn(move || serve_client(id, stream, c, d, k));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::canvas::Rgb;
    use crate::layout::GRID;
    use crate::protocol::PixelFormat;

    fn canvas() -> Canvas {
        Canvas::new(720, 1280, 2880)
    }

    #[test]
    fn fill_key_paints_window_and_reports_region() {
        let (mut c, keys) = (canvas(), GRID.keys());
        let r = key_for_index(&keys, 7).unwrap().rect;
        let effect = apply(&mut c, &keys, &Command::FillKey { index: 7, color: Rgb(1, 2, 3) }).unwrap();
        let (x, y, w, h) = Screen::new(&mut c).fb_region(r.x, r.y, r.w, r.h);
        assert_eq!(effect, Effect::Flush(x, y, w, h));
        assert_eq!(c.pixel(x, y), Rgb(1, 2, 3));
        assert_eq!(c.pixel(x + w - 1, y + h - 1), Rgb(1, 2, 3));
        assert_eq!(c.pixel(x + w, y), Rgb(0, 0, 0));
    }

    #[test]
    fn key_image_is_centred() {
        let (mut c, keys) = (canvas(), GRID.keys());
        let r = key_for_index(&keys, 0).unwrap().rect;
        let cmd = Command::SetKeyImage { index: 0, format: PixelFormat::Rgb888, w: 2, h: 2, pixels: vec![255; 12] };
        let effect = apply(&mut c, &keys, &cmd).unwrap();
        let (cx, cy) = (r.x + (r.w - 2) / 2, r.y + (r.h - 2) / 2);
        let expected = Screen::new(&mut c).fb_region(cx, cy, 2, 2);
        assert_eq!(effect, Effect::Flush(expected.0, expected.1, expected.2, expected.3));
        assert_eq!(c.pixel(expected.0, expected.1), Rgb(255, 255, 255));
    }

    #[test]
    fn rejects_unknown_key_and_oversized_image() {
        let (mut c, keys) = (canvas(), GRID.keys());
        let fill = Command::FillKey { index: 99, color: Rgb(0, 0, 0) };
        assert_eq!(apply(&mut c, &keys, &fill), Err("unknown key 99".into()));
        let big = Command::SetKeyImage { index: 9, format: PixelFormat::Rgb565, w: 197, h: 1, pixels: vec![0; 394] };
        assert!(apply(&mut c, &keys, &big).unwrap_err().contains("larger than key 9"));
    }

    #[test]
    fn clear_and_brightness_effects() {
        let (mut c, keys) = (canvas(), GRID.keys());
        assert_eq!(apply(&mut c, &keys, &Command::Clear(Rgb(9, 9, 9))), Ok(Effect::FlushAll));
        assert_eq!(c.pixel(719, 1279), Rgb(9, 9, 9));
        assert_eq!(apply(&mut c, &keys, &Command::SetBrightness(128)), Ok(Effect::Brightness(128)));
        assert_eq!(apply(&mut c, &keys, &Command::SysInfo), Ok(Effect::None));
    }
}
