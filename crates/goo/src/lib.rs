//! Reader and writer for ELEGOO's .goo resin print format, version V3.
//!
//! Written from ELEGOO's "Goo Format Spec V1.2". A file is:
//!
//! ```text
//! [header: 195,477 bytes] [layer 1] ... [layer N] [ending string: 11 bytes]
//! ```
//!
//! All numbers are big-endian. Each layer is a [`LayerDef`], a 4-byte data
//! size, the encoded image (see [`rle`]) and a `\r\n` delimiter.
//!
//! GOO V5, a newer and different layout, is not supported yet.

mod bytes;
mod header;
mod layer;
mod preview;
pub mod rle;

use std::fmt;
use std::io::{self, BufReader, BufWriter, Read, Write};

pub use header::{DelayMode, Header, HEADER_SIZE};
pub use layer::LayerDef;
pub use preview::{rgb565, Preview};
pub use rle::LayerEncoder;

/// Fixed 8-byte tag after the version string.
pub const MAGIC_TAG: &[u8; 8] = &[0x07, 0x00, 0x00, 0x00, 0x44, 0x4C, 0x50, 0x00];
/// `\r\n`, used between sections.
pub const DELIMITER: &[u8; 2] = &[0x0D, 0x0A];
/// The last 11 bytes of every file: three zero bytes, then the magic tag.
pub const ENDING: &[u8; 11] = &[
    0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x44, 0x4C, 0x50, 0x00,
];

#[derive(Debug)]
pub enum Error {
    Io(io::Error),
    BadConstant {
        what: &'static str,
        expected: Vec<u8>,
        got: Vec<u8>,
    },
    FieldTooLong {
        field: &'static str,
        max: usize,
        got: usize,
    },
    BadPreviewSize {
        expected: (u16, u16),
        got: (u16, u16),
    },
    Rle(String),
    Invalid(String),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Io(e) => write!(f, "I/O error: {e}"),
            Error::BadConstant {
                what,
                expected,
                got,
            } => {
                write!(f, "bad {what}: expected {expected:02x?}, got {got:02x?}")
            }
            Error::FieldTooLong { field, max, got } => {
                write!(f, "{field} is {got} bytes, the field holds {max}")
            }
            Error::BadPreviewSize { expected, got } => {
                write!(
                    f,
                    "preview is {}x{}, expected {}x{}",
                    got.0, got.1, expected.0, expected.1
                )
            }
            Error::Rle(m) => write!(f, "layer image: {m}"),
            Error::Invalid(m) => write!(f, "{m}"),
        }
    }
}

impl std::error::Error for Error {}

impl From<io::Error> for Error {
    fn from(e: io::Error) -> Self {
        Error::Io(e)
    }
}

/// Writes a .goo file one layer at a time, so a 16K print never needs more
/// than one encoded layer in memory.
pub struct GooWriter<W: Write> {
    out: BufWriter<W>,
    expected_layers: u32,
    pixels_per_layer: u64,
    written: u32,
}

impl<W: Write> GooWriter<W> {
    /// Writes the header. `header.layer_count` must equal the number of
    /// layers that will be written.
    pub fn new(inner: W, header: &Header) -> Result<Self, Error> {
        if header.layer_content_offset != HEADER_SIZE {
            return Err(Error::Invalid(format!(
                "layer_content_offset is {}, must be {HEADER_SIZE}",
                header.layer_content_offset
            )));
        }
        let mut out = BufWriter::with_capacity(1 << 20, inner);
        header.write_to(&mut out)?;
        Ok(Self {
            out,
            expected_layers: header.layer_count,
            pixels_per_layer: header.pixel_count(),
            written: 0,
        })
    }

    /// Writes one layer. `data` comes from `LayerEncoder::finish`.
    pub fn write_layer(&mut self, def: &LayerDef, data: &[u8]) -> Result<(), Error> {
        if self.written >= self.expected_layers {
            return Err(Error::Invalid(
                "more layers written than header.layer_count".into(),
            ));
        }
        let pixels = rle::decode_runs(data, |_, _| {})?;
        if pixels != self.pixels_per_layer {
            return Err(Error::Invalid(format!(
                "layer {} has {pixels} pixels, the LCD has {}",
                self.written, self.pixels_per_layer
            )));
        }
        def.write_with_data(&mut self.out, data)?;
        self.written += 1;
        Ok(())
    }

    /// Writes the ending string and flushes.
    pub fn finish(mut self) -> Result<W, Error> {
        if self.written != self.expected_layers {
            return Err(Error::Invalid(format!(
                "wrote {} layers, header says {}",
                self.written, self.expected_layers
            )));
        }
        self.out.write_all(ENDING)?;
        self.out.into_inner().map_err(|e| Error::Io(e.into_error()))
    }
}

/// A fully parsed file, with layer images still encoded.
#[derive(Debug, Clone)]
pub struct GooFile {
    pub header: Header,
    pub layers: Vec<(LayerDef, Vec<u8>)>,
}

impl GooFile {
    /// Parses a whole file and checks every layer's magic byte, checksum and
    /// pixel count.
    pub fn read(inner: impl Read) -> Result<Self, Error> {
        let mut r = BufReader::with_capacity(1 << 20, inner);
        let header = Header::read_from(&mut r)?;
        if header.layer_content_offset != HEADER_SIZE {
            return Err(Error::Invalid(format!(
                "layer_content_offset is {}, expected {HEADER_SIZE}",
                header.layer_content_offset
            )));
        }
        let mut layers = Vec::with_capacity(header.layer_count as usize);
        for i in 0..header.layer_count {
            let (def, data) = LayerDef::read_with_data(&mut r)?;
            let pixels = rle::decode_runs(&data, |_, _| {})
                .map_err(|e| Error::Invalid(format!("layer {i}: {e}")))?;
            if pixels != header.pixel_count() {
                return Err(Error::Invalid(format!(
                    "layer {i} has {pixels} pixels, expected {}",
                    header.pixel_count()
                )));
            }
            layers.push((def, data));
        }
        bytes::expect(&mut r, ENDING, "ending string")?;
        let mut rest = [0u8; 1];
        if r.read(&mut rest)? != 0 {
            return Err(Error::Invalid("trailing bytes after ending string".into()));
        }
        Ok(Self { header, layers })
    }
}
