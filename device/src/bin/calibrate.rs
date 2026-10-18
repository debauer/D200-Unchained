//! Interactive key-window calibration.
//!
//! Draws every window of the current grid as a colour ladder (nested outlines every
//! 4 px) and lets the keys adjust the grid until the ladder looks symmetric in every
//! window. The current values are printed after every change.
//! Usage: calibrate [/dev/fb0] [/dev/input/event0]

use std::env;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use d200_device::backlight::{self, Backlight};
use d200_device::canvas::{Canvas, Rgb};
use d200_device::fb::Framebuffer;
use d200_device::font;
use d200_device::input::Keys;
use d200_device::layout::{Grid, Key, GRID};
use d200_device::screen::Screen;

const LABEL_SCALE: usize = 8;

/// Nested 1px outlines (inset in px, colour): the first colour visible on an edge
/// tells how far the window edge sits inside the rectangle.
const RINGS: [(usize, Rgb); 8] = [
    (0, Rgb(255, 255, 255)),  // white
    (4, Rgb(255, 0, 0)),      // red
    (8, Rgb(255, 140, 0)),    // orange
    (12, Rgb(255, 255, 0)),   // yellow
    (16, Rgb(0, 255, 0)),     // green
    (20, Rgb(0, 255, 255)),   // cyan
    (24, Rgb(0, 80, 255)),    // blue
    (28, Rgb(255, 0, 255)),   // magenta
];

/// Key bindings in physical order (row by row), used for help and per-press output.
const ACTIONS: [(u16, &str); 14] = [
    (29, "grid left (x0-)"),
    (15, "grid right (x0+)"),
    (14, "grid up (y0-)"),
    (13, "grid down (y0+)"),
    (12, "toggle step 1/5 px"),
    (11, "column pitch - (pitch_x-)"),
    (10, "column pitch + (pitch_x+)"),
    (9, "row pitch - (pitch_y-)"),
    (8, "row pitch + (pitch_y+)"),
    (7, "unbound"),
    (34, "window size - (size-)"),
    (33, "window size + (size+)"),
    (31, "reset to defaults"),
    (30, "print values"),
];

fn action_name(code: u16) -> &'static str {
    ACTIONS.iter().find(|(c, _)| *c == code).map_or("not in layout", |(_, name)| name)
}

fn draw_key(s: &mut Screen, k: &Key, pressed: bool) {
    let r = k.rect;
    let bg = if pressed { Rgb(0, 60, 140) } else { Rgb(25, 25, 25) };
    s.fill_rect(r.x, r.y, r.w, r.h, bg);
    for (inset, c) in RINGS {
        s.outline(r.x + inset, r.y + inset, r.w - 2 * inset, r.h - 2 * inset, c);
    }
    let n = k.code as usize;
    let tw = font::number_width(n) * LABEL_SCALE;
    let th = font::HEIGHT * LABEL_SCALE;
    s.draw_number(r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2, n, LABEL_SCALE, Rgb(200, 200, 200));
}

fn draw_all(c: &mut Canvas, grid: &Grid, pressed: Option<u16>) {
    let mut s = Screen::new(c);
    let (w, h) = (s.width(), s.height());
    s.fill_rect(0, 0, w, h, Rgb(0, 0, 0));
    for k in grid.keys() {
        draw_key(&mut s, &k, pressed == Some(k.code));
    }
}

fn dec(v: usize, step: usize, name: &str) -> Result<usize, String> {
    v.checked_sub(step).ok_or_else(|| format!("{name} is already {v}, cannot go below 0"))
}

/// Apply the action bound to `code`; the error explains why nothing changed.
fn adjust(grid: &Grid, code: u16, step: usize) -> Result<Grid, String> {
    let mut g = *grid;
    match code {
        29 => g.x0 = dec(g.x0, step, "x0")?,
        15 => g.x0 += step,
        14 => g.y0 = dec(g.y0, step, "y0")?,
        13 => g.y0 += step,
        11 => g.pitch_x = dec(g.pitch_x, step, "pitch_x")?,
        10 => g.pitch_x += step,
        9 => g.pitch_y = dec(g.pitch_y, step, "pitch_y")?,
        8 => g.pitch_y += step,
        34 => g.size = dec(g.size, step, "size")?,
        33 => g.size += step,
        31 if g == GRID => return Err("already at defaults".into()),
        31 => g = GRID,
        _ => return Err("key has no function".into()),
    }
    g.validate()?;
    Ok(g)
}

fn print_grid(g: &Grid, step: usize) {
    println!(
        "x0={} y0={} pitch_x={} pitch_y={} size={}  (step {step})",
        g.x0, g.y0, g.pitch_x, g.pitch_y, g.size
    );
}

fn run() -> std::io::Result<()> {
    let mut args = env::args().skip(1);
    let fb_path = PathBuf::from(args.next().unwrap_or_else(|| "/dev/fb0".into()));
    let input_path = PathBuf::from(args.next().unwrap_or_else(|| "/dev/input/event0".into()));

    let bl = Backlight::open(Path::new(backlight::DEFAULT_PATH))?;
    bl.set(bl.max)?;
    println!("backlight set to {}", bl.max);

    let mut grid = GRID;
    let mut step = 1;
    let mut fb = Framebuffer::open(&fb_path)?;
    draw_all(&mut fb.canvas, &grid, None);
    fb.flush()?;

    let mut keys = Keys::open(&input_path)?;
    for (code, name) in ACTIONS {
        println!("  key {code:2}: {name}");
    }
    print_grid(&grid, step);
    loop {
        let ev = keys.next()?;
        if ev.pressed {
            println!("key {}: {}", ev.code, action_name(ev.code));
            match ev.code {
                12 => step = if step == 1 { 5 } else { 1 },
                30 => {}
                code => match adjust(&grid, code, step) {
                    Ok(g) => grid = g,
                    Err(why) => println!("  no change: {why}"),
                },
            }
            print_grid(&grid, step);
        }
        draw_all(&mut fb.canvas, &grid, ev.pressed.then_some(ev.code));
        fb.flush()?;
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn adjust_moves_and_resets() {
        let g = adjust(&GRID, 15, 1).unwrap();
        assert_eq!(g.x0, GRID.x0 + 1);
        assert_eq!(adjust(&g, 31, 1), Ok(GRID));
    }

    #[test]
    fn adjust_explains_rejections() {
        // Fills the panel height exactly: 2 * 262 + 196 = 720.
        let at_top = Grid { x0: 5, y0: 0, pitch_x: 266, pitch_y: 262, size: 196 };
        assert_eq!(adjust(&at_top, 14, 1), Err("y0 is already 0, cannot go below 0".into()));
        assert_eq!(adjust(&at_top, 13, 1), Err("bottom row would end at y=721 > 720".into()));
        assert_eq!(adjust(&GRID, 7, 1), Err("key has no function".into()));
        let big = Grid { size: GRID.pitch_y - 1, ..GRID };
        assert!(adjust(&big, 33, 1).unwrap_err().contains("windows would touch"));
    }

    #[test]
    fn every_layout_key_has_an_action_name() {
        for k in GRID.keys() {
            assert_ne!(action_name(k.code), "not in layout", "key {}", k.code);
        }
        assert_eq!(action_name(99), "not in layout");
    }
}
