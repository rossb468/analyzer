//! iOS backend: `AVAudioSession` for routing, the RemoteIO unit for audio.
//!
//! iOS has no HAL to enumerate. The app gets one audio session, the system
//! routes it, and the app can only state preferences: which input port, what
//! sample rate, what buffer duration. So a [`DeviceInfo`] here is an *input
//! port* - the built-in microphone, a USB measurement microphone, a wired
//! headset - and its id is the port's UID. Output always goes wherever the
//! session routes it alongside that input.
//!
//! RemoteIO is the lowest level iOS offers, and full duplex on one clock: input
//! and output share a unit, so the aggregate-device problem macOS has does not
//! exist here. The render callback on the output element pulls the input with
//! `AudioUnitRender`, hands both sides to the [`AudioCallback`], and returns.
//!
//! # Measurement mode
//!
//! The session is put in `AVAudioSessionModeMeasurement`. Without it iOS applies
//! automatic gain control and voice processing to the microphone, and every
//! measurement taken through it is a measurement of Apple's signal chain.
//! Bluetooth is deliberately not allowed: hands-free input runs at 16 kHz, and
//! A2DP output adds a large latency that varies from one connection to the next.
//!
//! # Microphone permission
//!
//! iOS gates capture behind a user prompt that only the app can raise, and the
//! app needs `NSMicrophoneUsageDescription` in its `Info.plist`. Asking is the
//! client's job, before it starts a session. If permission is refused the unit
//! still runs, `AudioUnitRender` fails, and the callback receives silence - the
//! same symptom as macOS, so the caller must check for it in the same way.
//!
//! # Interruptions
//!
//! A phone call or another app taking the session stops the unit without
//! telling it. The client observes `AVAudioSession` interruption and route-change
//! notifications, which are application lifecycle rather than analysis, and
//! restarts the session afterwards.

#![allow(non_upper_case_globals)]

use std::ffi::c_void;
use std::fmt;
use std::ptr::{self, NonNull};

use objc2::rc::Retained;
use objc2_audio_toolbox::{
    AURenderCallbackStruct, AudioComponentDescription, AudioComponentFindNext,
    AudioComponentInstanceDispose, AudioComponentInstanceNew, AudioOutputUnitStart,
    AudioOutputUnitStop, AudioUnit, AudioUnitGetProperty, AudioUnitInitialize, AudioUnitRender,
    AudioUnitRenderActionFlags, AudioUnitSetProperty, AudioUnitUninitialize,
    kAudioOutputUnitProperty_EnableIO, kAudioUnitManufacturer_Apple,
    kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitProperty_SetRenderCallback,
    kAudioUnitProperty_StreamFormat, kAudioUnitScope_Global, kAudioUnitScope_Input,
    kAudioUnitScope_Output, kAudioUnitType_Output,
};
use objc2_avf_audio::{
    AVAudioSession, AVAudioSessionCategoryOptions, AVAudioSessionCategoryPlayAndRecord,
    AVAudioSessionModeMeasurement, AVAudioSessionPortDescription,
};
use objc2_core_audio_types::{
    AudioBuffer, AudioBufferList, AudioStreamBasicDescription, AudioTimeStamp,
    kAudioFormatFlagIsFloat, kAudioFormatFlagIsNonInterleaved, kAudioFormatFlagIsPacked,
    kAudioFormatLinearPCM,
};

use crate::backend::AudioBackend;
use crate::device::{DeviceId, DeviceInfo};
use crate::error::AudioError;
use crate::stream::{AudioBuffers, AudioCallback, AudioStream, StreamConfig, StreamLatency};

/// `'rioc'`. The bindings do not export the RemoteIO subtype.
const kAudioUnitSubType_RemoteIO: u32 = u32::from_be_bytes(*b"rioc");

/// RemoteIO's input element. Microphone audio comes out of its output scope.
const INPUT_ELEMENT: u32 = 1;

/// RemoteIO's output element. The speaker is fed through its input scope.
const OUTPUT_ELEMENT: u32 = 0;

