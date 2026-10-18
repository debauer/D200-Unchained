//! Panel backlight via sysfs (`/sys/class/backlight/<name>`).

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

pub const DEFAULT_PATH: &str = "/sys/class/backlight/backlight";

pub struct Backlight {
    dir: PathBuf,
    pub max: u32,
}

fn read_u32(path: &Path) -> io::Result<u32> {
    let s = fs::read_to_string(path)
        .map_err(|e| io::Error::new(e.kind(), format!("{}: {e}", path.display())))?;
    s.trim().parse().map_err(|_| {
        io::Error::new(io::ErrorKind::InvalidData, format!("{}: not a number: '{}'", path.display(), s.trim()))
    })
}

impl Backlight {
    pub fn open(dir: &Path) -> io::Result<Self> {
        let max = read_u32(&dir.join("max_brightness"))?;
        Ok(Backlight { dir: dir.to_path_buf(), max })
    }

    pub fn get(&self) -> io::Result<u32> {
        read_u32(&self.dir.join("brightness"))
    }

    /// Set the brightness; values above `max` are rejected.
    pub fn set(&self, value: u32) -> io::Result<()> {
        if value > self.max {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("brightness {value} exceeds max {}", self.max),
            ));
        }
        let path = self.dir.join("brightness");
        fs::write(&path, value.to_string())
            .map_err(|e| io::Error::new(e.kind(), format!("{}: {e}", path.display())))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fake_dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("d200-bl-{name}-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("max_brightness"), "255\n").unwrap();
        fs::write(dir.join("brightness"), "10\n").unwrap();
        dir
    }

    #[test]
    fn reads_and_sets_brightness() {
        let dir = fake_dir("set");
        let bl = Backlight::open(&dir).unwrap();
        assert_eq!((bl.max, bl.get().unwrap()), (255, 10));
        bl.set(255).unwrap();
        assert_eq!(bl.get().unwrap(), 255);
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn rejects_values_above_max() {
        let dir = fake_dir("max");
        let bl = Backlight::open(&dir).unwrap();
        assert!(bl.set(256).is_err());
        assert_eq!(bl.get().unwrap(), 10);
        fs::remove_dir_all(dir).unwrap();
    }
}
