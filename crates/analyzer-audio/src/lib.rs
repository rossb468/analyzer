//! Audio device abstraction and platform backends.
//!
//! The [`AudioBackend`] trait is this project's own rather than a portability
//! crate's. That is a deliberate cost: device control, exclusive access and high
//! channel counts are where general-purpose audio wrappers are weakest, and a
//! measurement tool lives or dies on device control.
//!
//! [`OfflineBackend`] reads from memory and drives the headless harness.
//! CoreAudio drives macOS, and RemoteIO with `AVAudioSession` drives iOS. WASAPI and ALSA/PipeWire arrive with the other
//! clients. [`default_backend`] picks the one for the build's target, so nothing
//! above this crate names a platform.
//!
//! No Apple SDK type appears anywhere in this crate outside a
//! `cfg(target_os = "macos")` or `cfg(target_os = "ios")` module, and no other
//! `analyzer-*` crate depends on an Apple crate at all.

pub mod backend;
#[cfg(target_os = "macos")]
pub mod coreaudio;
pub mod device;
pub mod error;
#[cfg(target_os = "ios")]
pub mod ios;
pub mod offline;
pub mod platform;
pub mod stream;

pub use backend::AudioBackend;
#[cfg(target_os = "macos")]
pub use coreaudio::{CoreAudioBackend, CoreAudioStream};
pub use device::{DeviceId, DeviceInfo};
pub use error::AudioError;
#[cfg(target_os = "ios")]
pub use ios::{IosBackend, IosStream};
pub use offline::{OfflineBackend, OfflineStream, Source};
pub use platform::{UnavailableBackend, default_backend};
pub use stream::{AudioBuffers, AudioCallback, AudioStream, StreamConfig, StreamLatency};
