//! Stable C ABI surface consumed by the platform user interfaces.
//!
//! # Shape of the boundary
//!
//! Two kinds of traffic cross here and they have opposite needs, so they use
//! different mechanisms:
//!
//! - **Control and state** — starting, stopping, choosing a device, resizing the
//!   plot. Low frequency, so plain functions over opaque handles and POD structs.
//! - **Frame data** — a reduced trace, a few thousand floats, at up to 120 fps.
//!   This is **copied** into caller-owned memory.
//!
//! Copying is deliberate. An earlier design handed out a pointer into the
//! engine's triple buffer to avoid the copy, which is premature optimisation at
//! this scale — a trace is a few kilobytes and a memcpy costs microseconds —
//! and it introduced a real hazard, because that buffer is only valid until the
//! consumer's next read and an asynchronous Metal upload can race it into a torn
//! read.
//!
//! # Rules for every entry point
//!
//! - Null pointers are tolerated and become a failure return, never a crash.
//! - Panics are caught. Unwinding into C is undefined behaviour, and a UI thread
//!   should not die because analysis hit an edge case.
//! - Nothing allocates memory the caller must free, except the two explicit
//!   `_destroy`/`_stop` functions.

use std::ffi::{CString, c_char};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, Ordering};

use analyzer_audio::{
    AudioBackend, AudioBuffers, AudioStream, CoreAudioBackend, DeviceId, DeviceInfo, StreamConfig,
};
use analyzer_dsp::{
    Averaging, Biquad, DistortionConfig, Equaliser, FilterBand, FilterKind, Generator, Overlap,
    Signal, SpectrumConfig, WindowKind,
};
use analyzer_engine::{
    AnalysisMode, Engine, EngineConfig, SnapshotPublisher, SnapshotReader, rt_section,
    snapshot_channel,
};
use analyzer_model::{Measurement, MeasurementData, MeasurementId, References};
use analyzer_plot::{
    FrequencyAxis, LevelAxis, LinearReduction, Reduction, Trace, reduce, reduce_linear,
};

/// Longest message [`AnalyzerStatus`] can carry, including the terminator.
pub const ANALYZER_MESSAGE_LEN: usize = 256;

/// Outcome of a call that can fail.
///
/// The message is an inline fixed buffer rather than a pointer, so there is
/// nothing for the caller to free and no lifetime to reason about.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct AnalyzerStatus {
    /// Zero on success, non-zero on failure.
    pub code: i32,
    /// NUL-terminated UTF-8 explanation. Empty on success.
    pub message: [c_char; ANALYZER_MESSAGE_LEN],
}

impl Default for AnalyzerStatus {
    fn default() -> Self {
        Self {
            code: 0,
            message: [0; ANALYZER_MESSAGE_LEN],
        }
    }
}

impl AnalyzerStatus {
    fn ok() -> Self {
        Self::default()
    }

    fn failure(message: &str) -> Self {
        let mut status = Self {
            code: 1,
            message: [0; ANALYZER_MESSAGE_LEN],
        };
        // Truncate on a character boundary so the buffer stays valid UTF-8.
        let mut end = message.len().min(ANALYZER_MESSAGE_LEN - 1);
        while end > 0 && !message.is_char_boundary(end) {
            end -= 1;
        }
        for (slot, byte) in status
            .message
            .iter_mut()
            .zip(message.as_bytes().iter().take(end))
        {
            *slot = *byte as c_char;
        }
        status
    }
}

/// Write a status through an optional out-pointer.
///
/// # Safety
///
/// `out` must be null or point to a writable [`AnalyzerStatus`].
unsafe fn set_status(out: *mut AnalyzerStatus, status: AnalyzerStatus) {
    if !out.is_null() {
        unsafe { ptr::write(out, status) };
    }
}

/// Run `f`, converting a panic into `fallback`.
///
/// Unwinding across the FFI boundary is undefined behaviour. `AssertUnwindSafe`
/// is justified because a panic here abandons the call and discards its result;
/// nothing observes a half-mutated state afterwards.
fn guard<R>(fallback: R, f: impl FnOnce() -> R) -> R {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(fallback)
}

// ---------------------------------------------------------------------------
// Device enumeration
// ---------------------------------------------------------------------------

/// A snapshot of the devices present when it was created.
///
/// Owning the strings in a list, rather than returning them one at a time, is
/// what makes the borrowed `const char*` in [`AnalyzerDevice`] safe: they stay
/// valid until the list is destroyed.
pub struct AnalyzerDeviceList {
    devices: Vec<DeviceInfo>,
    /// Kept alive so the pointers handed out remain valid.
    strings: Vec<(CString, CString)>,
}

/// One device, with borrowed strings valid while its list lives.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct AnalyzerDevice {
    /// Stable identifier to pass back when starting a session.
    pub uid: *const c_char,
    /// Human-readable name.
    pub name: *const c_char,
    /// Capture channels.
    pub input_channels: u32,
    /// Playback channels.
    pub output_channels: u32,
    /// Current rate in hertz.
    pub sample_rate: f64,
    /// Whether the system considers this the default capture device.
    pub is_default_input: bool,
}

/// Enumerate audio devices. Returns null only if enumeration panicked.
#[unsafe(no_mangle)]
pub extern "C" fn analyzer_device_list_create() -> *mut AnalyzerDeviceList {
    guard(ptr::null_mut(), || {
        let devices = CoreAudioBackend::new().devices().unwrap_or_default();
        let strings = devices
            .iter()
            .map(|d| {
                (
                    CString::new(d.id.as_str()).unwrap_or_default(),
                    CString::new(d.name.as_str()).unwrap_or_default(),
                )
            })
            .collect();
        Box::into_raw(Box::new(AnalyzerDeviceList { devices, strings }))
    })
}

/// Release a device list.
///
/// # Safety
///
/// `list` must come from [`analyzer_device_list_create`] and not already be
/// destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_device_list_destroy(list: *mut AnalyzerDeviceList) {
    if list.is_null() {
        return;
    }
    guard((), || {
        drop(unsafe { Box::from_raw(list) });
    });
}

/// How many devices the list holds.
///
/// # Safety
///
/// `list` must be null or a live device list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_device_list_count(list: *const AnalyzerDeviceList) -> usize {
    if list.is_null() {
        return 0;
    }
    guard(0, || unsafe { (*list).devices.len() })
}

/// Read one device. Returns false for a bad index.
///
/// The strings in `out` point into the list and stay valid until it is
/// destroyed.
///
/// # Safety
///
/// `list` must be null or live; `out` must be null or writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_device_list_get(
    list: *const AnalyzerDeviceList,
    index: usize,
    out: *mut AnalyzerDevice,
) -> bool {
    if list.is_null() || out.is_null() {
        return false;
    }
    guard(false, || unsafe {
        let list = &*list;
        let (Some(device), Some((uid, name))) = (list.devices.get(index), list.strings.get(index))
        else {
            return false;
        };
        ptr::write(
            out,
            AnalyzerDevice {
                uid: uid.as_ptr(),
                name: name.as_ptr(),
                input_channels: device.input_channels,
                output_channels: device.output_channels,
                sample_rate: device.default_sample_rate,
                is_default_input: device.is_default_input,
            },
        );
        true
    })
}

impl std::fmt::Debug for AnalyzerDeviceList {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("AnalyzerDeviceList")
            .field("devices", &self.devices.len())
            .finish()
    }
}

// ---------------------------------------------------------------------------
// Session configuration
// ---------------------------------------------------------------------------

/// Analysis window, mirroring [`WindowKind`].
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AnalyzerWindow {
    /// No taper.
    Rectangular = 0,
    /// General-purpose default.
    Hann = 1,
    /// Low leakage, wider main lobe.
    BlackmanHarris = 2,
    /// Flat main lobe, accurate amplitude. For calibration.
    FlatTop = 3,
    /// Tapered cosine at alpha 0.25.
    Tukey = 4,
}

impl From<AnalyzerWindow> for WindowKind {
    fn from(value: AnalyzerWindow) -> Self {
        match value {
            AnalyzerWindow::Rectangular => WindowKind::Rectangular,
            AnalyzerWindow::Hann => WindowKind::Hann,
            AnalyzerWindow::BlackmanHarris => WindowKind::BlackmanHarris,
            AnalyzerWindow::FlatTop => WindowKind::FlatTop,
            AnalyzerWindow::Tukey => WindowKind::Tukey { alpha: 0.25 },
        }
    }
}

/// Frame overlap, mirroring [`Overlap`].
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AnalyzerOverlap {
    /// No overlap.
    None = 0,
    /// 50%.
    Half = 1,
    /// 75%.
    ThreeQuarters = 2,
    /// 87.5%.
    SevenEighths = 3,
}

impl From<AnalyzerOverlap> for Overlap {
    fn from(value: AnalyzerOverlap) -> Self {
        match value {
            AnalyzerOverlap::None => Overlap::None,
            AnalyzerOverlap::Half => Overlap::Half,
            AnalyzerOverlap::ThreeQuarters => Overlap::ThreeQuarters,
            AnalyzerOverlap::SevenEighths => Overlap::SevenEighths,
        }
    }
}

/// Averaging mode, mirroring the useful subset of [`Averaging`].
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AnalyzerAveraging {
    /// Each frame replaces the last.
    None = 0,
    /// Exponential with a one second time constant.
    Fast = 1,
    /// Average everything since start.
    Infinite = 2,
    /// Hold the maximum per bin.
    PeakHold = 3,
}

/// How several bins in one pixel column combine.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AnalyzerReduction {
    /// Loudest bin. Preserves narrow peaks.
    Max = 0,
    /// Power-domain mean.
    Mean = 1,
}

impl From<AnalyzerReduction> for Reduction {
    fn from(value: AnalyzerReduction) -> Self {
        match value {
            AnalyzerReduction::Max => Reduction::Max,
            AnalyzerReduction::Mean => Reduction::Mean,
        }
    }
}

/// Everything needed to start capturing and analysing.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct AnalyzerSessionConfig {
    /// Device UID, or null for the system default input.
    pub device_uid: *const c_char,
    /// Which device channel to analyse.
    pub channel: u32,
    /// FFT size; must be even and at least two.
    pub fft_size: u32,
    /// Requested callback size in frames.
    pub buffer_frames: u32,
    /// Analysis window.
    pub window: AnalyzerWindow,
    /// Frame overlap.
    pub overlap: AnalyzerOverlap,
    /// Averaging mode.
    pub averaging: AnalyzerAveraging,
    /// What to compute.
    pub mode: AnalyzerMode,
    /// Where the transfer function reference comes from.
    pub reference: AnalyzerReference,
    /// Reference input channel, used when `reference` is
    /// [`AnalyzerReference::Input`].
    pub reference_channel: u32,
    /// Stimulus to play. [`AnalyzerSignal::Silence`] opens no output at all.
    pub signal: AnalyzerSignal,
    /// Stimulus level in dBFS, as a peak amplitude. Clamped to at most 0.
    pub signal_level_db: f32,
    /// Sine frequency, used when `signal` is [`AnalyzerSignal::Sine`].
    pub signal_hz: f32,
    /// Bit per device output channel; bit 0 is channel 0.
    pub output_mask: u32,
}

