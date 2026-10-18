//! deckd: display/key service for the D200 (protocol in docs/protocol.md).
//! Usage: deckd [listen-addr]   (default 127.0.0.1:7200)
//! Device paths can be overridden for testing with DECKD_FB, DECKD_FB_SYSFS,
//! DECKD_INPUT and DECKD_BACKLIGHT.

use std::env;
use std::path::PathBuf;
use std::process::ExitCode;

use d200_device::backlight;
use d200_device::layout::GRID;
use d200_device::protocol::PORT;
use d200_device::server::{self, Config};

fn path(var: &str, default: &str) -> PathBuf {
    PathBuf::from(env::var(var).unwrap_or_else(|_| default.to_string()))
}

fn main() -> ExitCode {
    let cfg = Config {
        fb: path("DECKD_FB", "/dev/fb0"),
        fb_sysfs: path("DECKD_FB_SYSFS", "/sys/class/graphics/fb0"),
        input: path("DECKD_INPUT", "/dev/input/event0"),
        backlight: path("DECKD_BACKLIGHT", backlight::DEFAULT_PATH),
        listen: env::args().nth(1).unwrap_or_else(|| format!("127.0.0.1:{PORT}")),
    };
    match server::run(&cfg, GRID.keys()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("deckd: {e}");
            ExitCode::FAILURE
        }
    }
}
