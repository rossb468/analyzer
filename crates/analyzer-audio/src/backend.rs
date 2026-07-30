//! The backend abstraction.
//!
//! This trait is the project's own, deliberately not a portability crate's.
//! Device control, exclusive access and high channel counts are exactly where
//! general-purpose audio wrappers are weakest, and a measurement tool lives on
//! device control. Each platform gets a direct implementation: CoreAudio now,
//! WASAPI and ALSA/PipeWire when the other clients happen.
//!
//! Only one real implementation exists today. That is the point — a trait with a
//! single implementation looks like overhead and is the cheapest insurance
//! against the deferred port turning into a rewrite.

use crate::device::DeviceInfo;
use crate::error::AudioError;
use crate::stream::{AudioCallback, AudioStream, StreamConfig};

/// A source of audio devices and streams.
pub trait AudioBackend: Send {
    /// Name of this backend, for display and logs.
    fn name(&self) -> &str;

    /// Enumerate currently present devices.
    ///
    /// The result is a snapshot. Devices come and go, so ids from an old call may
    /// no longer resolve.
    ///
    /// # Errors
    ///
    /// Returns a backend error if the platform refuses to enumerate.
    fn devices(&self) -> Result<Vec<DeviceInfo>, AudioError>;

    /// The system default capture device, if there is one.
    ///
    /// # Errors
    ///
    /// Returns a backend error if the platform refuses to report it.
    fn default_input(&self) -> Result<Option<DeviceInfo>, AudioError> {
        Ok(self.devices()?.into_iter().find(|d| d.is_default_input))
    }

    /// The system default playback device, if there is one.
    ///
    /// # Errors
    ///
    /// Returns a backend error if the platform refuses to report it.
    fn default_output(&self) -> Result<Option<DeviceInfo>, AudioError> {
        Ok(self.devices()?.into_iter().find(|d| d.is_default_output))
    }

    /// Open a stream. The returned stream is stopped; call
    /// [`AudioStream::start`] to begin.
    ///
    /// Opening separately from starting matters: allocation and device
    /// negotiation happen here, so that starting is cheap and nothing on the
    /// real-time path has to allocate.
    ///
    /// # Errors
    ///
    /// Returns [`AudioError::DeviceNotFound`], [`AudioError::UnsupportedSampleRate`],
    /// [`AudioError::ChannelOutOfRange`] or [`AudioError::NothingToDo`] for a
    /// configuration the hardware cannot satisfy.
    fn open(
        &mut self,
        config: &StreamConfig,
        callback: Box<dyn AudioCallback>,
    ) -> Result<Box<dyn AudioStream>, AudioError>;
}