impl Default for AnalyzerSessionConfig {
    fn default() -> Self {
        Self {
            device_uid: ptr::null(),
            channel: 0,
            fft_size: 4096,
            buffer_frames: 512,
            window: AnalyzerWindow::Hann,
            overlap: AnalyzerOverlap::ThreeQuarters,
            averaging: AnalyzerAveraging::Fast,
            mode: AnalyzerMode::Spectrum,
            reference: AnalyzerReference::Internal,
            reference_channel: 1,
            signal: AnalyzerSignal::Silence,
            // Quiet enough not to startle anyone, loud enough to measure. A
            // default that plays at full scale into unknown speakers is a
            // default that damages something.
            signal_level_db: -20.0,
            signal_hz: 1000.0,
            output_mask: 0b11,
        }
    }
}

/// Most bands an equaliser can carry across the boundary.
///
/// Fixed so the coefficients handed to the audio thread are a plain array with
/// no allocation behind them. Twenty-four is more than any room correction
/// needs and more than any hardware unit this would be exported to accepts.
pub const ANALYZER_MAX_EQ_BANDS: usize = 24;

/// Shape of an equaliser band.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum AnalyzerFilterKind {
    /// A bump or dip centred on the frequency.
    #[default]
    Peaking = 0,
    LowShelf = 1,
    HighShelf = 2,
    LowPass = 3,
    HighPass = 4,
    BandPass = 5,
    Notch = 6,
    AllPass = 7,
}

impl From<AnalyzerFilterKind> for FilterKind {
    fn from(kind: AnalyzerFilterKind) -> Self {
        match kind {
            AnalyzerFilterKind::Peaking => FilterKind::Peaking,
            AnalyzerFilterKind::LowShelf => FilterKind::LowShelf,
            AnalyzerFilterKind::HighShelf => FilterKind::HighShelf,
            AnalyzerFilterKind::LowPass => FilterKind::LowPass,
            AnalyzerFilterKind::HighPass => FilterKind::HighPass,
            AnalyzerFilterKind::BandPass => FilterKind::BandPass,
            AnalyzerFilterKind::Notch => FilterKind::Notch,
            AnalyzerFilterKind::AllPass => FilterKind::AllPass,
        }
    }
}

impl From<FilterKind> for AnalyzerFilterKind {
    fn from(kind: FilterKind) -> Self {
        match kind {
            FilterKind::Peaking => AnalyzerFilterKind::Peaking,
            FilterKind::LowShelf => AnalyzerFilterKind::LowShelf,
            FilterKind::HighShelf => AnalyzerFilterKind::HighShelf,
            FilterKind::LowPass => AnalyzerFilterKind::LowPass,
            FilterKind::HighPass => AnalyzerFilterKind::HighPass,
            FilterKind::BandPass => AnalyzerFilterKind::BandPass,
            FilterKind::Notch => AnalyzerFilterKind::Notch,
            FilterKind::AllPass => AnalyzerFilterKind::AllPass,
        }
    }
}

/// One equaliser band.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct AnalyzerBand {
    pub kind: AnalyzerFilterKind,
    /// Centre or corner frequency in hertz.
    pub hz: f32,
    /// Gain in decibels. Ignored by the pass and reject shapes.
    pub gain_db: f32,
    /// Quality factor. Higher is narrower.
    pub q: f32,
    /// Whether the band contributes. A disabled band keeps its settings.
    pub enabled: bool,
}

impl Default for AnalyzerBand {
    fn default() -> Self {
        FilterBand::default().into()
    }
}

impl From<AnalyzerBand> for FilterBand {
    fn from(band: AnalyzerBand) -> Self {
        Self {
            kind: band.kind.into(),
            hz: band.hz,
            gain_db: band.gain_db,
            q: band.q,
            enabled: band.enabled,
        }
    }
}

impl From<FilterBand> for AnalyzerBand {
    fn from(band: FilterBand) -> Self {
        Self {
            kind: band.kind.into(),
            hz: band.hz,
            gain_db: band.gain_db,
            q: band.q,
            enabled: band.enabled,
        }
    }
}

/// Which equaliser is active.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum AnalyzerEqMode {
    /// No equalisation. The stimulus is unfiltered and no curve is drawn.
    #[default]
    Off = 0,
    /// Ten fixed octave bands, gains only.
    Graphic = 1,
    /// Arbitrary bands of any shape.
    Parametric = 2,
}

/// Coefficients handed to the audio thread.
///
/// A plain array rather than the [`Equaliser`] itself: the equaliser owns a
/// `Vec`, and the audio thread must never touch an allocation. `generation`
/// lets the callback tell a change from a re-read, so it only copies
/// coefficients when they actually moved - and it copies coefficients only,
/// leaving each section's delay line alone so a fader move does not click.
#[derive(Debug, Clone, Copy)]
struct EqCoefficients {
    generation: u64,
    count: usize,
    sections: [Biquad; ANALYZER_MAX_EQ_BANDS],
    trim: f32,
}

impl Default for EqCoefficients {
    fn default() -> Self {
        Self {
            generation: 0,
            count: 0,
            sections: [Biquad::IDENTITY; ANALYZER_MAX_EQ_BANDS],
            trim: 1.0,
        }
    }
}

impl EqCoefficients {
    fn from_equaliser(generation: u64, eq: &Equaliser) -> Self {
        let mut out = Self {
            generation,
            count: 0,
            sections: [Biquad::IDENTITY; ANALYZER_MAX_EQ_BANDS],
            trim: 10.0_f32.powf(eq.preamp_db() / 20.0),
        };
        for (slot, band) in out.sections.iter_mut().zip(eq.bands()) {
            *slot = band.design(eq.sample_rate());
            out.count += 1;
        }
        out
    }
}

/// The equaliser as the audio thread sees it.
///
/// Coefficients arrive through the same triple buffer the analysis frames use;
/// the state stays here, on the audio thread, because it belongs to the running
/// filter rather than to the settings.
struct EqProcessor {
    reader: SnapshotReader<EqCoefficients>,
    sections: [Biquad; ANALYZER_MAX_EQ_BANDS],
    count: usize,
    trim: f32,
    generation: u64,
}

impl EqProcessor {
    fn new(reader: SnapshotReader<EqCoefficients>) -> Self {
        Self {
            reader,
            sections: [Biquad::IDENTITY; ANALYZER_MAX_EQ_BANDS],
            count: 0,
            trim: 1.0,
            generation: 0,
        }
    }

    fn process(&mut self, samples: &mut [f32]) {
        let latest = *self.reader.read();
        if latest.generation != self.generation {
            self.generation = latest.generation;
            self.count = latest.count;
            self.trim = latest.trim;
            for (section, designed) in self.sections.iter_mut().zip(&latest.sections) {
                // Coefficients only. Replacing the whole section would reset the
                // delay line, and a filter restarted mid-signal clicks.
                section.b0 = designed.b0;
                section.b1 = designed.b1;
                section.b2 = designed.b2;
                section.a1 = designed.a1;
                section.a2 = designed.a2;
            }
        }

        for section in self.sections.iter_mut().take(self.count) {
            section.process_block(samples);
        }
        if self.trim != 1.0 {
            for sample in samples.iter_mut() {
                *sample *= self.trim;
            }
        }
    }
}

/// Generator settings shared with the audio callback.
///
/// Atomics rather than a lock: the callback reads these on the real-time
/// thread, where blocking on a UI thread's mutex is the classic way to produce
/// a dropout. `generation` is bumped last, so seeing a new value guarantees the
/// fields behind it are already written.
#[derive(Debug, Default)]
struct SignalState {
    generation: AtomicU32,
    kind: AtomicU32,
    amplitude_bits: AtomicU32,
    hz_bits: AtomicU32,
}

impl SignalState {
    fn new(signal: AnalyzerSignal, level_db: f32, hz: f32) -> Self {
        let state = Self::default();
        state.set(signal, level_db, hz);
        state
    }

    fn set(&self, signal: AnalyzerSignal, level_db: f32, hz: f32) {
        // A level above 0 dBFS cannot be produced and would only clip, so the
        // ceiling is enforced here rather than trusted to the caller.
        let amplitude = 10.0_f32.powf(level_db.min(0.0) / 20.0);
        self.kind.store(signal as u32, Ordering::Relaxed);
        self.amplitude_bits
            .store(amplitude.to_bits(), Ordering::Relaxed);
        self.hz_bits.store(hz.max(0.0).to_bits(), Ordering::Relaxed);
        self.generation.fetch_add(1, Ordering::Release);
    }

    fn signal(&self) -> Signal {
        let amplitude = f32::from_bits(self.amplitude_bits.load(Ordering::Relaxed));
        let hz = f32::from_bits(self.hz_bits.load(Ordering::Relaxed));
        match self.kind.load(Ordering::Relaxed) {
            1 => Signal::Sine { hz, amplitude },
            2 => Signal::WhiteNoise { amplitude },
            3 => Signal::PinkNoise { amplitude },
            _ => Signal::Silence,
        }
    }
}

/// What the session computes.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum AnalyzerMode {
    /// Single-channel spectrum.
    #[default]
    Spectrum = 0,
    /// Two-channel transfer function, alongside the spectrum.
    Transfer = 1,
}

/// Where the transfer function's reference comes from.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum AnalyzerReference {
    /// The generator's own samples, captured alongside the input.
    ///
    /// Needs no loopback cable and works with a one-channel microphone, which
    /// is what makes a transfer function possible on a bare laptop. The cost is
    /// that it measures the acoustic path plus the converter round trip rather
    /// than the acoustic path alone, so the delay finder has to remove a delay
    /// it cannot know in advance.
    #[default]
    Internal = 0,
    /// A second input channel, fed from a physical loopback.
    ///
    /// More accurate: the converter's own latency and response appear in both
    /// channels and divide out.
    Input = 1,
}

