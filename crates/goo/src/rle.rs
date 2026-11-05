//! Layer image codec, written from ELEGOO's "Goo Format Spec V1.2".
//!
//! An encoded layer is: the magic byte `0x55`, a sequence of chunks, then one
//! checksum byte. The checksum is the bitwise NOT of the wrapping 8-bit sum of
//! every chunk byte (the magic byte is excluded). Pixels run row by row,
//! left to right, over the whole LCD.
//!
//! Chunk byte 0, bits 7..6 give the chunk type:
//!
//! | bits | meaning                                                  |
//! |------|----------------------------------------------------------|
//! | `00` | run of `0x00` pixels                                     |
//! | `01` | run of a grey value `0x01..=0xFE`, stored in the next byte|
//! | `10` | value differs from the previous pixel by a 4-bit delta   |
//! | `11` | run of `0xFF` pixels                                     |
//!
//! For types `00`, `01` and `11`, bits 5..4 say how many extra length bytes
//! follow (0 to 3). The run length is those bytes, most significant first,
//! followed by bits 3..0 of byte 0 as the lowest nibble. So the longest run in
//! one chunk is 28 bits.
//!
//! For type `10`, bit 5 is the sign (0 = add, 1 = subtract), bits 3..0 are the
//! delta, and bit 4 says whether a one-byte run length follows (otherwise the
//! run is a single pixel). The encoder here never writes type `10` chunks;
//! the decoder accepts them.

use crate::Error;

/// First byte of every encoded layer.
pub const LAYER_MAGIC: u8 = 0x55;

/// Longest run a single chunk can hold (24 bits of extra bytes + 4 bits).
pub const MAX_RUN: u64 = (1 << 28) - 1;

/// Streams pixel runs into the encoded form. Adjacent runs with the same
/// value are merged, so callers can push one run per scanline span.
#[derive(Debug)]
pub struct LayerEncoder {
    out: Vec<u8>,
    sum: u8,
    pending: Option<(u8, u64)>,
    pixels: u64,
}

impl Default for LayerEncoder {
    fn default() -> Self {
        Self::new()
    }
}

impl LayerEncoder {
    pub fn new() -> Self {
        Self {
            out: vec![LAYER_MAGIC],
            sum: 0,
            pending: None,
            pixels: 0,
        }
    }

    /// Adds `len` pixels of `value`. A zero length is ignored.
    pub fn push_run(&mut self, value: u8, len: u64) {
        if len == 0 {
            return;
        }
        self.pixels += len;
        match &mut self.pending {
            Some((v, l)) if *v == value => *l += len,
            _ => {
                if let Some((v, l)) = self.pending.take() {
                    self.emit(v, l);
                }
                self.pending = Some((value, len));
            }
        }
    }

    /// Adds a row (or any slice) of raw 8-bit pixels.
    pub fn push_pixels(&mut self, pixels: &[u8]) {
        let mut i = 0;
        while i < pixels.len() {
            let v = pixels[i];
            let start = i;
            while i < pixels.len() && pixels[i] == v {
                i += 1;
            }
            self.push_run(v, (i - start) as u64);
        }
    }

    /// Total pixels pushed so far.
    pub fn pixel_count(&self) -> u64 {
        self.pixels
    }

    /// Finishes the layer: flushes the last run and appends the checksum.
    /// The returned bytes include the magic byte and checksum, which is what
    /// the layer's "data size" field counts.
    pub fn finish(mut self) -> Vec<u8> {
        if let Some((v, l)) = self.pending.take() {
            self.emit(v, l);
        }
        self.out.push(!self.sum);
        self.out
    }

    fn push_byte(&mut self, b: u8) {
        self.sum = self.sum.wrapping_add(b);
        self.out.push(b);
    }

    fn emit(&mut self, value: u8, mut len: u64) {
        while len > 0 {
            let chunk = len.min(MAX_RUN);
            self.emit_chunk(value, chunk);
            len -= chunk;
        }
    }

    fn emit_chunk(&mut self, value: u8, len: u64) {
        debug_assert!(len > 0 && len <= MAX_RUN);
        let kind: u8 = match value {
            0x00 => 0b00,
            0xFF => 0b11,
            _ => 0b01,
        };
        let extra: u8 = match len {
            0..=0xF => 0,
            0x10..=0xFFF => 1,
            0x1000..=0xF_FFFF => 2,
            _ => 3,
        };
        self.push_byte((kind << 6) | (extra << 4) | (len & 0xF) as u8);
        if kind == 0b01 {
            self.push_byte(value);
        }
        // Extra length bytes, most significant first, above the low nibble.
        for i in (0..extra).rev() {
            self.push_byte((len >> (4 + 8 * i as u64)) as u8);
        }
    }
}

