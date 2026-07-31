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

use analyzer_audio::{
    AudioBackend, AudioBuffers, AudioStream, CoreAudioBackend, DeviceId, DeviceInfo, StreamConfig,
};
use analyzer_dsp::{Averaging, DistortionConfig, Overlap, SpectrumConfig, WindowKind};
use analyzer_engine::{Engine, EngineConfig, rt_section};
use analyzer_model::{Measurement, MeasurementData, MeasurementId, References};
use analyzer_plot::{FrequencyAxis, LevelAxis, Reduction, Trace, reduce};

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
        }
    }
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

    let channels = device.input_channels as usize;
    let rate = device.default_sample_rate;
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
        ring_capacity_frames: 16_384,
    });

    let stream_config = StreamConfig {
        input: Some(DeviceId::new(device.id.as_str())),
        output: None,
        sample_rate: rate,
        buffer_frames: config.buffer_frames,
        input_channels: (0..channels as u32).collect(),
        output_channels: Vec::new(),
    };

    let mut stream = backend
        .open(
            &stream_config,
            Box::new(move |buffers: &mut AudioBuffers<'_>| {
                rt_section(|| {
                    sink.write_interleaved(buffers.input());
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

/// Which of the two traces a copy refers to.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Which {
    Live,
    Average,
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