/// Stimulus the generator produces.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum AnalyzerSignal {
    /// Generator off.
    #[default]
    Silence = 0,
    /// Steady sine, for distortion and calibration.
    Sine = 1,
    /// Equal energy per hertz.
    WhiteNoise = 2,
    /// Equal energy per octave. The usual transfer function stimulus.
    PinkNoise = 3,
}

/// A configuration filled with the defaults a UI should start from.
#[unsafe(no_mangle)]
pub extern "C" fn analyzer_session_config_default() -> AnalyzerSessionConfig {
    AnalyzerSessionConfig::default()
}

/// Metadata about the most recent frame.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct AnalyzerFrameInfo {
    /// Increments once per published frame.
    pub sequence: u64,
    /// Blocks the audio callback had to drop. Non-zero invalidates the
    /// measurement, and a UI is expected to say so rather than hide it.
    pub overruns: u64,
    /// Frames folded into the current average.
    pub frames_averaged: u32,
    /// Frames folded into the long-term average trace, which is what makes it
    /// trustworthy. A UI can report this rather than presenting a curve built
    /// from three frames as though it were settled.
    pub average_frames: u32,
    /// Rate the analysis ran at.
    pub sample_rate: f32,
    /// Hertz between adjacent bins.
    pub bin_spacing_hz: f32,
}

/// Harmonic orders reported across the boundary.
///
/// A fixed array keeps the struct POD with nothing for the caller to free. Ten
/// is what an audio measurement conventionally covers, and anything beyond it is
/// below the noise floor of any real system.
pub const ANALYZER_MAX_HARMONICS: usize = 10;

/// A distortion measurement.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct AnalyzerDistortion {
    /// Fundamental frequency found in the spectrum.
    pub fundamental_hz: f32,
    /// Its level.
    pub fundamental_db: f32,
    /// Total harmonic distortion as a percentage of the fundamental.
    pub thd_percent: f32,
    /// The same figure in decibels.
    pub thd_db: f32,
    /// Distortion plus noise: everything that is not the fundamental.
    pub thd_n_percent: f32,
    /// Median level of the bins that are neither fundamental nor harmonic.
    pub noise_floor_db: f32,
    /// How many entries of the harmonic arrays are populated.
    pub harmonic_count: u32,
    /// Orders that fall above Nyquist and were therefore not measured. Non-zero
    /// means the THD figure covers fewer orders than the full set.
    pub orders_above_nyquist: u32,
    /// Frequency of each harmonic found.
    pub harmonic_hz: [f32; ANALYZER_MAX_HARMONICS],
    /// Each harmonic as a percentage of the fundamental.
    pub harmonic_percent: [f32; ANALYZER_MAX_HARMONICS],
    /// Each harmonic relative to the fundamental, in decibels.
    pub harmonic_relative_db: [f32; ANALYZER_MAX_HARMONICS],
}

impl Default for AnalyzerDistortion {
    fn default() -> Self {
        Self {
            fundamental_hz: 0.0,
            fundamental_db: 0.0,
            thd_percent: 0.0,
            thd_db: 0.0,
            thd_n_percent: 0.0,
            noise_floor_db: 0.0,
            harmonic_count: 0,
            orders_above_nyquist: 0,
            harmonic_hz: [0.0; ANALYZER_MAX_HARMONICS],
            harmonic_percent: [0.0; ANALYZER_MAX_HARMONICS],
            harmonic_relative_db: [0.0; ANALYZER_MAX_HARMONICS],
        }
    }
}

/// A gridline.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct AnalyzerTick {
    /// Value in hertz or decibels.
    pub value: f32,
    /// Pixel position along the axis.
    pub position: f32,
    /// Whether it deserves a label.
    pub major: bool,
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

/// A running capture and analysis session.
///
/// Opaque across the boundary: cbindgen emits it as an incomplete type, so C
/// only ever holds a pointer.
pub struct AnalyzerSession {
    engine: Engine,
    stream: Box<dyn AudioStream>,
    frequency: FrequencyAxis,
    level: LevelAxis,
    reduction: Reduction,
    columns: usize,
    trace: Trace,
    average_trace: Trace,
    transfer_trace: Trace,
    /// Axis for phase in degrees, spanning the same pixels as the level axis.
    /// Held here so Swift never converts degrees to pixels itself.
    phase: LevelAxis,
    /// Axis for coherence, 0..1, over the same pixels again.
    coherence: LevelAxis,
    signal: Arc<SignalState>,
    /// The two equalisers are both kept, so switching between them does not
    /// throw away the one being left.
    graphic: Equaliser,
    parametric: Equaliser,
    eq_mode: AnalyzerEqMode,
    eq_publisher: SnapshotPublisher<EqCoefficients>,
    eq_generation: u64,
    /// Centre frequency of each pixel column, for evaluating the equaliser.
    /// Rebuilt only when the geometry changes.
    column_hz: Vec<f32>,
    eq_trace: Trace,
    bins: Vec<f32>,
    /// Scratch for the distortion analysis, which needs linear power.
    power: Vec<f32>,
    device_name: String,
}

impl std::fmt::Debug for AnalyzerSession {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("AnalyzerSession")
            .field("device", &self.device_name)
            .field("columns", &self.columns)
            .finish_non_exhaustive()
    }
}

/// Longest callback this session will process in one pass.
///
/// Buffers are sized for this once, at start. A device that hands over more
/// than this has the excess dropped, which costs a visible overrun; growing a
/// buffer on the audio thread would instead cost an audible one.
const MAX_CALLBACK_FRAMES: usize = 16_384;

/// Fixed so a run is reproducible. Noise that differs between runs makes two
/// measurements of the same room impossible to compare.
const GENERATOR_SEED: u64 = 0x5EED_5EED_5EED_5EED;

/// Start capturing and analysing. Returns null on failure, with `status`
/// describing why.
///
/// # Safety
///
/// `config` must point to a valid configuration; its `device_uid`, if non-null,
/// must be a NUL-terminated string. `status` must be null or writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_start(
    config: *const AnalyzerSessionConfig,
    status: *mut AnalyzerStatus,
) -> *mut AnalyzerSession {
    if config.is_null() {
        unsafe { set_status(status, AnalyzerStatus::failure("null configuration")) };
        return ptr::null_mut();
    }

    guard(ptr::null_mut(), || {
        let config = unsafe { *config };
        let uid = if config.device_uid.is_null() {
            None
        } else {
            match unsafe { std::ffi::CStr::from_ptr(config.device_uid) }.to_str() {
                Ok(text) => Some(text.to_owned()),
                Err(_) => {
                    unsafe {
                        set_status(
                            status,
                            AnalyzerStatus::failure("device uid is not valid UTF-8"),
                        );
                    }
                    return ptr::null_mut();
                }
            }
        };

        match start_session(&config, uid.as_deref()) {
            Ok(session) => {
                unsafe { set_status(status, AnalyzerStatus::ok()) };
                Box::into_raw(Box::new(session))
            }
            Err(message) => {
                unsafe { set_status(status, AnalyzerStatus::failure(&message)) };
                ptr::null_mut()
            }
        }
    })
}