/// Largest callback preallocated for.
///
/// iOS raises the slice size to 4096 frames when the screen locks, whatever
/// buffer duration was asked for. This leaves headroom above that, and the unit
/// is told it may not exceed it.
const MAX_BUFFER_FRAMES: usize = 8192;

/// Most hardware channels in one direction.
///
/// The input buffer list is a fixed-size struct so that the render callback has
/// one to hand without allocating. Thirty-two covers any USB interface an
/// iPhone can power.
const MAX_CHANNELS: usize = 32;

/// `AudioBufferList` with room for [`MAX_CHANNELS`] buffers.
///
/// Laid out exactly as the C struct with a longer trailing array, which is how
/// CoreAudio expects a multi-buffer list to be allocated.
#[repr(C)]
struct InputBufferList {
    number_buffers: u32,
    buffers: [AudioBuffer; MAX_CHANNELS],
}

fn os_error(status: i32, operation: &str) -> AudioError {
    AudioError::Backend(format!("{operation} failed with OSStatus {status}"))
}

fn ns_error(error: &objc2_foundation::NSError, operation: &str) -> AudioError {
    AudioError::Backend(format!(
        "{operation} failed: {}",
        error.localizedDescription()
    ))
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

fn session() -> Retained<AVAudioSession> {
    // SAFETY: the shared session is a process-wide singleton and always exists.
    unsafe { AVAudioSession::sharedInstance() }
}

/// Put the session in play-and-record measurement mode and activate it.
///
/// Done before enumeration as well as before opening: `availableInputs` is only
/// populated for a session whose category records, and the sample rate it
/// reports is only meaningful once the session is active.
fn configure(session: &AVAudioSession) -> Result<(), AudioError> {
    // SAFETY: both statics are always present on iOS; `None` would mean a
    // missing framework, which the link would already have failed on.
    let (Some(category), Some(mode)) = (unsafe { AVAudioSessionCategoryPlayAndRecord }, unsafe {
        AVAudioSessionModeMeasurement
    }) else {
        return Err(AudioError::NoBackend);
    };
    // DefaultToSpeaker: play-and-record otherwise routes output to the earpiece
    // receiver, which is useless as a measurement source.
    // SAFETY: plain Objective-C calls on the shared session.
    unsafe {
        session
            .setCategory_mode_options_error(
                category,
                mode,
                AVAudioSessionCategoryOptions::DefaultToSpeaker,
            )
            .map_err(|e| ns_error(&e, "setting the audio session category"))?;
        session
            .setActive_error(true)
            .map_err(|e| ns_error(&e, "activating the audio session"))?;
    }
    Ok(())
}

fn port_channels(port: &AVAudioSessionPortDescription) -> u32 {
    // SAFETY: reading a property of a live port description.
    unsafe { port.channels() }
        .map(|c| c.count() as u32)
        .unwrap_or(1)
        .max(1)
}

fn current_input_uid(session: &AVAudioSession) -> Option<String> {
    // SAFETY: reading properties of the live shared session.
    let inputs = unsafe { session.currentRoute().inputs() };
    inputs
        .firstObject()
        .map(|port| unsafe { port.UID() }.to_string())
}

fn find_port(
    session: &AVAudioSession,
    uid: &str,
) -> Option<Retained<AVAudioSessionPortDescription>> {
    // SAFETY: reading properties of the live shared session.
    let ports = unsafe { session.availableInputs() }?;
    ports
        .iter()
        .find(|port| unsafe { port.UID() }.to_string() == uid)
}

fn to_u32(value: isize) -> u32 {
    u32::try_from(value.max(0)).unwrap_or(0)
}

// ---------------------------------------------------------------------------
// Backend
// ---------------------------------------------------------------------------

/// The iOS backend.
#[derive(Debug, Default, Clone, Copy)]
pub struct IosBackend;

impl IosBackend {
    /// Create the backend. The session is configured lazily, per call.
    pub fn new() -> Self {
        Self
    }

    /// Open a concrete [`IosStream`].
    ///
    /// # Errors
    ///
    /// [`AudioError::DeviceNotFound`] if the input port is gone,
    /// [`AudioError::ChannelOutOfRange`] for a channel the route lacks,
    /// [`AudioError::UnsupportedSampleRate`] if the session will not run at the
    /// requested rate, or [`AudioError::Backend`] wrapping an `OSStatus` or an
    /// `NSError`.
    pub fn open_stream(
        &mut self,
        config: &StreamConfig,
        callback: Box<dyn AudioCallback>,
    ) -> Result<IosStream, AudioError> {
        config.validate()?;

        if let (Some(input), Some(output)) = (&config.input, &config.output)
            && input != output
        {
            return Err(AudioError::Backend(format!(
                "input '{input}' and output '{output}' are different devices; on iOS one \
                 session carries both directions, so they must name the same port"
            )));
        }

        let session = session();
        configure(&session)?;

        let uid = config.input.as_ref().or(config.output.as_ref());
        let mut name = String::from("iOS audio session");
        if let Some(uid) = uid {
            let port = find_port(&session, uid.as_str())
                .ok_or_else(|| AudioError::DeviceNotFound(uid.to_string()))?;
            // SAFETY: plain Objective-C calls on the shared session and a live
            // port description.
            unsafe {
                name = port.portName().to_string();
                session
                    .setPreferredInput_error(Some(&port))
                    .map_err(|e| ns_error(&e, "selecting the input"))?;
                // Ask for every channel the port has. The session defaults to
                // one even on a multichannel interface. Refusal is not fatal:
                // the counts actually granted are checked below.
                let _ = session.setPreferredInputNumberOfChannels_error(
                    session.maximumInputNumberOfChannels(),
                );
            }
        }

        // SAFETY: plain Objective-C calls on the shared session.
        unsafe {
            session
                .setPreferredSampleRate_error(config.sample_rate)
                .map_err(|e| ns_error(&e, "requesting the sample rate"))?;
            let wanted = f64::from(config.buffer_frames.clamp(16, MAX_BUFFER_FRAMES as u32));
            // A preference, and only a preference: iOS rounds it, and the
            // callback copes with whatever size actually arrives.
            let _ = session.setPreferredIOBufferDuration_error(wanted / config.sample_rate);
        }

        // SAFETY: reading properties of the live shared session.
        let (rate, inputs_available, outputs_available, io_duration, latency) = unsafe {
            let rate = session.sampleRate();
            (
                rate,
                to_u32(session.inputNumberOfChannels()),
                to_u32(session.outputNumberOfChannels()),
                session.IOBufferDuration(),
                StreamLatency {
                    input_frames: (session.inputLatency() * rate).round() as u32,
                    output_frames: (session.outputLatency() * rate).round() as u32,
                    safety_offset_frames: 0,
                },
            )
        };

        // Switching the input port can switch the hardware rate. Carrying on at
        // the wrong one would mislabel every frequency the engine reports.
        if (rate - config.sample_rate).abs() > 0.5 {
            return Err(AudioError::UnsupportedSampleRate {
                device: name,
                requested: config.sample_rate,
            });
        }

        for (channels, available) in [
            (&config.input_channels, inputs_available),
            (&config.output_channels, outputs_available),
        ] {
            for &channel in channels {
                if channel >= available || channel as usize >= MAX_CHANNELS {
                    return Err(AudioError::ChannelOutOfRange {
                        device: name.clone(),
                        channel,
                        available,
                    });
                }
            }
        }

        let capture = !config.input_channels.is_empty();
        let input_count = config.input_channels.len();
        let output_count = config.output_channels.len();

        // Every buffer the render callback touches is allocated here, once.
        let state = Box::new(RenderState {
            unit: ptr::null_mut(),
            callback,
            capture,
            input_list: InputBufferList {
                number_buffers: 0,
                buffers: [AudioBuffer {
                    mNumberChannels: 1,
                    mDataByteSize: 0,
                    mData: ptr::null_mut(),
                }; MAX_CHANNELS],
            },
            input_planes: vec![0.0; MAX_BUFFER_FRAMES * inputs_available.max(1) as usize],
            input_hardware_channels: inputs_available as usize,
            interleaved: vec![0.0; MAX_BUFFER_FRAMES * input_count.max(1)],
            output_scratch: vec![0.0; MAX_BUFFER_FRAMES * output_count.max(1)],
            selected: config.input_channels.clone(),
            outputs: config.output_channels.clone(),
        });

        let unit = new_remote_io()?;
        // From here on the unit must be disposed on every early return, which
        // the stream's Drop does once it owns it.
        let mut stream = IosStream {
            unit,
            initialized: false,
            state,
            config: config.clone(),
            latency,
            running: false,
            name,
        };
        stream.state.unit = unit;

        let format = |channels: u32| AudioStreamBasicDescription {
            mSampleRate: rate,
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagIsFloat
                | kAudioFormatFlagIsPacked
                | kAudioFormatFlagIsNonInterleaved,
            mBytesPerPacket: size_of::<f32>() as u32,
            mFramesPerPacket: 1,
            mBytesPerFrame: size_of::<f32>() as u32,
            mChannelsPerFrame: channels.max(1),
            mBitsPerChannel: 32,
            mReserved: 0,
        };

        set_property(
            unit,
            kAudioOutputUnitProperty_EnableIO,
            kAudioUnitScope_Input,
            INPUT_ELEMENT,
            &u32::from(capture),
            "enabling input",
        )?;
        if capture {
            set_property(
                unit,
                kAudioUnitProperty_StreamFormat,
                kAudioUnitScope_Output,
                INPUT_ELEMENT,
                &format(inputs_available),
                "setting the input format",
            )?;
        }
        set_property(
            unit,
            kAudioUnitProperty_StreamFormat,
            kAudioUnitScope_Input,
            OUTPUT_ELEMENT,
            &format(outputs_available),
            "setting the output format",
        )?;
        set_property(
            unit,
            kAudioUnitProperty_MaximumFramesPerSlice,
            kAudioUnitScope_Global,
            0,
            &(MAX_BUFFER_FRAMES as u32),
            "setting the maximum slice",
        )?;
        let render = AURenderCallbackStruct {
            inputProc: Some(render_callback),
            inputProcRefCon: (&raw mut *stream.state).cast::<c_void>(),
        };
        set_property(
            unit,
            kAudioUnitProperty_SetRenderCallback,
            kAudioUnitScope_Input,
            OUTPUT_ELEMENT,
            &render,
            "installing the render callback",
        )?;

        // SAFETY: `unit` is a live, fully configured RemoteIO instance.
        let status = unsafe { AudioUnitInitialize(unit) };
        if status != 0 {
            return Err(os_error(status, "AudioUnitInitialize"));
        }
        stream.initialized = true;

        let granted_frames = (io_duration * rate).round() as u32;
        stream.config.sample_rate = rate;
        stream.config.buffer_frames = granted_frames.clamp(1, MAX_BUFFER_FRAMES as u32);
        Ok(stream)
    }
}

fn new_remote_io() -> Result<AudioUnit, AudioError> {
    let description = AudioComponentDescription {
        componentType: kAudioUnitType_Output,
        componentSubType: kAudioUnitSubType_RemoteIO,
        componentManufacturer: kAudioUnitManufacturer_Apple,
        componentFlags: 0,
        componentFlagsMask: 0,
    };
    // SAFETY: the description is a valid stack value for the duration of the call.
    let component = unsafe { AudioComponentFindNext(ptr::null_mut(), NonNull::from(&description)) };
    if component.is_null() {
        return Err(AudioError::Backend("RemoteIO audio unit not found".into()));
    }
    let mut unit: AudioUnit = ptr::null_mut();
    // SAFETY: `component` is a live component; CoreAudio writes one instance.
    let status = unsafe { AudioComponentInstanceNew(component, NonNull::from(&mut unit)) };
    if status != 0 || unit.is_null() {
        return Err(os_error(status, "AudioComponentInstanceNew"));
    }
    Ok(unit)
}

fn set_property<T>(
    unit: AudioUnit,
    property: u32,
    scope: u32,
    element: u32,
    value: &T,
    operation: &str,
) -> Result<(), AudioError> {
    // SAFETY: `value` is a live `T`, and its size is what is passed.
    let status = unsafe {
        AudioUnitSetProperty(
            unit,
            property,
            scope,
            element,
            ptr::from_ref(value).cast::<c_void>(),
            size_of::<T>() as u32,
        )
    };
    if status != 0 {
        return Err(os_error(status, operation));
    }
    Ok(())
}

/// Read back a fixed-size property, for tests and diagnostics.
#[allow(dead_code)]
fn get_property<T: Copy + Default>(
    unit: AudioUnit,
    property: u32,
    scope: u32,
    element: u32,
) -> Option<T> {
    let mut value = T::default();
    let mut size = size_of::<T>() as u32;
    // SAFETY: `value` is a `T` and `size` says so, so CoreAudio cannot overrun it.
    let status = unsafe {
        AudioUnitGetProperty(
            unit,
            property,
            scope,
            element,
            NonNull::from(&mut value).cast::<c_void>(),
            NonNull::from(&mut size),
        )
    };
    (status == 0).then_some(value)
}

impl AudioBackend for IosBackend {
    fn name(&self) -> &str {
        "ios"
    }

    /// The input ports the session can route, each with the output it would
    /// play through.
    ///
    /// Configures and activates the session as a side effect: iOS lists inputs
    /// only for a session that records, and reports a meaningful sample rate
    /// only for one that is active.
    fn devices(&self) -> Result<Vec<DeviceInfo>, AudioError> {
        let session = session();
        configure(&session)?;
        let current = current_input_uid(&session);

        // SAFETY: reading properties of the live shared session.
        let (rate, outputs) = unsafe {
            (
                session.sampleRate(),
                to_u32(session.outputNumberOfChannels()),
            )
        };
        // SAFETY: as above.
        let Some(ports) = (unsafe { session.availableInputs() }) else {
            return Ok(Vec::new());
        };

        Ok(ports
            .iter()
            .map(|port| {
                // SAFETY: reading properties of a live port description.
                let (uid, name) = unsafe { (port.UID().to_string(), port.portName().to_string()) };
                let routed = current.as_deref() == Some(uid.as_str());
                DeviceInfo {
                    id: DeviceId::new(uid),
                    name,
                    input_channels: port_channels(&port),
                    output_channels: outputs,
                    // Only the routed port's rate is known without switching
                    // to it. Opening another verifies the rate it really gets.
                    default_sample_rate: rate,
                    supported_sample_rates: Vec::new(),
                    is_default_input: routed,
                    is_default_output: routed,
                }
            })
            .collect())
    }

    fn open(
        &mut self,
        config: &StreamConfig,
        callback: Box<dyn AudioCallback>,
    ) -> Result<Box<dyn AudioStream>, AudioError> {
        Ok(Box::new(self.open_stream(config, callback)?))
    }
}

// ---------------------------------------------------------------------------
// Stream
// ---------------------------------------------------------------------------

/// State the render callback reaches through its client pointer.
///
/// Boxed and never moved while the unit lives. Every buffer here is sized at
/// open so the callback allocates nothing.
struct RenderState {
    unit: AudioUnit,
    callback: Box<dyn AudioCallback>,
    capture: bool,
    /// Points into `input_planes`; refilled each callback, because
    /// `AudioUnitRender` may rewrite the sizes and pointers it is given.
    input_list: InputBufferList,
    /// One plane of [`MAX_BUFFER_FRAMES`] per hardware input channel.
    input_planes: Vec<f32>,
    input_hardware_channels: usize,
    /// Selected input channels, interleaved.
    interleaved: Vec<f32>,
    /// Interleaved output the callback writes, scattered afterwards.
    output_scratch: Vec<f32>,
    selected: Vec<u32>,
    outputs: Vec<u32>,
}

/// A RemoteIO stream.
pub struct IosStream {
    unit: AudioUnit,
    initialized: bool,
    /// CoreAudio holds a raw pointer into this allocation; the field keeps it
    /// alive until Drop has disposed of the unit.
    state: Box<RenderState>,
    config: StreamConfig,
    latency: StreamLatency,
    running: bool,
    name: String,
}

// SAFETY: the only shared state is `state`, reached solely by the render
// callback while the unit runs. `start`/`stop` bracket that access, and Drop
// disposes of the unit before the box is freed.
unsafe impl Send for IosStream {}

impl AudioStream for IosStream {
    fn start(&mut self) -> Result<(), AudioError> {
        if self.running {
            return Err(AudioError::AlreadyRunning);
        }
        // Reactivate in case an interruption deactivated the session since open.
        configure(&session())?;
        // SAFETY: `unit` is a live, initialised RemoteIO instance.
        let status = unsafe { AudioOutputUnitStart(self.unit) };
        if status != 0 {
            return Err(os_error(status, "AudioOutputUnitStart"));
        }
        self.running = true;
        Ok(())
    }

    fn stop(&mut self) -> Result<(), AudioError> {
        if !self.running {
            return Ok(());
        }
        // SAFETY: as above; stopping a stopped unit is harmless.
        let status = unsafe { AudioOutputUnitStop(self.unit) };
        self.running = false;
        if status != 0 {
            return Err(os_error(status, "AudioOutputUnitStop"));
        }
        Ok(())
    }

    fn is_running(&self) -> bool {
        self.running
    }

    fn config(&self) -> &StreamConfig {
        &self.config
    }

    fn latency(&self) -> StreamLatency {
        self.latency
    }
}

impl Drop for IosStream {
    fn drop(&mut self) {
        let _ = self.stop();
        // Must happen before `state` is freed: the unit holds a pointer to it.
        // SAFETY: the unit was created by this stream and is disposed exactly once.
        unsafe {
            if self.initialized {
                AudioUnitUninitialize(self.unit);
            }
            AudioComponentInstanceDispose(self.unit);
        }
    }
}

impl fmt::Debug for IosStream {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("IosStream")
            .field("device", &self.name)
            .field("running", &self.running)
            .field("buffer_frames", &self.config.buffer_frames)
            .field("sample_rate", &self.config.sample_rate)
            .finish_non_exhaustive()
    }
}

