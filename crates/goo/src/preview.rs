//! Preview thumbnails: RGB565, big-endian, row-major.

use std::io::{Read, Write};

use crate::bytes::{read_u16, write_u16};
use crate::Error;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Preview {
    pub width: u16,
    pub height: u16,
    /// One RGB565 value per pixel, row-major.
    pub pixels: Vec<u16>,
}

impl Preview {
    pub const SMALL: (u16, u16) = (116, 116);
    pub const BIG: (u16, u16) = (290, 290);

    /// A preview filled with one colour.
    pub fn solid((width, height): (u16, u16), rgb565: u16) -> Self {
        Self {
            width,
            height,
            pixels: vec![rgb565; width as usize * height as usize],
        }
    }

    pub fn from_fn((width, height): (u16, u16), mut f: impl FnMut(u16, u16) -> u16) -> Self {
        let mut pixels = Vec::with_capacity(width as usize * height as usize);
        for y in 0..height {
            for x in 0..width {
                pixels.push(f(x, y));
            }
        }
        Self {
            width,
            height,
            pixels,
        }
    }

    pub(crate) fn write_to(&self, w: &mut impl Write, size: (u16, u16)) -> Result<(), Error> {
        if (self.width, self.height) != size
            || self.pixels.len() != size.0 as usize * size.1 as usize
        {
            return Err(Error::BadPreviewSize {
                expected: size,
                got: (self.width, self.height),
            });
        }
        let mut buf = Vec::with_capacity(self.pixels.len() * 2);
        for &p in &self.pixels {
            write_u16(&mut buf, p)?;
        }
        w.write_all(&buf)?;
        Ok(())
    }

    pub(crate) fn read_from(r: &mut impl Read, (width, height): (u16, u16)) -> Result<Self, Error> {
        let n = width as usize * height as usize;
        let mut pixels = Vec::with_capacity(n);
        for _ in 0..n {
            pixels.push(read_u16(r)?);
        }
        Ok(Self {
            width,
            height,
            pixels,
        })
    }
}

/// Packs 8-bit RGB into RGB565.
pub fn rgb565(r: u8, g: u8, b: u8) -> u16 {
    ((r as u16 >> 3) << 11) | ((g as u16 >> 2) << 5) | (b as u16 >> 3)
}