fn start_session(
    config: &AnalyzerSessionConfig,
    uid: Option<&str>,
) -> Result<AnalyzerSession, String> {
    if config.fft_size < 2 || !config.fft_size.is_multiple_of(2) {
        return Err(format!(
            "fft size must be even and at least 2, got {}",
            config.fft_size
        ));
    }

    let mut backend = CoreAudioBackend::new();
    let device = match uid {
        Some(uid) => backend
            .devices()
            .map_err(|e| e.to_string())?
            .into_iter()
            .find(|d| d.id.as_str() == uid)
            .ok_or_else(|| format!("no device with uid '{uid}'"))?,
        None => backend
            .default_input()
            .map_err(|e| e.to_string())?
            .ok_or("no default input device")?,
    };

    if device.input_channels == 0 {
        return Err(format!("{} has no input channels", device.name));
    }
    if config.channel >= device.input_channels {
        return Err(format!(
            "channel {} requested but {} has {}",
            config.channel, device.name, device.input_channels
        ));
    }

    let inputs = device.input_channels as usize;
    let rate = device.default_sample_rate;

    let playing = config.signal != AnalyzerSignal::Silence;
    let output_channels: Vec<u32> = if playing {
        (0..device.output_channels)
            .filter(|c| config.output_mask & (1 << c) != 0)
            .collect()
    } else {
        Vec::new()
    };
    if playing && output_channels.is_empty() {
        return Err(format!(
            "a stimulus was requested but no output channel was selected; {} has {} output(s)",
            device.name, device.output_channels
        ));
    }

    // The internal reference is captured as one extra channel appended to the
    // device's own, so the stimulus travels through the same ring, in the same
    // block, as the audio it will be compared against. Nothing downstream can
    // then slide the two apart.
    let internal_reference =
        config.mode == AnalyzerMode::Transfer && config.reference == AnalyzerReference::Internal;
    let channels = inputs + usize::from(internal_reference);

    let mode = match config.mode {
        AnalyzerMode::Spectrum => AnalysisMode::Spectrum,
        AnalyzerMode::Transfer => {
            let reference_channel = if internal_reference {
                inputs
            } else {
                config.reference_channel as usize
            };
            if reference_channel >= channels {
                return Err(format!(
                    "reference channel {reference_channel} requested but {} has {inputs}",
                    device.name
                ));
            }
            if reference_channel == config.channel as usize {
                return Err(
                    "the reference and measurement channels must differ; the same channel \
                     against itself measures a wire, not a loudspeaker"
                        .into(),
                );
            }
            if internal_reference && !playing {
                return Err(
                    "an internal reference needs a stimulus to reference; choose a signal \
                     or wire a loopback into a second input"
                        .into(),
                );
            }
            AnalysisMode::Transfer {
                reference_channel,
                measurement_channel: config.channel as usize,
            }
        }
    };
    let hop = Overlap::from(config.overlap).hop(config.fft_size as usize);
    let frames_per_second = rate as f32 / hop as f32;

    let averaging = match config.averaging {
        AnalyzerAveraging::None => Averaging::None,
        AnalyzerAveraging::Fast => Averaging::exponential_over(1.0, frames_per_second),
        AnalyzerAveraging::Infinite => Averaging::Infinite,
        AnalyzerAveraging::PeakHold => Averaging::PeakHold,
    };

    let (mut sink, engine) = Engine::start(EngineConfig {
        channels,
        analysis_channel: config.channel as usize,
        spectrum: SpectrumConfig {
            sample_rate: rate as f32,
            size: config.fft_size as usize,
            window: config.window.into(),
            overlap: config.overlap.into(),
            averaging,
        },
        // The long-term trace always averages everything since its last reset.
        // Anything shorter would just be a second live trace.
        average: Averaging::Infinite,
        mode,
        ring_capacity_frames: 16_384,
    });

    let stream_config = StreamConfig {
        input: Some(DeviceId::new(device.id.as_str())),
        output: playing.then(|| DeviceId::new(device.id.as_str())),
        sample_rate: rate,
        buffer_frames: config.buffer_frames,
        input_channels: (0..inputs as u32).collect(),
        output_channels,
    };

    let signal = Arc::new(SignalState::new(
        config.signal,
        config.signal_level_db,
        config.signal_hz,
    ));

    let (eq_publisher, eq_reader) = snapshot_channel(EqCoefficients::default());
    let mut equaliser = EqProcessor::new(eq_reader);

    let mut generator = Generator::new(rate as f32, signal.signal(), GENERATOR_SEED);
    let mut stimulus = vec![0.0_f32; MAX_CALLBACK_FRAMES];
    // Only allocated when the reference has to be spliced in; the common
    // spectrum path writes the device's own buffer straight into the ring.
    let mut block = vec![
        0.0_f32;
        if internal_reference {
            MAX_CALLBACK_FRAMES * channels
        } else {
            0
        }
    ];
    let callback_signal = Arc::clone(&signal);
    let mut generation = callback_signal.generation.load(Ordering::Acquire);

    let mut stream = backend
        .open(
            &stream_config,
            Box::new(move |buffers: &mut AudioBuffers<'_>| {
                rt_section(|| {
                    // Clamped, not trusted. A device is free to hand over a
                    // larger block than it promised, and growing a buffer on
                    // this thread is exactly what must never happen.
                    let frames = buffers.frames().min(MAX_CALLBACK_FRAMES);

                    if callback_signal.generation.load(Ordering::Acquire) != generation {
                        generation = callback_signal.generation.load(Ordering::Acquire);
                        generator.set_signal(callback_signal.signal());
                    }

                    let stimulus = stimulus.get_mut(..frames).unwrap_or_default();
                    if playing {
                        generator.fill(stimulus);
                        // Equalise before anything sees it, so the reference
                        // channel carries what was actually played. Filtering
                        // only the output would make the transfer function
                        // report the equaliser's own curve as if it were the
                        // room's.
                        equaliser.process(stimulus);
                    } else {
                        stimulus.fill(0.0);
                    }

                    // Silence first so an oversized callback leaves no stale
                    // audio in the tail rather than playing it back.
                    buffers.silence_output();
                    let outputs = buffers.output_channels();
                    if outputs > 0 {
                        for (frame, sample) in buffers
                            .output_mut()
                            .chunks_exact_mut(outputs)
                            .zip(stimulus.iter())
                        {
                            // The same mono stimulus to every selected channel:
                            // a transfer function measures one path, and
                            // decorrelated noise between channels would make
                            // the room sum unpredictably.
                            frame.fill(*sample);
                        }
                    }

                    if internal_reference {
                        let input = buffers.input();
                        let stride = buffers.input_channels().max(1);
                        for ((out, src), sample) in block
                            .chunks_exact_mut(channels)
                            .zip(input.chunks_exact(stride))
                            .zip(stimulus.iter())
                        {
                            for (slot, value) in out.iter_mut().zip(src.iter()) {
                                *slot = *value;
                            }
                            if let Some(slot) = out.get_mut(stride) {
                                *slot = *sample;
                            }
                        }
                        sink.write_interleaved(block.get(..frames * channels).unwrap_or_default());
                    } else {
                        sink.write_interleaved(buffers.input());
                    }
                });
            }),
        )
        .map_err(|e| format!("opening {}: {e}", device.name))?;

    stream
        .start()
        .map_err(|e| format!("starting {}: {e}", device.name))?;

    Ok(AnalyzerSession {
        engine,
        stream,
        // Placeholder geometry. The UI calls analyzer_session_set_plot with the
        // real drawable size before drawing anything.
        frequency: FrequencyAxis::audible(1000.0),
        level: LevelAxis::full_scale(600.0),
        reduction: Reduction::Max,
        columns: 1000,
        trace: Trace::default(),
        average_trace: Trace::default(),
        transfer_trace: Trace::default(),
        phase: LevelAxis::new(-180.0, 180.0, 600.0),
        coherence: LevelAxis::new(0.0, 1.0, 600.0),
        signal,
        graphic: Equaliser::graphic(rate as f32),
        parametric: Equaliser::parametric(rate as f32),
        eq_mode: AnalyzerEqMode::Off,
        eq_publisher,
        eq_generation: 0,
        column_hz: Vec::new(),
        eq_trace: Trace::default(),
        bins: Vec::new(),
        power: Vec::new(),
        device_name: device.name,
    })
}

/// Stop and release a session. Safe to call with null.
///
/// # Safety
///
/// `session` must come from [`analyzer_session_start`] and not already be
/// destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_stop(session: *mut AnalyzerSession) {
    if session.is_null() {
        return;
    }
    guard((), || {
        let mut session = unsafe { Box::from_raw(session) };
        let _ = session.stream.stop();
        session.engine.stop();
    });
}

/// Set the plot geometry. Call on resize, on zoom, or when the reduction
/// changes. Returns false for degenerate geometry.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn analyzer_session_set_plot(
    session: *mut AnalyzerSession,
    width_px: f32,
    height_px: f32,
    min_hz: f32,
    max_hz: f32,
    min_db: f32,
    max_db: f32,
    reduction: AnalyzerReduction,
) -> bool {
    if session.is_null() {
        return false;
    }
    guard(false, || {
        // Reject degenerate geometry here rather than letting the axis
        // constructors panic across the boundary. Finiteness is checked
        // explicitly: a NaN arriving from a UI layout calculation would slip
        // past a bare comparison and poison every subsequent transform.
        let usable = width_px.is_finite()
            && height_px.is_finite()
            && min_hz.is_finite()
            && max_hz.is_finite()
            && min_db.is_finite()
            && max_db.is_finite()
            && width_px > 0.0
            && height_px > 0.0
            && min_hz > 0.0
            && max_hz > min_hz
            && max_db > min_db;
        if !usable {
            return false;
        }
        let session = unsafe { &mut *session };
        session.frequency = FrequencyAxis::new(min_hz, max_hz, width_px);
        session.level = LevelAxis::new(min_db, max_db, height_px);
        // Fixed ranges over the same pixels. Phase spans one full turn and
        // coherence spans its whole domain, so neither ever needs rescaling and
        // a UI cannot accidentally clip either.
        session.phase = LevelAxis::new(-180.0, 180.0, height_px);
        session.coherence = LevelAxis::new(0.0, 1.0, height_px);
        session.column_hz.clear();
        session.reduction = reduction.into();
        session.columns = width_px.round().max(1.0) as usize;
        true
    })
}

/// Whether a frame has arrived since the last [`analyzer_session_copy_trace`].
///
/// A UI can skip a redraw entirely when this is false, which is how idle CPU
/// stays near zero — a timer redrawing identical data is the real battery cost.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_has_new_frame(session: *const AnalyzerSession) -> bool {
    if session.is_null() {
        return false;
    }
    guard(false, || unsafe { (*session).engine.has_new_frame() })
}

/// Copy the reduced trace into `out`, returning how many values were written.
///
/// One value per pixel column, in decibels, at most `capacity` of them.
///
/// # Safety
///
/// `session` must be null or live; `out` must be null or point to at least
/// `capacity` writable floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_copy_trace(
    session: *mut AnalyzerSession,
    out: *mut f32,
    capacity: usize,
) -> usize {
    if session.is_null() || out.is_null() || capacity == 0 {
        return 0;
    }
    guard(0, || {
        let session = unsafe { &mut *session };
        // SAFETY: caller guarantees `capacity` writable floats.
        unsafe { copy_reduced(session, out, capacity, Which::Live) }
    })
}

/// Which trace a copy refers to.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Which {
    Live,
    Average,
}

/// Which transfer function curve a copy refers to.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum AnalyzerCurve {
    /// Magnitude in decibels. Maps through the ordinary level axis.
    #[default]
    Magnitude = 0,
    /// Phase in degrees, wrapped to -180..180. Maps through
    /// [`analyzer_phase_to_y`].
    Phase = 1,
    /// Coherence, 0..1. Maps through [`analyzer_coherence_to_y`].
    Coherence = 2,
}

/// Copy one transfer function curve, one value per pixel column.
///
/// Returns zero in spectrum mode, so a UI can call this unconditionally and
/// simply draw nothing.
///
/// Each curve is reduced in the domain it actually lives in. Magnitude is
/// decibels and goes through the session's chosen reduction. Coherence takes
/// the worst value in a column, because showing the best would hide the
/// dropouts a user is looking for. Phase is averaged as a direction, so a
/// column straddling the wrap reads 180 rather than 0.
///
/// # Safety
///
/// `session` must be null or live; `out` must be null or point to at least
/// `capacity` writable floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_copy_transfer(
    session: *mut AnalyzerSession,
    curve: AnalyzerCurve,
    out: *mut f32,
    capacity: usize,
) -> usize {
    if session.is_null() || out.is_null() || capacity == 0 {
        return 0;
    }
    guard(0, || {
        let session = unsafe { &mut *session };
        let columns = session.columns.min(capacity);

        let spacing = {
            let frame = session.engine.latest();
            if frame.transfer_frames == 0 {
                return 0;
            }
            session.bins.clear();
            session.bins.extend_from_slice(match curve {
                AnalyzerCurve::Magnitude => &frame.transfer_magnitude_db,
                AnalyzerCurve::Phase => &frame.transfer_phase_degrees,
                AnalyzerCurve::Coherence => &frame.transfer_coherence,
            });
            frame.bin_spacing_hz
        };

        match curve {
            AnalyzerCurve::Magnitude => reduce(
                &session.bins,
                spacing,
                &session.frequency,
                columns,
                session.reduction,
                &mut session.transfer_trace,
            ),
            AnalyzerCurve::Phase => reduce_linear(
                &session.bins,
                spacing,
                &session.frequency,
                columns,
                LinearReduction::Circular,
                0.0,
                &mut session.transfer_trace,
            ),
            AnalyzerCurve::Coherence => reduce_linear(
                &session.bins,
                spacing,
                &session.frequency,
                columns,
                LinearReduction::Min,
                0.0,
                &mut session.transfer_trace,
            ),
        }

        let written = session.transfer_trace.points.len().min(capacity);
        // SAFETY: caller guarantees `capacity` writable floats, written <= capacity.
        unsafe { ptr::copy_nonoverlapping(session.transfer_trace.points.as_ptr(), out, written) };
        written
    })
}

