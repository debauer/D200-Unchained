//! Landscape drawing on the portrait-mounted panel.
//!
//! The panel is 720x1280 in framebuffer space and mounted rotated by 90 degrees:
//! landscape (x, y) maps to framebuffer (y, fb_height - 1 - x).

use crate::canvas::{Canvas, Rgb};
use crate::font;

pub struct Screen<'a> {
    canvas: &'a mut Canvas,
}

impl<'a> Screen<'a> {
    pub fn new(canvas: &'a mut Canvas) -> Self {
        Screen { canvas }
    }

    pub fn width(&self) -> usize {
        self.canvas.height
    }

    pub fn height(&self) -> usize {
        self.canvas.width
    }

    pub fn set_pixel(&mut self, x: usize, y: usize, c: Rgb) {
        if x < self.width() && y < self.height() {
            let fb_y = self.canvas.height - 1 - x;
            self.canvas.set_pixel(y, fb_y, c);
        }
    }

    /// Fill a landscape rectangle, clipped to the screen.
    pub fn fill_rect(&mut self, x: usize, y: usize, w: usize, h: usize, c: Rgb) {
        let x_end = (x + w).min(self.width());
        let y_end = (y + h).min(self.height());
        if x >= x_end || y >= y_end {
            return;
        }
        let fb_y = self.canvas.height - x_end;
        self.canvas.fill_rect(y, fb_y, y_end - y, x_end - x, c);
    }

    /// Draw a 1px-wide rectangle outline.
    pub fn outline(&mut self, x: usize, y: usize, w: usize, h: usize, c: Rgb) {
        self.fill_rect(x, y, w, 1, c);
        self.fill_rect(x, y + h - 1, w, 1, c);
        self.fill_rect(x, y, 1, h, c);
        self.fill_rect(x + w - 1, y, 1, h, c);
    }

    pub fn draw_number(&mut self, x: usize, y: usize, n: usize, scale: usize, c: Rgb) {
        font::for_each_cell(n, |col, row| {
            self.fill_rect(x + col * scale, y + row * scale, scale, scale, c)
        });
    }

    /// Draw a w x h image at (x, y); `pixel(col, row)` yields the colour, clipped to the screen.
    pub fn blit(&mut self, x: usize, y: usize, w: usize, h: usize, pixel: impl Fn(usize, usize) -> Rgb) {
        for row in 0..h {
            for col in 0..w {
                self.set_pixel(x + col, y + row, pixel(col, row));
            }
        }
    }

    /// Framebuffer region (x, y, w, h) covered by a landscape rectangle, clipped.
    pub fn fb_region(&self, x: usize, y: usize, w: usize, h: usize) -> (usize, usize, usize, usize) {
        let x_end = (x + w).min(self.width());
        let y_end = (y + h).min(self.height());
        if x >= x_end || y >= y_end {
            return (0, 0, 0, 0);
        }
        (y, self.canvas.height - x_end, y_end - y, x_end - x)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const RED: Rgb = Rgb(255, 0, 0);

    #[test]
    fn corners_map_like_the_photo() {
        let mut c = Canvas::new(720, 1280, 2880);
        let mut s = Screen::new(&mut c);
        assert_eq!((s.width(), s.height()), (1280, 720));
        s.set_pixel(1279, 0, RED); // landscape top right = fb (0, 0)
        s.set_pixel(0, 0, Rgb(0, 0, 255)); // landscape top left = fb (0, 1279)
        s.set_pixel(0, 719, Rgb(255, 255, 255)); // landscape bottom left = fb (719, 1279)
        assert_eq!(c.pixel(0, 0), RED);
        assert_eq!(c.pixel(0, 1279), Rgb(0, 0, 255));
        assert_eq!(c.pixel(719, 1279), Rgb(255, 255, 255));
    }

    #[test]
    fn fill_rect_matches_set_pixel() {
        let mut a = Canvas::new(720, 1280, 2880);
        let mut b = Canvas::new(720, 1280, 2880);
        Screen::new(&mut a).fill_rect(1200, 700, 100, 50, RED);
        let mut s = Screen::new(&mut b);
        for y in 700..720 {
            for x in 1200..1280 {
                s.set_pixel(x, y, RED);
            }
        }
        assert!(a.buf == b.buf);
    }

    #[test]
    fn blit_places_image_upright() {
        let mut c = Canvas::new(720, 1280, 2880);
        let mut s = Screen::new(&mut c);
        // 2x1 image: left red, right green, drawn at landscape (10, 20).
        s.blit(10, 20, 2, 1, |col, _| if col == 0 { RED } else { Rgb(0, 255, 0) });
        // landscape (10, 20) -> fb (20, 1269); (11, 20) -> fb (20, 1268)
        assert_eq!(c.pixel(20, 1269), RED);
        assert_eq!(c.pixel(20, 1268), Rgb(0, 255, 0));
    }

    #[test]
    fn fb_region_covers_the_drawn_pixels() {
        let mut c = Canvas::new(720, 1280, 2880);
        let s = Screen::new(&mut c);
        assert_eq!(s.fb_region(10, 20, 30, 40), (20, 1240, 40, 30));
        assert_eq!(s.fb_region(1270, 0, 50, 5), (0, 0, 5, 10)); // clipped at the right edge
        assert_eq!(s.fb_region(2000, 0, 5, 5), (0, 0, 0, 0));
    }
}
