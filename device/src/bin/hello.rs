//! Phase-2 test: draw a coordinate grid on the panel and print key events.
//! Usage: hello [/dev/fb0] [/dev/input/event0]

use std::env;
use std::path::PathBuf;
use std::process::ExitCode;

use d200_device::canvas::{Canvas, Rgb};
use d200_device::fb::Framebuffer;
use d200_device::input::Keys;

const GRID: usize = 40;
const LABEL_EVERY: usize = 80;
const CORNER: usize = 60;

fn draw_pattern(c: &mut Canvas) {
    let (w, h) = (c.width, c.height);
    c.clear(Rgb(20, 20, 20));
    for x in (0..w).step_by(GRID) {
        let col = if x % LABEL_EVERY == 0 { Rgb(140, 140, 140) } else { Rgb(60, 60, 60) };
        c.fill_rect(x, 0, 1, h, col);
    }
    for y in (0..h).step_by(GRID) {
        let col = if y % LABEL_EVERY == 0 { Rgb(140, 140, 140) } else { Rgb(60, 60, 60) };
        c.fill_rect(0, y, w, 1, col);
    }
    // Corner markers identify the panel orientation in fb coordinates.
    c.fill_rect(0, 0, CORNER, CORNER, Rgb(255, 0, 0));
    c.fill_rect(w - CORNER, 0, CORNER, CORNER, Rgb(0, 255, 0));
    c.fill_rect(0, h - CORNER, CORNER, CORNER, Rgb(0, 0, 255));
    c.fill_rect(w - CORNER, h - CORNER, CORNER, CORNER, Rgb(255, 255, 255));
    // Coordinate labels: x along every labelled column, y along every labelled row.
    for y in (LABEL_EVERY..h).step_by(LABEL_EVERY) {
        for x in (LABEL_EVERY..w).step_by(LABEL_EVERY) {
            c.draw_number(x + 4, y + 4, x, 2, Rgb(255, 200, 0));
            c.draw_number(x + 4, y + 20, y, 2, Rgb(0, 200, 255));
        }
    }
}

fn run() -> std::io::Result<()> {
    let mut args = env::args().skip(1);
    let fb_path = PathBuf::from(args.next().unwrap_or_else(|| "/dev/fb0".into()));
    let input_path = PathBuf::from(args.next().unwrap_or_else(|| "/dev/input/event0".into()));

    let mut fb = Framebuffer::open(&fb_path)?;
    println!(
        "framebuffer {}: {}x{} stride {}",
        fb_path.display(),
        fb.canvas.width,
        fb.canvas.height,
        fb.canvas.stride
    );
    draw_pattern(&mut fb.canvas);
    fb.flush()?;

    let mut keys = Keys::open(&input_path)?;
    println!("pattern drawn; waiting for keys on {} (Ctrl-C to quit)", input_path.display());
    loop {
        let ev = keys.next()?;
        println!("key {:3} {}", ev.code, if ev.pressed { "down" } else { "up" });
    }
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("error: {e}");
            ExitCode::FAILURE
        }
    }
}