/// State of the transfer function.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct AnalyzerTransferInfo {
    /// Frames folded into the estimate. Zero means no transfer function is
    /// running, or none has been produced yet.
    ///
    /// Coherence is identically one for a single frame, so a UI should not
    /// present it as meaningful until this is comfortably above one.
    pub frames: u32,
    /// Delay currently removed from the reference, in samples.
    pub delay_frames: u32,
    /// The same delay in milliseconds.
    pub delay_ms: f32,
    /// Distance that delay corresponds to in air, in metres.
    pub delay_metres: f32,
    /// Whether a delay estimate has been asked for and not yet settled.
    pub estimating: bool,
}

/// Read the transfer function's state.
///
/// # Safety
///
/// `session` must be null or live; `out` must be null or writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_transfer_info(
    session: *mut AnalyzerSession,
    out: *mut AnalyzerTransferInfo,
) -> bool {
    if session.is_null() || out.is_null() {
        return false;
    }
    guard(false, || {
        let session = unsafe { &mut *session };
        let delay = session.engine.reference_delay();
        let rate = session.engine.latest().sample_rate.max(1.0);
        let seconds = delay as f32 / rate;
        let info = AnalyzerTransferInfo {
            frames: session.engine.latest().transfer_frames,
            delay_frames: delay,
            delay_ms: seconds * 1000.0,
            // 343 m/s, the conventional figure at 20 C.
            delay_metres: seconds * 343.0,
            estimating: session.engine.delay_estimate_pending(),
        };
        unsafe { *out = info };
        true
    })
}

/// Ask the core to measure the reference-to-measurement delay and remove it.
///
/// Returns immediately. The estimate needs signal to work with, so it settles
/// over the next fraction of a second and appears in
/// [`analyzer_session_transfer_info`]; asking during silence waits rather than
/// answering zero.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_estimate_delay(session: *mut AnalyzerSession) -> bool {
    if session.is_null() {
        return false;
    }
    guard(false, || {
        unsafe { &*session }.engine.estimate_reference_delay();
        true
    })
}

/// Set the reference delay by hand, in samples.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_set_delay(
    session: *mut AnalyzerSession,
    frames: u32,
) -> bool {
    if session.is_null() {
        return false;
    }
    guard(false, || {
        unsafe { &*session }.engine.set_reference_delay(frames);
        true
    })
}

/// Change the stimulus while running.
///
/// Only takes effect if the session was started with a signal: opening an
/// output stream is a device operation and cannot happen from here. A session
/// started silent stays silent, which is why a UI offering a generator should
/// start one even when the initial choice is silence.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_set_signal(
    session: *mut AnalyzerSession,
    signal: AnalyzerSignal,
    level_db: f32,
    hz: f32,
) -> bool {
    if session.is_null() || !level_db.is_finite() || !hz.is_finite() {
        return false;
    }
    guard(false, || {
        unsafe { &*session }.signal.set(signal, level_db, hz);
        true
    })
}

impl AnalyzerSession {
    /// The equaliser the current mode selects.
    fn equaliser(&self) -> Option<&Equaliser> {
        match self.eq_mode {
            AnalyzerEqMode::Off => None,
            AnalyzerEqMode::Graphic => Some(&self.graphic),
            AnalyzerEqMode::Parametric => Some(&self.parametric),
        }
    }

    fn equaliser_mut(&mut self) -> Option<&mut Equaliser> {
        match self.eq_mode {
            AnalyzerEqMode::Off => None,
            AnalyzerEqMode::Graphic => Some(&mut self.graphic),
            AnalyzerEqMode::Parametric => Some(&mut self.parametric),
        }
    }

    /// Push the active equaliser's coefficients to the audio thread.
    ///
    /// Called after every change. Cheap - a couple of dozen biquad designs -
    /// and it happens on a UI event, not per frame.
    fn publish_eq(&mut self) {
        self.eq_generation += 1;
        let generation = self.eq_generation;
        let coefficients = match self.equaliser() {
            Some(eq) => EqCoefficients::from_equaliser(generation, eq),
            None => EqCoefficients {
                generation,
                ..EqCoefficients::default()
            },
        };
        self.eq_publisher.publish_with(|slot| *slot = coefficients);
    }

    /// Centre frequency of every pixel column, cached.
    fn columns_hz(&mut self, columns: usize) -> &[f32] {
        if self.column_hz.len() != columns {
            self.column_hz.clear();
            let width = self.frequency.width();
            self.column_hz.extend((0..columns).map(|index| {
                // The column centre, matching where `reduce` samples, so an
                // equaliser curve and a measured curve line up.
                let x = (index as f32 + 0.5) * width / columns as f32;
                self.frequency.x_to_freq(x)
            }));
        }
        &self.column_hz
    }
}

/// Choose which equaliser is active. Returns false for a null session.
///
/// Both equalisers are kept across a switch, so moving to the parametric and
/// back does not lose the graphic's fader positions.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_set_eq_mode(
    session: *mut AnalyzerSession,
    mode: AnalyzerEqMode,
) -> bool {
    if session.is_null() {
        return false;
    }
    guard(false, || {
        let session = unsafe { &mut *session };
        session.eq_mode = mode;
        session.publish_eq();
        true
    })
}

/// Bands the active equaliser has. Zero when it is off.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_eq_band_count(session: *const AnalyzerSession) -> usize {
    if session.is_null() {
        return 0;
    }
    guard(0, || {
        unsafe { &*session }
            .equaliser()
            .map_or(0, |eq| eq.bands().len())
    })
}

/// Read one band.
///
/// # Safety
///
/// `session` must be null or live; `out` must be null or writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_eq_get_band(
    session: *const AnalyzerSession,
    index: usize,
    out: *mut AnalyzerBand,
) -> bool {
    if session.is_null() || out.is_null() {
        return false;
    }
    guard(false, || {
        let Some(band) = unsafe { &*session }
            .equaliser()
            .and_then(|eq| eq.bands().get(index).copied())
        else {
            return false;
        };
        unsafe { *out = band.into() };
        true
    })
}

/// Replace one band.
///
/// # Safety
///
/// `session` must be null or live; `band` must be null or readable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_eq_set_band(
    session: *mut AnalyzerSession,
    index: usize,
    band: *const AnalyzerBand,
) -> bool {
    if session.is_null() || band.is_null() {
        return false;
    }
    guard(false, || {
        let band: FilterBand = unsafe { *band }.into();
        if !band.hz.is_finite() || !band.gain_db.is_finite() || !band.q.is_finite() {
            return false;
        }
        let session = unsafe { &mut *session };
        let Some(eq) = session.equaliser_mut() else {
            return false;
        };
        if index >= eq.bands().len() {
            return false;
        }
        eq.set_band(index, band);
        session.publish_eq();
        true
    })
}

/// Set one band's gain, which is all a graphic equaliser can change.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_eq_set_gain(
    session: *mut AnalyzerSession,
    index: usize,
    gain_db: f32,
) -> bool {
    if session.is_null() || !gain_db.is_finite() {
        return false;
    }
    guard(false, || {
        let session = unsafe { &mut *session };
        let Some(eq) = session.equaliser_mut() else {
            return false;
        };
        if index >= eq.bands().len() {
            return false;
        }
        eq.set_gain_db(index, gain_db);
        session.publish_eq();
        true
    })
}

/// Append a band to the parametric equaliser. Returns its index, or -1.
///
/// # Safety
///
/// `session` must be null or live; `band` must be null or readable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_eq_add_band(
    session: *mut AnalyzerSession,
    band: *const AnalyzerBand,
) -> isize {
    if session.is_null() || band.is_null() {
        return -1;
    }
    guard(-1, || {
        let band: FilterBand = unsafe { *band }.into();
        if !band.hz.is_finite() || !band.gain_db.is_finite() || !band.q.is_finite() {
            return -1;
        }
        let session = unsafe { &mut *session };
        let Some(eq) = session.equaliser_mut() else {
            return -1;
        };
        // The audio thread's coefficient array is fixed, so a band beyond it
        // would silently not be heard. Refusing is the honest answer.
        if eq.bands().len() >= ANALYZER_MAX_EQ_BANDS {
            return -1;
        }
        let index = eq.push_band(band);
        session.publish_eq();
        index as isize
    })
}

/// Remove a band from the parametric equaliser.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_eq_remove_band(
    session: *mut AnalyzerSession,
    index: usize,
) -> bool {
    if session.is_null() {
        return false;
    }
    guard(false, || {
        let session = unsafe { &mut *session };
        let Some(eq) = session.equaliser_mut() else {
            return false;
        };
        if index >= eq.bands().len() {
            return false;
        }
        eq.remove_band(index);
        session.publish_eq();
        true
    })
}

/// Set every gain to zero and clear the trim.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_eq_flatten(session: *mut AnalyzerSession) -> bool {
    if session.is_null() {
        return false;
    }
    guard(false, || {
        let session = unsafe { &mut *session };
        let Some(eq) = session.equaliser_mut() else {
            return false;
        };
        eq.flatten();
        session.publish_eq();
        true
    })
}

/// Trim the output so the equaliser's loudest point sits at unity.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_eq_trim(session: *mut AnalyzerSession) -> bool {
    if session.is_null() {
        return false;
    }
    guard(false, || {
        let session = unsafe { &mut *session };
        let Some(eq) = session.equaliser_mut() else {
            return false;
        };
        eq.trim_to_unity();
        session.publish_eq();
        true
    })
}

/// Set the output trim by hand, in decibels.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_eq_set_preamp(
    session: *mut AnalyzerSession,
    db: f32,
) -> bool {
    if session.is_null() || !db.is_finite() {
        return false;
    }
    guard(false, || {
        let session = unsafe { &mut *session };
        let Some(eq) = session.equaliser_mut() else {
            return false;
        };
        eq.set_preamp_db(db);
        session.publish_eq();
        true
    })
}

/// Headroom figures for the active equaliser.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct AnalyzerEqInfo {
    /// Bands the active equaliser holds.
    pub band_count: usize,
    /// Largest gain applied anywhere in the audio band, in decibels.
    ///
    /// Bands add, so this can far exceed any single band's gain. A UI showing
    /// it next to a trim control is the difference between an equaliser that is
    /// safe to use and one that clips without saying so.
    pub peak_gain_db: f32,
    /// Output trim, in decibels.
    pub preamp_db: f32,
    /// Whether an equaliser is active at all.
    pub active: bool,
}

