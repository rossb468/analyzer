//! Session state, the measurement store and the on-disk format.
//!
//! The plan calls for designing the container before there is data to migrate,
//! because retrofitting a format after users have a folder of saved measurements
//! is the worst version of that problem. So this exists ahead of anything that
//! writes to it.
//!
//! Three storage rules drive the design, all cheap now and impossible later:
//! measurements are stored **unsmoothed, complex, at the native sample rate**;
//! in **`f64`** so repeated round trips do not accumulate rounding; and with
//! their **absolute references** attached, because a measurement whose time zero
//! and voltage scaling are unknown is a picture of a measurement rather than a
//! measurement.

pub mod export;
pub mod filter_export;
pub mod format;
pub mod measurement;
pub mod settings;
pub mod store;

pub use filter_export::FilterFormat;
pub use format::{FormatError, MAGIC, read, write};
pub use measurement::{Complex64, Measurement, MeasurementData, MeasurementId, References};
pub use settings::{AveragingChoice, SETTINGS_MAGIC, Settings, WindowChoice};
pub use store::MeasurementStore;
