//! Errors raised while enumerating devices or opening streams.

use thiserror::Error;

/// Something went wrong talking to the audio hardware.
///
/// These are all setup-time failures. Nothing here is returned from the
/// real-time callback, which has no way to handle an error and no time to try.
#[derive(Debug, Error)]
pub enum AudioError {
    /// No backend is compiled in or available for this platform.
    #[error("no audio backend is available on this platform")]
    NoBackend,

    /// The requested device is gone — unplugged, or an id from a stale list.
    #[error("audio device not found: {0}")]
    DeviceNotFound(String),

    /// The device will not run at the requested rate.
    #[error("{device} does not support {requested} Hz")]
    UnsupportedSampleRate {
        /// Human-readable device name.
        device: String,
        /// Rate that was asked for, in hertz.
        requested: f64,
    },

    /// A requested channel index does not exist on the device.
    #[error("channel {channel} out of range for {device}, which has {available}")]
    ChannelOutOfRange {
        /// Human-readable device name.
        device: String,
        /// Index that was asked for.
        channel: u32,
        /// How many the device actually has.
        available: u32,
    },

    /// No input and no output channel was selected, so there is nothing to do.
    #[error("stream configuration selects no input and no output channels")]
    NothingToDo,

    /// Start was called on a stream that is already running.
    #[error("stream is already running")]
    AlreadyRunning,

    /// The platform API failed for a reason worth passing through verbatim.
    #[error("audio backend error: {0}")]
    Backend(String),
}
