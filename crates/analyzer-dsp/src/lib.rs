//! Signal processing primitives: FFT, windows, spectra, transfer functions.
//!
//! This crate is deliberately free of I/O and platform dependencies. It knows
//! nothing about audio devices, files, or user interfaces — it turns buffers of
//! samples into buffers of numbers, which is what makes it testable headlessly
//! and portable by construction.
//!
//! # Real-time contract
//!
//! Types here that sit on the analysis path preallocate at construction and do
//! not allocate afterwards. That is not a style preference: the analysis thread
//! runs several hundred times a second behind a lock-free queue fed by an audio
//! callback with a hard deadline, and an allocation with an unbounded worst case
//! eventually surfaces as a dropped block and a silently corrupted measurement.

pub mod fft;
pub mod generator;
pub mod spectrum;
pub mod window;

pub use fft::{Fft, RealFft};
pub use generator::{Generator, Signal};
pub use spectrum::{Averaging, Overlap, SpectrumAnalyzer, SpectrumConfig};
pub use window::{Window, WindowKind};

/// Re-exported so callers need not depend on `rustfft` directly to name a bin.
pub use rustfft::num_complex::Complex32;
