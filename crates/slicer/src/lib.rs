//! FruitSlicer's slicing core.
//!
//! The pipeline is: load an STL ([`stl`]), place it on the plate
//! ([`Mesh::place_on_plate`]), then [`slice_to_goo`] cuts it into layers,
//! fills each layer onto the LCD's pixels ([`raster`]) and writes a .goo
//! file one layer at a time.

pub mod checks;
pub mod hollow;
pub mod job;
pub mod mesh;
pub mod preview;
pub mod printer;
pub mod raster;
pub mod settings;
pub mod stl;
pub mod supports;

use std::fmt;

pub use job::{slice_to_goo, slice_to_goo_with, SliceHooks, SliceReport};
pub use mesh::Mesh;
pub use printer::{Printer, SATURN_4_ULTRA_16K};
pub use settings::PrintSettings;

#[derive(Debug)]
pub enum SliceError {
    EmptyModel,
    DoesNotFit(String),
    Settings(String),
    Goo(goo::Error),
    Cancelled,
}

impl fmt::Display for SliceError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            SliceError::EmptyModel => write!(f, "the model is empty or has no height"),
            SliceError::DoesNotFit(m) => write!(f, "the model does not fit: {m}"),
            SliceError::Settings(m) => write!(f, "bad settings: {m}"),
            SliceError::Goo(e) => write!(f, "writing .goo: {e}"),
            SliceError::Cancelled => write!(f, "slicing was cancelled"),
        }
    }
}

impl std::error::Error for SliceError {}

impl From<goo::Error> for SliceError {
    fn from(e: goo::Error) -> Self {
        SliceError::Goo(e)
    }
}
