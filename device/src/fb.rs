//! Linux fbdev output (`/dev/fb0`), geometry read from sysfs.

use std::fs::{self, File, OpenOptions};
use std::io::{self, Seek, SeekFrom, Write};
use std::os::unix::fs::FileExt;
use std::path::Path;

use crate::canvas::Canvas;

pub struct Framebuffer {
    file: File,
    pub canvas: Canvas,
}

fn sysfs(dir: &Path, attr: &str) -> io::Result<String> {
    let path = dir.join(attr);
    fs::read_to_string(&path)
        .map(|s| s.trim().to_string())
        .map_err(|e| io::Error::new(e.kind(), format!("{}: {e}", path.display())))
}

fn bad(msg: String) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg)
}

impl Framebuffer {
    /// Open `/dev/fbN` with geometry from `/sys/class/graphics/fbN`.
    pub fn open(dev: &Path) -> io::Result<Self> {
        let name = dev
            .file_name()
            .ok_or_else(|| bad(format!("invalid framebuffer path {}", dev.display())))?;
        Self::open_with_sysfs(dev, &Path::new("/sys/class/graphics").join(name))
    }

    /// Open a framebuffer device whose geometry attributes live in `sysfs_dir`.
    pub fn open_with_sysfs(dev: &Path, sysfs_dir: &Path) -> io::Result<Self> {
        let size = sysfs(sysfs_dir, "virtual_size")?;
        let (w, h) = size
            .split_once(',')
            .ok_or_else(|| bad(format!("unexpected virtual_size '{size}'")))?;
        let width: usize = w.parse().map_err(|_| bad(format!("bad width '{w}'")))?;
        let height: usize = h.parse().map_err(|_| bad(format!("bad height '{h}'")))?;
        let bpp = sysfs(sysfs_dir, "bits_per_pixel")?;
        if bpp != "32" {
            return Err(bad(format!("unsupported bits_per_pixel {bpp}, expected 32")));
        }
        let stride: usize = sysfs(sysfs_dir, "stride")?
            .parse()
            .map_err(|_| bad("bad stride".into()))?;
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(dev)
            .map_err(|e| io::Error::new(e.kind(), format!("{}: {e}", dev.display())))?;
        Ok(Framebuffer { file, canvas: Canvas::new(width, height, stride) })
    }

    /// Copy the whole canvas to the framebuffer.
    pub fn flush(&mut self) -> io::Result<()> {
        self.file.seek(SeekFrom::Start(0))?;
        self.file.write_all(&self.canvas.buf)
    }

    /// Copy only a framebuffer-space region (x, y, w, h) of the canvas.
    pub fn flush_region(&self, x: usize, y: usize, w: usize, h: usize) -> io::Result<()> {
        let c = &self.canvas;
        assert!(x + w <= c.width && y + h <= c.height, "region outside framebuffer");
        for row in y..y + h {
            let off = row * c.stride + x * 4;
            self.file.write_all_at(&c.buf[off..off + w * 4], off as u64)?;
        }
        Ok(())
    }
}
