//! In-memory XRGB8888 pixel buffer laid out like the D200 framebuffer.

use crate::font;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rgb(pub u8, pub u8, pub u8);

pub struct Canvas {
    pub width: usize,
    pub height: usize,
    pub stride: usize,
    pub buf: Vec<u8>,
}

impl Canvas {
    pub fn new(width: usize, height: usize, stride: usize) -> Self {
        assert!(stride >= width * 4, "stride too small for 32 bpp");
        Canvas { width, height, stride, buf: vec![0; stride * height] }
    }

    pub fn set_pixel(&mut self, x: usize, y: usize, c: Rgb) {
        if x >= self.width || y >= self.height {
            return;
        }
        let i = y * self.stride + x * 4;
        self.buf[i..i + 4].copy_from_slice(&[c.2, c.1, c.0, 0]);
    }

    pub fn pixel(&self, x: usize, y: usize) -> Rgb {
        let i = y * self.stride + x * 4;
        Rgb(self.buf[i + 2], self.buf[i + 1], self.buf[i])
    }

    /// Fill a rectangle, clipped to the canvas.
    pub fn fill_rect(&mut self, x: usize, y: usize, w: usize, h: usize, c: Rgb) {
        let x_end = (x + w).min(self.width);
        let y_end = (y + h).min(self.height);
        for yy in y.min(y_end)..y_end {
            for xx in x.min(x_end)..x_end {
                self.set_pixel(xx, yy, c);
            }
        }
    }

    pub fn clear(&mut self, c: Rgb) {
        self.fill_rect(0, 0, self.width, self.height, c);
    }

    /// Draw a decimal number with the built-in digit font; returns the drawn width.
    pub fn draw_number(&mut self, x: usize, y: usize, n: usize, scale: usize, c: Rgb) -> usize {
        font::for_each_cell(n, |col, row| {
            self.fill_rect(x + col * scale, y + row * scale, scale, scale, c)
        });
        font::number_width(n) * scale
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pixel_is_stored_as_bgrx() {
        let mut c = Canvas::new(2, 2, 8);
        c.set_pixel(1, 1, Rgb(0x11, 0x22, 0x33));
        assert_eq!(&c.buf[12..16], &[0x33, 0x22, 0x11, 0]);
        assert_eq!(c.pixel(1, 1), Rgb(0x11, 0x22, 0x33));
    }

    #[test]
    fn fill_rect_is_clipped() {
        let mut c = Canvas::new(4, 4, 16);
        c.fill_rect(2, 2, 10, 10, Rgb(255, 0, 0));
        assert_eq!(c.pixel(3, 3), Rgb(255, 0, 0));
        assert_eq!(c.pixel(1, 1), Rgb(0, 0, 0));
    }

    #[test]
    fn draw_number_advances_per_digit() {
        let mut c = Canvas::new(64, 16, 256);
        let w = c.draw_number(0, 0, 720, 2, Rgb(255, 255, 255));
        assert_eq!(w, 3 * (font::WIDTH + 1) * 2);
        // Top row of '7' is fully set.
        assert_eq!(c.pixel(0, 0), Rgb(255, 255, 255));
    }
}