/// The real-time render callback RemoteIO invokes for its output element.
///
/// Pulls the input, gathers the selected channels, calls through, scatters the
/// output. It must not allocate, lock or panic: a panic here unwinds into
/// CoreAudio.
unsafe extern "C-unwind" fn render_callback(
    client: NonNull<c_void>,
    flags: NonNull<AudioUnitRenderActionFlags>,
    timestamp: NonNull<AudioTimeStamp>,
    _bus: u32,
    frame_count: u32,
    output: *mut AudioBufferList,
) -> i32 {
    // SAFETY: `client` is the pointer installed with the render callback, which
    // points at a live boxed RenderState for as long as the unit can run.
    let state = unsafe { &mut *client.cast::<RenderState>().as_ptr() };
    let RenderState {
        unit,
        callback,
        capture,
        input_list,
        input_planes,
        input_hardware_channels,
        interleaved,
        output_scratch,
        selected,
        outputs,
    } = state;

    let frames = frame_count as usize;
    if frames == 0 || frames > MAX_BUFFER_FRAMES {
        // Larger than preallocated: play silence rather than grow a buffer here.
        // SAFETY: the output list is the unit's own, valid for this call.
        unsafe { silence(output) };
        return 0;
    }

    let channels = selected.len();
    let gathered_len = frames * channels;
    let gathered = interleaved.get_mut(..gathered_len).unwrap_or(&mut []);
    gathered.fill(0.0);

    if *capture {
        let hardware = (*input_hardware_channels).min(MAX_CHANNELS);
        input_list.number_buffers = hardware as u32;
        let base = input_planes.as_mut_ptr();
        for (index, buffer) in input_list.buffers.iter_mut().take(hardware).enumerate() {
            buffer.mNumberChannels = 1;
            buffer.mDataByteSize = (frames * size_of::<f32>()) as u32;
            // SAFETY: plane `index` lies inside `input_planes`, which holds
            // MAX_BUFFER_FRAMES per hardware channel.
            buffer.mData = unsafe { base.add(index * MAX_BUFFER_FRAMES) }.cast::<c_void>();
        }
        // SAFETY: the list is laid out as an AudioBufferList with `hardware`
        // buffers, each large enough for `frames`.
        let status = unsafe {
            AudioUnitRender(
                *unit,
                flags.as_ptr(),
                timestamp,
                INPUT_ELEMENT,
                frame_count,
                NonNull::from(&mut *input_list).cast::<AudioBufferList>(),
            )
        };
        // A failed render - permission refused, or the route changing under
        // us - leaves the gathered input silent rather than stale.
        if status == 0 {
            for (slot, &wanted) in selected.iter().enumerate() {
                let Some(plane) = input_planes
                    .get(wanted as usize * MAX_BUFFER_FRAMES..)
                    .and_then(|p| p.get(..frames))
                else {
                    continue;
                };
                for (frame, sample) in plane.iter().enumerate() {
                    if let Some(out) = gathered.get_mut(frame * channels + slot) {
                        *out = *sample;
                    }
                }
            }
        }
    }

    let output_channels = outputs.len();
    let writable = output_scratch
        .get_mut(..frames * output_channels)
        .unwrap_or(&mut []);
    // Never hand the callback stale contents.
    writable.fill(0.0);

    let mut buffers = AudioBuffers::new(gathered, writable, channels, output_channels, frames);
    callback.process(&mut buffers);

    // SAFETY: the output list is the unit's own, valid for this call.
    unsafe {
        silence(output);
        scatter(output, outputs, output_scratch, frames);
    }
    0
}

