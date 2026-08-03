//! Live capture, and what it looks like where there is no backend for it.
//!
//! The settings live here and the implementation does not. Capturing from real
//! hardware needs a platform backend, and only CoreAudio exists so far, so the
//! macOS implementation sits in [`crate::live_coreaudio`] and this module
//! dispatches to it.
//!
//! Everything else the harness does - WAV analysis, the bench, swept
//! measurement against a pair of files - is pure computation and runs anywhere.
//! Keeping the platform-bound part behind one `cfg` is what lets the core be
//! built and tested on Linux and Windows without an audio stack at all.

use analyzer_dsp::SpectrumConfig;

/// Settings for a live capture run.
///
/// Every field is read by the CoreAudio implementation and none of them by the
/// stub, so on any other platform this is structurally dead. It is kept whole
/// rather than trimmed per platform: the settings describe what a live capture
/// *is*, and a second backend will want all of them.
#[cfg_attr(not(target_os = "macos"), allow(dead_code))]
#[derive(Debug, Clone)]
pub(crate) struct LiveOptions {
    /// Device UID, or `None` for the system default input.
    pub(crate) device: Option<String>,
    /// How long to capture.
    pub(crate) seconds: f64,
    /// Channel of the device to analyse.
    pub(crate) channel: usize,
    /// Requested callback size.
    pub(crate) block: u32,
    /// Spectrum settings; `sample_rate` is overwritten with what the device grants.
    pub(crate) spectrum: SpectrumConfig,
    /// Print a running meter to stderr while capturing.
    pub(crate) meter: bool,
}

#[cfg(target_os = "macos")]
pub(crate) use crate::live_coreaudio::{capture, list_devices};

/// What the live paths do on a platform with no backend.
///
/// The flags still exist and still parse, and saying so plainly beats a usage
/// message that silently differs between platforms. Implementing them is the
/// port work, not a missing feature of the harness.
#[cfg(not(target_os = "macos"))]
mod unsupported {
    use analyzer_engine::SpectrumFrame;

    use super::LiveOptions;

    const MESSAGE: &str = "live capture needs a platform audio backend, and this build has \
none. CoreAudio is implemented; WASAPI and ALSA/PipeWire arrive with the Windows and Linux \
clients. Everything else in this harness runs here: analyse a WAV file, or use --bench, \
--measure or --measure-demo.";

    pub(crate) fn list_devices() -> Result<String, String> {
        Err(MESSAGE.to_owned())
    }

    pub(crate) fn capture(_options: &LiveOptions) -> Result<SpectrumFrame, String> {
        Err(MESSAGE.to_owned())
    }
}

#[cfg(not(target_os = "macos"))]
pub(crate) use unsupported::{capture, list_devices};
