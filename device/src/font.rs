//! Minimal 3x5 digit font for test patterns.

pub const WIDTH: usize = 3;
pub const HEIGHT: usize = 5;

const DIGITS: [[u8; HEIGHT]; 10] = [
    [0b111, 0b101, 0b101, 0b101, 0b111],
    [0b010, 0b110, 0b010, 0b010, 0b111],
    [0b111, 0b001, 0b111, 0b100, 0b111],
    [0b111, 0b001, 0b111, 0b001, 0b111],
    [0b101, 0b101, 0b111, 0b001, 0b001],
    [0b111, 0b100, 0b111, 0b001, 0b111],
    [0b111, 0b100, 0b111, 0b101, 0b111],
    [0b111, 0b001, 0b010, 0b010, 0b010],
    [0b111, 0b101, 0b111, 0b101, 0b111],
    [0b111, 0b101, 0b111, 0b001, 0b111],
];

/// Width of a rendered number in font cells (one blank column per digit).
pub fn number_width(n: usize) -> usize {
    n.to_string().len() * (WIDTH + 1)
}

/// Call `cell(col, row)` for every set cell of `n`, in font-cell units.
pub fn for_each_cell(n: usize, mut cell: impl FnMut(usize, usize)) {
    for (i, ch) in n.to_string().bytes().enumerate() {
        let glyph = &DIGITS[(ch - b'0') as usize];
        for (row, bits) in glyph.iter().enumerate() {
            for col in 0..WIDTH {
                if bits & (1 << (WIDTH - 1 - col)) != 0 {
                    cell(i * (WIDTH + 1) + col, row);
                }
            }
        }
    }
}
