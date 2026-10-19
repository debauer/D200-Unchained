//! Pixel run-length coding of `SET_KEY_IMAGE` format 2 (docs/protocol.md): RGB565 pixels as a sequence of
//! control bytes. `0..=127`: the next `c + 1` pixels follow as they are; `128..=255`: the next pixel is
//! repeated `c - 126` times (2..=129).

/// Decodes `src` into exactly `pixels` RGB565 pixels (`2 * pixels` bytes).
pub fn decode(src: &[u8], pixels: usize) -> Result<Vec<u8>, String> {
    let mut out = Vec::with_capacity(pixels * 2);
    let mut i = 0;
    while i < src.len() {
        let c = src[i] as usize;
        i += 1;
        let (count, literal) = if c < 128 { (c + 1, true) } else { (c - 126, false) };
        let need = if literal { count * 2 } else { 2 };
        if i + need > src.len() {
            return Err("RLE data ends inside a run".into());
        }
        if out.len() + count * 2 > pixels * 2 {
            return Err(format!("RLE data has more than the {pixels} pixels of the image"));
        }
        if literal {
            out.extend_from_slice(&src[i..i + need]);
        } else {
            for _ in 0..count {
                out.extend_from_slice(&src[i..i + 2]);
            }
        }
        i += need;
    }
    if out.len() != pixels * 2 {
        return Err(format!("RLE data has {} of the {pixels} pixels", out.len() / 2));
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn literal_and_repeat_runs() {
        // 3 literal pixels, then one pixel repeated 5 times
        let src = [2, 1, 0, 2, 0, 3, 0, 131, 9, 9];
        assert_eq!(decode(&src, 8).unwrap(), [1, 0, 2, 0, 3, 0, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9]);
    }

    #[test]
    fn longest_runs() {
        assert_eq!(decode(&[255, 7, 7], 129).unwrap().len(), 258);
        let literal: Vec<u8> = std::iter::once(127).chain((0..256).map(|i| i as u8)).collect();
        assert_eq!(decode(&literal, 128).unwrap().len(), 256);
    }

    #[test]
    fn rejects_wrong_lengths() {
        assert!(decode(&[130, 1], 4).unwrap_err().contains("ends inside"));
        assert!(decode(&[130, 1, 1], 5).unwrap_err().contains("of the 5 pixels"));
        assert!(decode(&[130, 1, 1], 3).unwrap_err().contains("more than"));
        assert!(decode(&[], 1).is_err());
    }
}
