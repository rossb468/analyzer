//! The backend for whatever platform this build targets.
//!
//! Callers that want "the audio hardware on this machine" ask here rather than
//! naming a backend type. That keeps platform selection in exactly one place:
//! the C ABI, and through it every client, compiles unchanged on every target,
//! and a new backend is one `cfg` arm rather than a hunt through its callers.

use crate::backend::AudioBackend;
use crate::device::DeviceInfo;
use crate::error::AudioError;
use crate::stream::{AudioCallback, AudioStream, StreamConfig};

/// The native backend for this build's target platform.
///
/// On a platform with no backend yet this is [`UnavailableBackend`], which
/// enumerates nothing and refuses to open, so everything above it still builds,
/// runs and can be tested.
pub fn default_backend() -> Box<dyn AudioBackend> {
    #[cfg(target_os = "macos")]
    {
        Box::new(crate::coreaudio::CoreAudioBackend::new())
    }
    #[cfg(target_os = "ios")]
    {
        Box::new(crate::ios::IosBackend::new())
    }
    #[cfg(not(any(target_os = "macos", target_os = "ios")))]
    {
        Box::new(UnavailableBackend)
    }
}

/// Stands in where no platform backend exists yet.
///
/// Enumeration succeeds and finds nothing, which is the truth: a client can
/// show an empty device list rather than an error. Opening fails with
/// [`AudioError::NoBackend`], because pretending to capture would be worse.
#[derive(Debug, Default, Clone, Copy)]
pub struct UnavailableBackend;

impl AudioBackend for UnavailableBackend {
    fn name(&self) -> &str {
        "none"
    }

    fn devices(&self) -> Result<Vec<DeviceInfo>, AudioError> {
        Ok(Vec::new())
    }

    fn open(
        &mut self,
        _config: &StreamConfig,
        _callback: Box<dyn AudioCallback>,
    ) -> Result<Box<dyn AudioStream>, AudioError> {
        Err(AudioError::NoBackend)
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::stream::AudioBuffers;

    #[test]
    fn the_unavailable_backend_lists_nothing_and_refuses_to_open() {
        let mut backend = UnavailableBackend;
        assert!(backend.devices().unwrap().is_empty());
        assert!(backend.default_input().unwrap().is_none());
        let config = StreamConfig {
            input: None,
            output: None,
            sample_rate: 48_000.0,
            buffer_frames: 512,
            input_channels: vec![0],
            output_channels: Vec::new(),
        };
        let result = backend.open(&config, Box::new(|_: &mut AudioBuffers<'_>| {}));
        assert!(matches!(result, Err(AudioError::NoBackend)));
    }

    #[test]
    fn a_default_backend_exists_on_every_platform() {
        let backend = default_backend();
        assert!(!backend.name().is_empty());
    }
}
