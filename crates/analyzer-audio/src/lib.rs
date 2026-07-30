//! Audio device abstraction and platform backends.
//!
//! The [`AudioBackend`] trait is this project's own rather than a portability
//! crate's. That is a deliberate cost: device control, exclusive access and high
//! channel counts are where general-purpose audio wrappers are weakest, and a
//! measurement tool lives or dies on device control.
//!
//! Today there is one real implementation, [`OfflineBackend`], which reads from
//! memory and drives the headless harness. CoreAudio comes next; WASAPI and
//! ALSA/PipeWire arrive with the other clients. The trait existing before the
//! second implementation does is what keeps that port cheap.
//!
//! No Apple SDK type appears anywhere in this crate outside a
//! `cfg(target_os = "macos")` module, and no other `analyzer-*` crate depends on
//! an Apple crate at all.

pub mod backend;
pub mod device;
pub mod error;
pub mod offline;
pub mod stream;

pub use backend::AudioBackend;
pub use device::{DeviceId, DeviceInfo};
pub use error::AudioError;
pub use offline::{OfflineBackend, OfflineStream, Source};
pub use stream::{AudioBuffers, AudioCallback, AudioStream, StreamConfig, StreamLatency};