/// Read the active equaliser's headroom.
///
/// # Safety
///
/// `session` must be null or live; `out` must be null or writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_eq_info(
    session: *const AnalyzerSession,
    out: *mut AnalyzerEqInfo,
) -> bool {
    if session.is_null() || out.is_null() {
        return false;
    }
    guard(false, || {
        let info = match unsafe { &*session }.equaliser() {
            Some(eq) => AnalyzerEqInfo {
                band_count: eq.bands().len(),
                peak_gain_db: eq.peak_gain_db(),
                preamp_db: eq.preamp_db(),
                active: true,
            },
            None => AnalyzerEqInfo::default(),
        };
        unsafe { *out = info };
        true
    })
}

/// Copy the equaliser's own curve, one value per pixel column, in decibels.
///
/// Pass `band` of -1 for the combined curve, or a band index for that band
/// alone. Returns zero when no equaliser is active.
///
/// This is arithmetic on the coefficients, not a measurement: it works whether
/// or not audio is running, which is what makes designing a correction against
/// a saved measurement possible.
///
/// # Safety
///
/// `session` must be null or live; `out` must be null or point to at least
/// `capacity` writable floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_copy_eq_curve(
    session: *mut AnalyzerSession,
    band: isize,
    out: *mut f32,
    capacity: usize,
) -> usize {
    if session.is_null() || out.is_null() || capacity == 0 {
        return 0;
    }
    guard(0, || {
        let session = unsafe { &mut *session };
        if session.equaliser().is_none() {
            return 0;
        }
        let columns = session.columns.min(capacity);
        session.columns_hz(columns);

        session.eq_trace.points.clear();
        session.eq_trace.points.reserve(columns);
        // Split the borrow: the curve reads the equaliser while the cache is
        // read and the trace is written.
        let (graphic, parametric) = (&session.graphic, &session.parametric);
        let eq = match session.eq_mode {
            AnalyzerEqMode::Off => return 0,
            AnalyzerEqMode::Graphic => graphic,
            AnalyzerEqMode::Parametric => parametric,
        };
        for hz in session.column_hz.iter().take(columns) {
            session.eq_trace.points.push(if band < 0 {
                eq.magnitude_db_at(*hz)
            } else {
                eq.band_magnitude_db_at(band as usize, *hz)
            });
        }

        let written = session.eq_trace.points.len().min(capacity);
        // SAFETY: caller guarantees `capacity` writable floats, written <= capacity.
        unsafe { ptr::copy_nonoverlapping(session.eq_trace.points.as_ptr(), out, written) };
        written
    })
}

/// Copy the measured trace with the equaliser applied.
///
/// The point of an equaliser in a measurement tool: what the room would look
/// like after the correction, drawn beside what it looks like now. Adding
/// decibels is exact here because the equaliser's curve is known analytically
/// rather than measured.
///
/// # Safety
///
/// `session` must be null or live; `out` must be null or point to at least
/// `capacity` writable floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_copy_corrected(
    session: *mut AnalyzerSession,
    out: *mut f32,
    capacity: usize,
) -> usize {
    if session.is_null() || out.is_null() || capacity == 0 {
        return 0;
    }
    guard(0, || {
        let session = unsafe { &mut *session };
        if session.equaliser().is_none() {
            return 0;
        }
        // SAFETY: caller guarantees `capacity` writable floats.
        let written = unsafe { copy_reduced(session, out, capacity, Which::Live) };
        if written == 0 {
            return 0;
        }
        session.columns_hz(written);

        let (graphic, parametric) = (&session.graphic, &session.parametric);
        let eq = match session.eq_mode {
            AnalyzerEqMode::Off => return 0,
            AnalyzerEqMode::Graphic => graphic,
            AnalyzerEqMode::Parametric => parametric,
        };
        for (index, hz) in session.column_hz.iter().take(written).enumerate() {
            // SAFETY: index < written <= capacity.
            unsafe {
                let slot = out.add(index);
                *slot += eq.magnitude_db_at(*hz);
            }
        }
        written
    })
}

/// Map a phase in degrees to a pixel row.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_phase_to_y(session: *const AnalyzerSession, degrees: f32) -> f32 {
    if session.is_null() {
        return f32::NAN;
    }
    guard(f32::NAN, || unsafe { &*session }.phase.db_to_y(degrees))
}

/// Map a coherence value to a pixel row.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_coherence_to_y(
    session: *const AnalyzerSession,
    value: f32,
) -> f32 {
    if session.is_null() {
        return f32::NAN;
    }
    guard(f32::NAN, || unsafe { &*session }.coherence.db_to_y(value))
}

/// Copy the long-term average trace, one value per pixel column.
///
/// Uses the same geometry and reduction as [`analyzer_session_copy_trace`], so
/// the two curves overlay exactly rather than being subtly offset from each
/// other.
///
/// # Safety
///
/// `session` must be null or live; `out` must be null or point to at least
/// `capacity` writable floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_copy_average(
    session: *mut AnalyzerSession,
    out: *mut f32,
    capacity: usize,
) -> usize {
    if session.is_null() || out.is_null() || capacity == 0 {
        return 0;
    }
    guard(0, || {
        let session = unsafe { &mut *session };
        // SAFETY: caller guarantees `capacity` writable floats.
        unsafe { copy_reduced(session, out, capacity, Which::Average) }
    })
}

/// Restart the long-term average, leaving the live trace running.
///
/// Takes effect on the analysis thread's next pass, so the very next frame may
/// still show the old average.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_reset_average(session: *mut AnalyzerSession) -> bool {
    if session.is_null() {
        return false;
    }
    guard(false, || {
        unsafe { (*session).engine.reset_average() };
        true
    })
}

/// Shared body of the two copy calls.
///
/// # Safety
///
/// `out` must point to at least `capacity` writable floats.
unsafe fn copy_reduced(
    session: &mut AnalyzerSession,
    out: *mut f32,
    capacity: usize,
    which: Which,
) -> usize {
    let columns = session.columns.min(capacity);

    // Copy the bins out before reducing: `latest` borrows the engine, and the
    // reduction needs a mutable borrow of the session's scratch trace.
    let spacing = {
        let frame = session.engine.latest();
        session.bins.clear();
        session.bins.extend_from_slice(match which {
            Which::Live => &frame.bins,
            Which::Average => &frame.average_bins,
        });
        frame.bin_spacing_hz
    };

    let target = match which {
        Which::Live => &mut session.trace,
        Which::Average => &mut session.average_trace,
    };
    reduce(
        &session.bins,
        spacing,
        &session.frequency,
        columns,
        session.reduction,
        target,
    );

    let written = target.points.len().min(capacity);
    // SAFETY: caller guarantees `capacity` writable floats, and written <= capacity.
    unsafe { ptr::copy_nonoverlapping(target.points.as_ptr(), out, written) };
    written
}

/// Read metadata about the newest frame.
///
/// # Safety
///
/// `session` must be null or live; `out` must be null or writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_frame_info(
    session: *mut AnalyzerSession,
    out: *mut AnalyzerFrameInfo,
) -> bool {
    if session.is_null() || out.is_null() {
        return false;
    }
    guard(false, || unsafe {
        let frame = (*session).engine.latest();
        ptr::write(
            out,
            AnalyzerFrameInfo {
                sequence: frame.sequence,
                overruns: frame.overruns,
                frames_averaged: frame.frames_averaged,
                average_frames: frame.average_frames,
                sample_rate: frame.sample_rate,
                bin_spacing_hz: frame.bin_spacing_hz,
            },
        );
        true
    })
}

/// Measure harmonic distortion in the current live spectrum.
///
/// Returns false when no fundamental stands far enough above the noise floor for
/// the figure to mean anything — distortion of a room's background hiss is not a
/// number worth showing, and a UI should hide the readout rather than display
/// a plausible-looking one.
///
/// `fundamental_hz` may be zero to use the loudest peak, or set explicitly when
/// the stimulus frequency is known.
///
/// # Safety
///
/// `session` must be null or live; `out` must be null or writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_distortion(
    session: *mut AnalyzerSession,
    fundamental_hz: f32,
    out: *mut AnalyzerDistortion,
) -> bool {
    if session.is_null() || out.is_null() {
        return false;
    }
    guard(false, || {
        let session = unsafe { &mut *session };

        // The frame carries decibels; the analysis needs linear power. The dB
        // came from power in the first place, so this inverts exactly the
        // conversion that produced it. Round-tripping through f32 dB costs far
        // less precision than the measurement itself has.
        let spacing = {
            let frame = session.engine.latest();
            session.power.clear();
            session
                .power
                .extend(frame.bins.iter().map(|db| 10.0_f32.powf(db / 10.0) / 2.0));
            frame.bin_spacing_hz
        };

        let hint = (fundamental_hz > 0.0).then_some(fundamental_hz);
        let Some(result) = analyzer_dsp::distortion::analyse(
            &session.power,
            spacing,
            hint,
            &DistortionConfig::default(),
        ) else {
            return false;
        };

        // A fundamental buried in the floor makes every derived figure noise.
        const MINIMUM_HEADROOM_DB: f32 = 20.0;
        if result.fundamental_db < result.noise_floor_db + MINIMUM_HEADROOM_DB {
            return false;
        }

        let mut value = AnalyzerDistortion {
            fundamental_hz: result.fundamental_hz,
            fundamental_db: result.fundamental_db,
            thd_percent: result.thd_percent,
            thd_db: result.thd_db,
            thd_n_percent: result.thd_n_percent,
            noise_floor_db: result.noise_floor_db,
            harmonic_count: 0,
            orders_above_nyquist: result.orders_above_nyquist,
            ..AnalyzerDistortion::default()
        };
        for (index, harmonic) in result
            .harmonics
            .iter()
            .take(ANALYZER_MAX_HARMONICS)
            .enumerate()
        {
            value.harmonic_hz[index] = harmonic.hz;
            value.harmonic_percent[index] = harmonic.percent;
            value.harmonic_relative_db[index] = harmonic.relative_db;
            value.harmonic_count += 1;
        }

        // SAFETY: `out` is non-null and writable per the contract.
        unsafe { ptr::write(out, value) };
        true
    })
}

