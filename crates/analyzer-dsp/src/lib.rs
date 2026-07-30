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

pub mod deconv;
pub mod delay;
pub mod distortion;
pub mod fft;
pub mod generator;
pub mod ir;
pub mod meter;
pub mod mtw;
pub mod octave;
pub mod spectrum;
pub mod transfer;
pub mod window;

pub use deconv::{Deconvolver, ImpulseResponse};
pub use delay::{DelayEstimate, DelayFinder};
pub use distortion::{Distortion, DistortionConfig, Harmonic};
pub use fft::{Fft, RealFft};
pub use generator::{Generator, Signal};
pub use ir::{Gate, GatedResponse, ReverbTime, gated_response, reverb_time, schroeder_decay};
pub use meter::{Integration, LevelMeter, MeterWeighting};
pub use mtw::{MtwConfig, MtwPoint, MultiTimeWindow};
pub use octave::{Band, OctaveBands};
pub use spectrum::{Averaging, Overlap, SpectrumAnalyzer, SpectrumConfig};
pub use transfer::{TransferAveraging, TransferConfig, TransferFunction, unwrap_phase_degrees};
pub use window::{Window, WindowKind};

/// Re-exported so callers need not depend on `rustfft` directly to name a bin.
pub use rustfft::num_complex::Complex32;