/// Zero every buffer in a list.
///
/// # Safety
///
/// `list` must be null or a valid `AudioBufferList`.
unsafe fn silence(list: *mut AudioBufferList) {
    if list.is_null() {
        return;
    }
    // SAFETY: caller guarantees a valid list.
    let (count, buffers) = unsafe {
        (
            (*list).mNumberBuffers as usize,
            (*list).mBuffers.as_mut_ptr(),
        )
    };
    for index in 0..count {
        // SAFETY: index < count.
        let buffer = unsafe { &mut *buffers.add(index) };
        if !buffer.mData.is_null() {
            // SAFETY: the buffer reports its own size in bytes.
            unsafe {
                ptr::write_bytes(buffer.mData.cast::<u8>(), 0, buffer.mDataByteSize as usize)
            };
        }
    }
}

/// Write interleaved scratch into the unit's non-interleaved output planes.
///
/// # Safety
///
/// `list` must be null or a valid non-interleaved `Float32` `AudioBufferList`.
unsafe fn scatter(
    list: *mut AudioBufferList,
    selected: &[u32],
    interleaved: &[f32],
    frames: usize,
) {
    if list.is_null() || selected.is_empty() {
        return;
    }
    // SAFETY: caller guarantees a valid list.
    let (count, buffers) = unsafe {
        (
            (*list).mNumberBuffers as usize,
            (*list).mBuffers.as_mut_ptr(),
        )
    };
    let stride = selected.len();
    for (slot, &wanted) in selected.iter().enumerate() {
        let wanted = wanted as usize;
        if wanted >= count {
            continue;
        }
        // SAFETY: wanted < count.
        let buffer = unsafe { &mut *buffers.add(wanted) };
        if buffer.mData.is_null() {
            continue;
        }
        let capacity = buffer.mDataByteSize as usize / size_of::<f32>();
        let data = buffer.mData.cast::<f32>();
        for frame in 0..frames.min(capacity) {
            if let Some(sample) = interleaved.get(frame * stride + slot) {
                // SAFETY: frame < capacity, the plane's length in samples.
                unsafe { *data.add(frame) = *sample };
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use std::mem::offset_of;

    use super::*;

    /// The render callback hands `InputBufferList` to CoreAudio as an
    /// `AudioBufferList`. That is only sound while the header and the first
    /// buffer sit where the C struct puts them.
    #[test]
    fn the_input_list_is_laid_out_as_an_audio_buffer_list() {
        assert_eq!(
            offset_of!(InputBufferList, number_buffers),
            offset_of!(AudioBufferList, mNumberBuffers)
        );
        assert_eq!(
            offset_of!(InputBufferList, buffers),
            offset_of!(AudioBufferList, mBuffers)
        );
        assert_eq!(align_of::<InputBufferList>(), align_of::<AudioBufferList>());
    }

    #[test]
    fn the_remote_io_subtype_is_rioc() {
        assert_eq!(kAudioUnitSubType_RemoteIO, 0x7269_6f63);
    }
}