/// Save the current spectrum to a measurement file.
///
/// Writes a magnitude-only measurement, because that is what an RTA produces -
/// squaring the magnitude discarded the phase, and a file claiming zero phase
/// would be indistinguishable from one that measured it.
///
/// Whether the levels are dB SPL or dBFS is recorded rather than assumed:
/// `spl_offset_db` is stored when non-zero and omitted otherwise, so a reader
/// can tell a calibrated measurement from an uncalibrated one.
///
/// # Safety
///
/// `session` must be null or live. `path` and `name` must be NUL-terminated
/// strings. `status` must be null or writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_save_measurement(
    session: *mut AnalyzerSession,
    path: *const c_char,
    name: *const c_char,
    spl_offset_db: f32,
    status: *mut AnalyzerStatus,
) -> bool {
    if session.is_null() || path.is_null() {
        unsafe { set_status(status, AnalyzerStatus::failure("null session or path")) };
        return false;
    }
    guard(false, || {
        let path = match unsafe { std::ffi::CStr::from_ptr(path) }.to_str() {
            Ok(text) => text.to_owned(),
            Err(_) => {
                unsafe { set_status(status, AnalyzerStatus::failure("path is not valid UTF-8")) };
                return false;
            }
        };
        let label = if name.is_null() {
            "Measurement".to_owned()
        } else {
            unsafe { std::ffi::CStr::from_ptr(name) }
                .to_str()
                .unwrap_or("Measurement")
                .to_owned()
        };

        let session = unsafe { &mut *session };
        let (magnitude_db, bin_spacing_hz, sample_rate) = {
            let frame = session.engine.latest();
            (
                frame
                    .bins
                    .iter()
                    .map(|db| f64::from(*db))
                    .collect::<Vec<f64>>(),
                f64::from(frame.bin_spacing_hz),
                f64::from(frame.sample_rate),
            )
        };
        if magnitude_db.is_empty() {
            unsafe { set_status(status, AnalyzerStatus::failure("nothing captured yet")) };
            return false;
        }

        let mut measurement = Measurement::new(
            MeasurementId(0),
            label,
            sample_rate,
            MeasurementData::PowerSpectrum {
                magnitude_db,
                bin_spacing_hz,
            },
        );
        measurement.references = References {
            // Zero means uncalibrated here, which stays None - "not measured"
            // and "measured as needing no correction" are different facts.
            spl_offset_db: (spl_offset_db != 0.0).then(|| f64::from(spl_offset_db)),
            ..References::default()
        };

        match std::fs::write(&path, analyzer_model::write(&measurement)) {
            Ok(()) => {
                unsafe { set_status(status, AnalyzerStatus::ok()) };
                true
            }
            Err(error) => {
                unsafe {
                    set_status(
                        status,
                        AnalyzerStatus::failure(&format!("writing {path}: {error}")),
                    );
                }
                false
            }
        }
    })
}

/// Export the current spectrum as REW-compatible text.
///
/// # Safety
///
/// As [`analyzer_session_save_measurement`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_export_text(
    session: *mut AnalyzerSession,
    path: *const c_char,
    name: *const c_char,
    spl_offset_db: f32,
    status: *mut AnalyzerStatus,
) -> bool {
    if session.is_null() || path.is_null() {
        unsafe { set_status(status, AnalyzerStatus::failure("null session or path")) };
        return false;
    }
    guard(false, || {
        let path = match unsafe { std::ffi::CStr::from_ptr(path) }.to_str() {
            Ok(text) => text.to_owned(),
            Err(_) => return false,
        };
        let label = if name.is_null() {
            "Measurement".to_owned()
        } else {
            unsafe { std::ffi::CStr::from_ptr(name) }
                .to_str()
                .unwrap_or("Measurement")
                .to_owned()
        };

        let session = unsafe { &mut *session };
        let (magnitude_db, bin_spacing_hz, sample_rate) = {
            let frame = session.engine.latest();
            (
                frame
                    .bins
                    .iter()
                    .map(|db| f64::from(*db))
                    .collect::<Vec<f64>>(),
                f64::from(frame.bin_spacing_hz),
                f64::from(frame.sample_rate),
            )
        };

        let mut measurement = Measurement::new(
            MeasurementId(0),
            label,
            sample_rate,
            MeasurementData::PowerSpectrum {
                magnitude_db,
                bin_spacing_hz,
            },
        );
        measurement.references.spl_offset_db =
            (spl_offset_db != 0.0).then(|| f64::from(spl_offset_db));

        match std::fs::write(&path, analyzer_model::export::to_text(&measurement)) {
            Ok(()) => {
                unsafe { set_status(status, AnalyzerStatus::ok()) };
                true
            }
            Err(error) => {
                unsafe {
                    set_status(
                        status,
                        AnalyzerStatus::failure(&format!("writing {path}: {error}")),
                    );
                }
                false
            }
        }
    })
}

/// Copy the captured device's name into a caller buffer, returning its length.
///
/// # Safety
///
/// `session` must be null or live; `out` must point to at least `capacity`
/// writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_session_device_name(
    session: *const AnalyzerSession,
    out: *mut c_char,
    capacity: usize,
) -> usize {
    if session.is_null() || out.is_null() || capacity == 0 {
        return 0;
    }
    guard(0, || unsafe {
        let name = &(*session).device_name;
        let mut end = name.len().min(capacity - 1);
        while end > 0 && !name.is_char_boundary(end) {
            end -= 1;
        }
        for (i, byte) in name.as_bytes().iter().take(end).enumerate() {
            ptr::write(out.add(i), *byte as c_char);
        }
        ptr::write(out.add(end), 0);
        end
    })
}

// ---------------------------------------------------------------------------
// Axis queries
//
// These exist so the UI never reimplements the mapping. Cursor readout,
// hit-testing and the drawn geometry must agree exactly, and duplicating the
// maths in Swift is how they quietly stop agreeing.
// ---------------------------------------------------------------------------

/// Pixel position of a frequency. NaN if the session is null.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_freq_to_x(session: *const AnalyzerSession, hz: f32) -> f32 {
    if session.is_null() {
        return f32::NAN;
    }
    guard(f32::NAN, || unsafe { (*session).frequency.freq_to_x(hz) })
}

/// Frequency at a pixel position. NaN if the session is null.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_x_to_freq(session: *const AnalyzerSession, x: f32) -> f32 {
    if session.is_null() {
        return f32::NAN;
    }
    guard(f32::NAN, || unsafe { (*session).frequency.x_to_freq(x) })
}

/// Pixel position of a level, zero being the top. NaN if the session is null.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_db_to_y(session: *const AnalyzerSession, db: f32) -> f32 {
    if session.is_null() {
        return f32::NAN;
    }
    guard(f32::NAN, || unsafe { (*session).level.db_to_y(db) })
}

/// Level at a pixel position. NaN if the session is null.
///
/// # Safety
///
/// `session` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_y_to_db(session: *const AnalyzerSession, y: f32) -> f32 {
    if session.is_null() {
        return f32::NAN;
    }
    guard(f32::NAN, || unsafe { (*session).level.y_to_db(y) })
}

/// Copy frequency gridlines into `out`, returning how many were written.
///
/// # Safety
///
/// `session` must be null or live; `out` must hold at least `capacity` ticks.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_frequency_ticks(
    session: *const AnalyzerSession,
    out: *mut AnalyzerTick,
    capacity: usize,
) -> usize {
    if session.is_null() || out.is_null() || capacity == 0 {
        return 0;
    }
    guard(0, || unsafe {
        let ticks = (*session).frequency.ticks();
        let written = ticks.len().min(capacity);
        for (i, tick) in ticks.iter().take(written).enumerate() {
            ptr::write(
                out.add(i),
                AnalyzerTick {
                    value: tick.value,
                    position: tick.position,
                    major: tick.major,
                },
            );
        }
        written
    })
}

