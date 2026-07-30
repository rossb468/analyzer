//! The calibration chain: raw converter samples to absolute dB SPL.
//!
//! This is its own crate because calibration is pervasive and subtle, and every
//! number a user reads depends on all of it:
//!
//! ```text
//! dBFS ──▶ + SPL offset ──▶ + microphone response ──▶ + weighting ──▶ dB SPL
//! ```
//!
//! Scattered across the DSP and model crates it would be subtly wrong somewhere
//! and stay wrong for a long time, because the failure mode is a constant offset
//! on every reading rather than anything that looks broken. Here it is one chain
//! that raw levels pass through exactly once, with a golden test asserting that a
//! 94 dB reference tone reads back as 94 dB.

pub mod chain;
pub mod curve;
pub mod weighting;

pub use chain::{CALIBRATOR_SPL_DB, Calibration};
pub use curve::ResponseCurve;
pub use weighting::Weighting;
