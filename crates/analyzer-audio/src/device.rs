//! Device identity and capabilities.

use std::fmt;

/// Opaque, backend-scoped device identifier.
///
/// Deliberately a string rather than an integer: CoreAudio uses numeric ids that
/// are only stable within a boot, WASAPI uses endpoint id strings, and ALSA uses
/// `hw:X,Y` names. A string is the only representation all three can round-trip,
/// and it is what gets persisted in a saved session.
#[derive(Debug, Clone, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct DeviceId(String);

impl DeviceId {
    /// Wrap a backend-specific identifier.
    pub fn new(id: impl Into<String>) -> Self {
        Self(id.into())
    }

    /// The underlying identifier.
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

impl fmt::Display for DeviceId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

/// What a device is and what it can do.
#[derive(Debug, Clone, PartialEq)]
pub struct DeviceInfo {
    /// Backend-scoped identifier.
    pub id: DeviceId,
    /// Name to show a user.
    pub name: String,
    /// Capture channels available.
    pub input_channels: u32,
    /// Playback channels available.
    pub output_channels: u32,
    /// The rate the device is currently set to.
    pub default_sample_rate: f64,
    /// Rates the device reports it can run at.
    ///
    /// Empty means the backend could not determine the list, not that no rate
    /// works. Treat it as unknown rather than unsupported.
    pub supported_sample_rates: Vec<f64>,
    /// Whether the system considers this the default capture device.
    pub is_default_input: bool,
    /// Whether the system considers this the default playback device.
    pub is_default_output: bool,
}

impl DeviceInfo {
    /// Whether this device can capture.
    pub fn has_input(&self) -> bool {
        self.input_channels > 0
    }

    /// Whether this device can play back.
    pub fn has_output(&self) -> bool {
        self.output_channels > 0
    }

    /// Whether `rate` is known to work.
    ///
    /// Returns `true` when the supported list is empty, since an unknown list
    /// must not be read as a refusal.
    pub fn supports_sample_rate(&self, rate: f64) -> bool {
        if self.supported_sample_rates.is_empty() {
            return true;
        }
        self.supported_sample_rates
            .iter()
            .any(|r| (r - rate).abs() < 0.5)
    }
}
