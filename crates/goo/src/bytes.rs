//! Big-endian primitive readers and writers. Every multi-byte number in a
//! .goo file is big-endian.

use std::io::{self, Read, Write};

use crate::Error;

pub(crate) fn write_u8(w: &mut impl Write, v: u8) -> io::Result<()> {
    w.write_all(&[v])
}

pub(crate) fn write_bool(w: &mut impl Write, v: bool) -> io::Result<()> {
    write_u8(w, v as u8)
}

pub(crate) fn write_u16(w: &mut impl Write, v: u16) -> io::Result<()> {
    w.write_all(&v.to_be_bytes())
}

pub(crate) fn write_u32(w: &mut impl Write, v: u32) -> io::Result<()> {
    w.write_all(&v.to_be_bytes())
}

pub(crate) fn write_f32(w: &mut impl Write, v: f32) -> io::Result<()> {
    w.write_all(&v.to_be_bytes())
}

/// Writes `s` into a fixed-size, zero-padded field. Fails if it does not fit.
pub(crate) fn write_str(
    w: &mut impl Write,
    s: &str,
    len: usize,
    field: &'static str,
) -> Result<(), Error> {
    let bytes = s.as_bytes();
    if bytes.len() > len {
        return Err(Error::FieldTooLong {
            field,
            max: len,
            got: bytes.len(),
        });
    }
    w.write_all(bytes)?;
    w.write_all(&vec![0u8; len - bytes.len()])?;
    Ok(())
}

pub(crate) fn read_exact<const N: usize>(r: &mut impl Read) -> io::Result<[u8; N]> {
    let mut buf = [0u8; N];
    r.read_exact(&mut buf)?;
    Ok(buf)
}

pub(crate) fn read_u8(r: &mut impl Read) -> io::Result<u8> {
    Ok(read_exact::<1>(r)?[0])
}

pub(crate) fn read_bool(r: &mut impl Read) -> io::Result<bool> {
    Ok(read_u8(r)? != 0)
}

pub(crate) fn read_u16(r: &mut impl Read) -> io::Result<u16> {
    Ok(u16::from_be_bytes(read_exact(r)?))
}

pub(crate) fn read_u32(r: &mut impl Read) -> io::Result<u32> {
    Ok(u32::from_be_bytes(read_exact(r)?))
}

pub(crate) fn read_f32(r: &mut impl Read) -> io::Result<f32> {
    Ok(f32::from_be_bytes(read_exact(r)?))
}

/// Reads a fixed-size string field, dropping trailing NUL padding.
pub(crate) fn read_str(r: &mut impl Read, len: usize) -> io::Result<String> {
    let mut buf = vec![0u8; len];
    r.read_exact(&mut buf)?;
    let end = buf.iter().position(|&b| b == 0).unwrap_or(len);
    Ok(String::from_utf8_lossy(&buf[..end]).into_owned())
}

pub(crate) fn expect(r: &mut impl Read, expected: &[u8], what: &'static str) -> Result<(), Error> {
    let mut buf = vec![0u8; expected.len()];
    r.read_exact(&mut buf)?;
    if buf != expected {
        return Err(Error::BadConstant {
            what,
            expected: expected.to_vec(),
            got: buf,
        });
    }
    Ok(())
}