/// Copy level gridlines every `step_db` into `out`.
///
/// # Safety
///
/// `session` must be null or live; `out` must hold at least `capacity` ticks.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn analyzer_level_ticks(
    session: *const AnalyzerSession,
    step_db: f32,
    out: *mut AnalyzerTick,
    capacity: usize,
) -> usize {
    if session.is_null() || out.is_null() || capacity == 0 || !step_db.is_finite() {
        return 0;
    }
    if step_db <= 0.0 {
        return 0;
    }
    guard(0, || unsafe {
        let ticks = (*session).level.ticks(step_db);
        let written = ticks.len().min(capacity);
        for (i, tick) in ticks.iter().take(written).enumerate() {
            ptr::write(
                out.add(i),
                AnalyzerTick {
                    value: tick.value,
                    position: tick.position,
                    major: tick.major,
                },
            );
        }
        written
    })
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    /// Every entry point must survive a null handle. A UI that has not started a
    /// session yet will call these, and crashing is not an acceptable answer.
    #[test]
    fn null_handles_are_tolerated_everywhere() {
        unsafe {
            analyzer_device_list_destroy(ptr::null_mut());
            assert_eq!(analyzer_device_list_count(ptr::null()), 0);
            assert!(!analyzer_device_list_get(ptr::null(), 0, ptr::null_mut()));

            analyzer_session_stop(ptr::null_mut());
            assert_eq!(
                analyzer_session_copy_transfer(
                    ptr::null_mut(),
                    AnalyzerCurve::Magnitude,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert!(!analyzer_session_transfer_info(
                ptr::null_mut(),
                ptr::null_mut()
            ));
            assert!(!analyzer_session_estimate_delay(ptr::null_mut()));
            assert!(!analyzer_session_set_delay(ptr::null_mut(), 0));
            assert!(!analyzer_session_set_signal(
                ptr::null_mut(),
                AnalyzerSignal::PinkNoise,
                -20.0,
                1000.0
            ));
            assert!(!analyzer_session_set_eq_mode(
                ptr::null_mut(),
                AnalyzerEqMode::Graphic
            ));
            assert_eq!(analyzer_session_eq_band_count(ptr::null()), 0);
            assert!(!analyzer_session_eq_get_band(
                ptr::null(),
                0,
                ptr::null_mut()
            ));
            assert!(!analyzer_session_eq_set_band(
                ptr::null_mut(),
                0,
                ptr::null()
            ));
            assert!(!analyzer_session_eq_set_gain(ptr::null_mut(), 0, 0.0));
            assert_eq!(
                analyzer_session_eq_add_band(ptr::null_mut(), ptr::null()),
                -1
            );
            assert!(!analyzer_session_eq_remove_band(ptr::null_mut(), 0));
            assert!(!analyzer_session_eq_flatten(ptr::null_mut()));
            assert!(!analyzer_session_eq_trim(ptr::null_mut()));
            assert!(!analyzer_session_eq_set_preamp(ptr::null_mut(), 0.0));
            assert!(!analyzer_session_eq_info(ptr::null(), ptr::null_mut()));
            assert_eq!(
                analyzer_session_copy_eq_curve(ptr::null_mut(), -1, ptr::null_mut(), 0),
                0
            );
            assert_eq!(
                analyzer_session_copy_corrected(ptr::null_mut(), ptr::null_mut(), 0),
                0
            );
            assert!(analyzer_phase_to_y(ptr::null(), 0.0).is_nan());
            assert!(analyzer_coherence_to_y(ptr::null(), 0.0).is_nan());
            assert!(!analyzer_session_has_new_frame(ptr::null()));
            assert_eq!(
                analyzer_session_copy_trace(ptr::null_mut(), ptr::null_mut(), 0),
                0
            );
            assert_eq!(
                analyzer_session_copy_average(ptr::null_mut(), ptr::null_mut(), 0),
                0
            );
            assert!(!analyzer_session_reset_average(ptr::null_mut()));
            assert!(!analyzer_session_distortion(
                ptr::null_mut(),
                0.0,
                ptr::null_mut()
            ));
            assert!(!analyzer_session_save_measurement(
                ptr::null_mut(),
                ptr::null(),
                ptr::null(),
                0.0,
                ptr::null_mut()
            ));
            assert!(!analyzer_session_export_text(
                ptr::null_mut(),
                ptr::null(),
                ptr::null(),
                0.0,
                ptr::null_mut()
            ));
            assert!(!analyzer_session_frame_info(
                ptr::null_mut(),
                ptr::null_mut()
            ));
            assert_eq!(
                analyzer_session_device_name(ptr::null(), ptr::null_mut(), 0),
                0
            );
            assert!(!analyzer_session_set_plot(
                ptr::null_mut(),
                100.0,
                100.0,
                20.0,
                20_000.0,
                -120.0,
                0.0,
                AnalyzerReduction::Max
            ));
            assert!(analyzer_freq_to_x(ptr::null(), 1000.0).is_nan());
            assert!(analyzer_x_to_freq(ptr::null(), 100.0).is_nan());
            assert!(analyzer_db_to_y(ptr::null(), -20.0).is_nan());
            assert!(analyzer_y_to_db(ptr::null(), 100.0).is_nan());
            assert_eq!(analyzer_frequency_ticks(ptr::null(), ptr::null_mut(), 0), 0);
            assert_eq!(
                analyzer_level_ticks(ptr::null(), 10.0, ptr::null_mut(), 0),
                0
            );
        }
    }

    #[test]
    fn starting_with_a_null_config_reports_rather_than_crashes() {
        let mut status = AnalyzerStatus::default();
        let session = unsafe { analyzer_session_start(ptr::null(), &mut status) };
        assert!(session.is_null());
        assert_ne!(status.code, 0);
    }

    #[test]
    fn status_messages_round_trip_and_truncate_safely() {
        let status = AnalyzerStatus::failure("device not found");
        assert_eq!(status.code, 1);
        assert_eq!(read_message(&status), "device not found");

        // Over-long multi-byte input must not leave a broken buffer behind.
        let long = "é".repeat(500);
        let status = AnalyzerStatus::failure(&long);
        let text = read_message(&status);
        assert!(text.len() < ANALYZER_MESSAGE_LEN);
        assert!(text.chars().all(|c| c == 'é'), "truncated mid-character");
    }

    fn read_message(status: &AnalyzerStatus) -> String {
        let bytes: Vec<u8> = status
            .message
            .iter()
            .take_while(|c| **c != 0)
            .map(|c| *c as u8)
            .collect();
        String::from_utf8(bytes).expect("message must stay valid UTF-8")
    }

    #[test]
    fn a_successful_status_is_empty() {
        let status = AnalyzerStatus::ok();
        assert_eq!(status.code, 0);
        assert_eq!(status.message[0], 0);
    }

    #[test]
    fn default_config_is_usable() {
        let config = analyzer_session_config_default();
        assert!(config.device_uid.is_null(), "null means default device");
        assert_eq!(config.fft_size, 4096);
        assert!(config.fft_size.is_multiple_of(2));
        assert_eq!(config.mode, AnalyzerMode::Spectrum, "a UI opens on the RTA");
        assert_eq!(
            config.signal,
            AnalyzerSignal::Silence,
            "nothing plays until asked"
        );
        assert!(
            config.signal_level_db <= -12.0,
            "the default stimulus level must be quiet enough not to damage anything"
        );
    }

    /// The coefficients handed to the audio thread must describe the same
    /// filter the equaliser does, or what is heard and what is drawn diverge.
    #[test]
    fn published_coefficients_match_the_equaliser() {
        let mut eq = Equaliser::graphic(48_000.0);
        eq.set_gain_db(5, 6.0);
        eq.set_preamp_db(-6.0);

        let coefficients = EqCoefficients::from_equaliser(1, &eq);
        assert_eq!(coefficients.count, 10);
        assert!(
            (coefficients.trim - 0.501_187).abs() < 1e-4,
            "-6 dB is 0.5012"
        );

        let section = coefficients.sections[5];
        let expected = eq.bands()[5].design(48_000.0);
        assert_eq!(section.b0, expected.b0);
        assert_eq!(section.a1, expected.a1);
    }

    /// An equaliser with more bands than the array holds must not overflow it.
    #[test]
    fn publishing_stops_at_the_array_bound() {
        let bands = (0..ANALYZER_MAX_EQ_BANDS + 8)
            .map(|i| FilterBand::peaking(100.0 + i as f32 * 100.0, 3.0, 2.0))
            .collect();
        let eq = Equaliser::new(48_000.0, bands);
        let coefficients = EqCoefficients::from_equaliser(1, &eq);
        assert_eq!(coefficients.count, ANALYZER_MAX_EQ_BANDS);
    }

    /// The processor must apply what was published, and must pick up a change.
    #[test]
    fn the_processor_follows_the_published_coefficients() {
        let (mut publisher, reader) = snapshot_channel(EqCoefficients::default());
        let mut processor = EqProcessor::new(reader);

        // Nothing published yet: a pass-through.
        let mut samples = vec![1.0_f32, 0.0, 0.0, 0.0];
        processor.process(&mut samples);
        assert_eq!(samples[0], 1.0);

        // A pure trim is the easiest thing to check exactly.
        let mut eq = Equaliser::parametric(48_000.0);
        eq.set_preamp_db(-6.0);
        let coefficients = EqCoefficients::from_equaliser(1, &eq);
        publisher.publish_with(|slot| *slot = coefficients);

        let mut samples = vec![1.0_f32, 0.0, 0.0, 0.0];
        processor.process(&mut samples);
        assert!(
            (samples[0] - 0.501_187).abs() < 1e-4,
            "the trim was not applied, got {}",
            samples[0]
        );
    }

    /// A level above full scale cannot be produced and would only clip.
    #[test]
    fn the_stimulus_level_is_capped_at_full_scale() {
        let state = SignalState::new(AnalyzerSignal::Sine, 40.0, 1000.0);
        match state.signal() {
            Signal::Sine { amplitude, .. } => assert!(
                (amplitude - 1.0).abs() < 1e-6,
                "expected clamping to unity, got {amplitude}"
            ),
            other => panic!("wrong signal: {other:?}"),
        }
    }

    /// Decibels must reach the generator as a linear amplitude.
    #[test]
    fn the_stimulus_level_converts_from_decibels() {
        let state = SignalState::new(AnalyzerSignal::PinkNoise, -20.0, 0.0);
        match state.signal() {
            Signal::PinkNoise { amplitude } => assert!(
                (amplitude - 0.1).abs() < 1e-6,
                "-20 dB is 0.1, got {amplitude}"
            ),
            other => panic!("wrong signal: {other:?}"),
        }
    }

    /// Every change must be visible to the audio thread, which only reloads
    /// when the counter moves.
    #[test]
    fn changing_the_stimulus_bumps_the_generation() {
        let state = SignalState::new(AnalyzerSignal::Silence, -20.0, 1000.0);
        let before = state.generation.load(Ordering::Acquire);
        state.set(AnalyzerSignal::Sine, -6.0, 440.0);
        assert!(state.generation.load(Ordering::Acquire) > before);
        assert!(matches!(state.signal(), Signal::Sine { hz, .. } if (hz - 440.0).abs() < 1e-6));
    }

    #[test]
    fn device_enumeration_works_over_the_boundary() {
        let list = analyzer_device_list_create();
        assert!(!list.is_null());
        unsafe {
            let count = analyzer_device_list_count(list);
            assert!(count > 0, "expected at least one device");

            let mut device = AnalyzerDevice {
                uid: ptr::null(),
                name: ptr::null(),
                input_channels: 0,
                output_channels: 0,
                sample_rate: 0.0,
                is_default_input: false,
            };
            assert!(analyzer_device_list_get(list, 0, &mut device));
            assert!(!device.uid.is_null() && !device.name.is_null());
            let uid = std::ffi::CStr::from_ptr(device.uid).to_str().unwrap();
            assert!(!uid.is_empty());

            // Out of range must fail rather than read past the end.
            assert!(!analyzer_device_list_get(list, count + 10, &mut device));

            analyzer_device_list_destroy(list);
        }
    }

    /// Discriminants are part of the ABI. Changing one silently breaks an
    /// already-compiled UI, so they are pinned rather than left implicit.
    #[test]
    fn enum_discriminants_are_stable() {
        assert_eq!(AnalyzerWindow::Rectangular as u32, 0);
        assert_eq!(AnalyzerWindow::Hann as u32, 1);
        assert_eq!(AnalyzerWindow::BlackmanHarris as u32, 2);
        assert_eq!(AnalyzerWindow::FlatTop as u32, 3);
        assert_eq!(AnalyzerWindow::Tukey as u32, 4);
        assert_eq!(AnalyzerOverlap::None as u32, 0);
        assert_eq!(AnalyzerOverlap::ThreeQuarters as u32, 2);
        assert_eq!(AnalyzerAveraging::PeakHold as u32, 3);
        assert_eq!(AnalyzerReduction::Mean as u32, 1);
    }

    #[test]
    fn enum_mappings_reach_the_core_types() {
        assert_eq!(
            WindowKind::from(AnalyzerWindow::FlatTop),
            WindowKind::FlatTop
        );
        assert_eq!(Overlap::from(AnalyzerOverlap::Half), Overlap::Half);
        assert_eq!(Reduction::from(AnalyzerReduction::Mean), Reduction::Mean);
    }

    /// The structs cross a C boundary, so their layout must not drift silently.
    #[test]
    fn repr_c_types_have_the_expected_shape() {
        assert_eq!(
            size_of::<AnalyzerTick>(),
            12,
            "float, float, bool + padding"
        );
        assert_eq!(align_of::<AnalyzerStatus>(), 4);
        assert!(size_of::<AnalyzerFrameInfo>() >= 24);
    }
}