/// Checks the magic byte and checksum, then calls `f(value, run_length)` for
/// every run in order. Returns the total number of pixels.
pub fn decode_runs(data: &[u8], mut f: impl FnMut(u8, u64)) -> Result<u64, Error> {
    if data.len() < 2 {
        return Err(Error::Rle(
            "layer data shorter than magic + checksum".into(),
        ));
    }
    if data[0] != LAYER_MAGIC {
        return Err(Error::Rle(format!(
            "layer magic is {:#04x}, expected 0x55",
            data[0]
        )));
    }
    let body = &data[1..data.len() - 1];
    let stored = data[data.len() - 1];
    let computed = !body.iter().fold(0u8, |s, &b| s.wrapping_add(b));
    if stored != computed {
        return Err(Error::Rle(format!(
            "checksum is {stored:#04x}, computed {computed:#04x}"
        )));
    }

    let mut i = 0;
    let mut prev: u8 = 0;
    let mut total = 0u64;
    let byte = |i: usize| -> Result<u8, Error> {
        body.get(i)
            .copied()
            .ok_or_else(|| Error::Rle("chunk runs past end of layer data".into()))
    };
    while i < body.len() {
        let head = body[i];
        i += 1;
        let kind = head >> 6;
        let (value, len) = if kind == 0b10 {
            let delta = head & 0x0F;
            let value = if head & 0x20 == 0 {
                prev.checked_add(delta)
            } else {
                prev.checked_sub(delta)
            }
            .ok_or_else(|| Error::Rle("diff chunk overflows a pixel value".into()))?;
            let len = if head & 0x10 != 0 {
                i += 1;
                byte(i - 1)? as u64
            } else {
                1
            };
            (value, len)
        } else {
            let value = match kind {
                0b00 => 0x00,
                0b11 => 0xFF,
                _ => {
                    i += 1;
                    byte(i - 1)?
                }
            };
            let extra = (head >> 4) & 0x3;
            let mut len = 0u64;
            for _ in 0..extra {
                len = (len << 8) | byte(i)? as u64;
                i += 1;
            }
            (value, (len << 4) | (head & 0xF) as u64)
        };
        f(value, len);
        total += len;
        prev = value;
    }
    Ok(total)
}

/// Decodes a layer into a full 8-bit image buffer of `expected_pixels`.
pub fn decode_to_vec(data: &[u8], expected_pixels: u64) -> Result<Vec<u8>, Error> {
    let mut out = Vec::with_capacity(expected_pixels as usize);
    let total = decode_runs(data, |v, l| out.extend(std::iter::repeat_n(v, l as usize)))?;
    if total != expected_pixels {
        return Err(Error::Rle(format!(
            "layer has {total} pixels, expected {expected_pixels}"
        )));
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn body(encoded: &[u8]) -> &[u8] {
        &encoded[1..encoded.len() - 1]
    }

    // Byte examples from the spec's table.
    #[test]
    fn spec_example_black_run() {
        let mut e = LayerEncoder::new();
        e.push_run(0x00, 0x5);
        assert_eq!(body(&e.finish()), &[0b0000_0101]);
    }

    #[test]
    fn spec_example_black_long_run() {
        let mut e = LayerEncoder::new();
        e.push_run(0x00, 0x555657F);
        assert_eq!(body(&e.finish()), &[0b0011_1111, 0x55, 0x56, 0x57]);
    }

    #[test]
    fn spec_example_grey_run() {
        let mut e = LayerEncoder::new();
        e.push_run(0xAA, 0xBBCC155);
        assert_eq!(body(&e.finish()), &[0b0111_0101, 0xAA, 0xBB, 0xCC, 0x15]);
    }

    #[test]
    fn spec_example_diff_chunks_decode() {
        // 0xff x1, then +1 is invalid past 0xff, so start from grey 0x10.
        // grey 0x10 x1, +1 x1 (0x11), +2 x0xff (0x13), -1 x1 (0x12), -2 x0xee (0x10)
        let chunks = [
            0b0100_0001,
            0x10,
            0b1000_0001,
            0b1001_0010,
            0xFF,
            0b1010_0001,
            0b1011_0010,
            0xEE,
        ];
        let mut data = vec![LAYER_MAGIC];
        data.extend_from_slice(&chunks);
        data.push(!chunks.iter().fold(0u8, |s, &b| s.wrapping_add(b)));
        let mut runs = vec![];
        decode_runs(&data, |v, l| runs.push((v, l))).unwrap();
        assert_eq!(
            runs,
            vec![(0x10, 1), (0x11, 1), (0x13, 0xFF), (0x12, 1), (0x10, 0xEE)]
        );
    }

    #[test]
    fn merges_and_splits_runs() {
        let mut e = LayerEncoder::new();
        e.push_run(0xFF, 10);
        e.push_run(0xFF, 10);
        e.push_run(0x00, MAX_RUN + 5);
        let enc = e.finish();
        let mut runs = vec![];
        decode_runs(&enc, |v, l| runs.push((v, l))).unwrap();
        assert_eq!(runs, vec![(0xFF, 20), (0x00, MAX_RUN), (0x00, 5)]);
    }

    #[test]
    fn round_trip_pixels() {
        let px: Vec<u8> = (0..10_000u32).map(|i| ((i / 37) % 256) as u8).collect();
        let mut e = LayerEncoder::new();
        e.push_pixels(&px);
        let enc = e.finish();
        assert_eq!(decode_to_vec(&enc, px.len() as u64).unwrap(), px);
    }

    #[test]
    fn rejects_bad_checksum() {
        let mut e = LayerEncoder::new();
        e.push_run(0, 100);
        let mut enc = e.finish();
        *enc.last_mut().unwrap() ^= 1;
        assert!(decode_runs(&enc, |_, _| {}).is_err());
    }
}
