//! Physical key layout in landscape screen coordinates (see `screen`).
//! The windows form a regular grid; the default values were measured on the device
//! with the interactive `calibrate` tool.

pub const WIDTH: usize = 1280;
pub const HEIGHT: usize = 720;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rect {
    pub x: usize,
    pub y: usize,
    pub w: usize,
    pub h: usize,
}

#[derive(Clone, Copy, Debug)]
pub struct Key {
    pub index: u16,
    pub code: u16,
    pub rect: Rect,
}

/// Regular window grid: top-left window at (x0, y0), square windows of `size`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Grid {
    pub x0: usize,
    pub y0: usize,
    pub pitch_x: usize,
    pub pitch_y: usize,
    pub size: usize,
}

pub const GRID: Grid = Grid { x0: 11, y0: 3, pitch_x: 260, pitch_y: 259, size: 196 };

/// Keycodes by (row, column); 0 = no key. Key 30 spans columns 3 and 4.
const CODES: [[u16; 5]; 3] = [[29, 15, 14, 13, 12], [11, 10, 9, 8, 7], [34, 33, 31, 30, 0]];
const WIDE_KEY: u16 = 30;

impl Grid {
    pub fn keys(&self) -> Vec<Key> {
        let mut keys = Vec::with_capacity(14);
        for (row, codes) in CODES.iter().enumerate() {
            for (col, &code) in codes.iter().enumerate() {
                if code == 0 {
                    continue;
                }
                let w = if code == WIDE_KEY { self.pitch_x + self.size } else { self.size };
                let rect = Rect {
                    x: self.x0 + col * self.pitch_x,
                    y: self.y0 + row * self.pitch_y,
                    w,
                    h: self.size,
                };
                keys.push(Key { index: keys.len() as u16, code, rect });
            }
        }
        keys
    }

    /// Check that all windows lie on screen and do not overlap; the error says why not.
    pub fn validate(&self) -> Result<(), String> {
        if self.size == 0 {
            return Err("size must be > 0".into());
        }
        if self.pitch_x <= self.size {
            return Err(format!("pitch_x {} must exceed size {} (windows would touch)", self.pitch_x, self.size));
        }
        if self.pitch_y <= self.size {
            return Err(format!("pitch_y {} must exceed size {} (windows would touch)", self.pitch_y, self.size));
        }
        let right = self.keys().iter().map(|k| k.rect.x + k.rect.w).max().unwrap_or(0);
        if right > WIDTH {
            return Err(format!("right column would end at x={right} > {WIDTH}"));
        }
        let bottom = self.keys().iter().map(|k| k.rect.y + k.rect.h).max().unwrap_or(0);
        if bottom > HEIGHT {
            return Err(format!("bottom row would end at y={bottom} > {HEIGHT}"));
        }
        Ok(())
    }

    pub fn is_valid(&self) -> bool {
        self.validate().is_ok()
    }
}

pub fn key_for_code(keys: &[Key], code: u16) -> Option<&Key> {
    keys.iter().find(|k| k.code == code)
}

pub fn key_for_index(keys: &[Key], index: u16) -> Option<&Key> {
    keys.iter().find(|k| k.index == index)
}

pub fn index_for_code(keys: &[Key], code: u16) -> Option<u16> {
    keys.iter().find(|k| k.code == code).map(|k| k.index)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_grid_is_valid_and_disjoint() {
        assert!(GRID.is_valid());
        let keys = GRID.keys();
        assert_eq!(keys.len(), 14);
        for (i, a) in keys.iter().enumerate() {
            for b in &keys[i + 1..] {
                assert_ne!(a.code, b.code);
                let (p, q) = (a.rect, b.rect);
                let overlap = p.x < q.x + q.w && q.x < p.x + p.w && p.y < q.y + q.h && q.y < p.y + p.h;
                assert!(!overlap, "keys {} and {} overlap", a.code, b.code);
            }
        }
    }

    #[test]
    fn wide_key_spans_two_columns() {
        let keys = GRID.keys();
        let wide = key_for_code(&keys, 30).unwrap().rect;
        let right_col = key_for_code(&keys, 7).unwrap().rect;
        assert_eq!(wide.x + wide.w, right_col.x + right_col.w);
        assert!(key_for_code(&keys, 16).is_none());
    }

    #[test]
    fn indices_follow_physical_order_and_map_to_codes() {
        let keys = GRID.keys();
        let codes: Vec<u16> = keys.iter().map(|k| k.code).collect();
        assert_eq!(codes, [29, 15, 14, 13, 12, 11, 10, 9, 8, 7, 34, 33, 31, 30]);
        for (i, k) in keys.iter().enumerate() {
            assert_eq!(k.index, i as u16);
            assert_eq!(key_for_index(&keys, i as u16).unwrap().code, k.code);
            assert_eq!(index_for_code(&keys, k.code), Some(i as u16));
        }
    }

    #[test]
    fn rejects_off_screen_grid() {
        assert!(!Grid { x0: 100, ..GRID }.is_valid());
        assert!(!Grid { pitch_x: GRID.size, ..GRID }.is_valid());
        let full_height = Grid { x0: 5, y0: 1, pitch_x: 266, pitch_y: 262, size: 196 };
        assert_eq!(
            full_height.validate(),
            Err("bottom row would end at y=721 > 720".into())
        );
    }
}
