//! CoreAudio backend for macOS.
//!
//! Talks to the HAL directly — `AudioObjectGetPropertyData` for enumeration and
//! `AudioDeviceCreateIOProcID` for capture — rather than going through an
//! AudioUnit graph or a portability wrapper. For a measurement tool that is the
//! right level: it addresses exact devices, reports the hardware's own latency
//! figures, and adds no resampling or mixing between the converter and us.
//!
//! # Device identity
//!
//! [`DeviceId`] carries the device's **UID**, not its `AudioDeviceID`. The
//! numeric id is only stable within a boot, so persisting it in a session would
//! silently reopen the wrong device tomorrow. The UID survives reboots and
//! reconnection, and is resolved to a numeric id at open time.
//!
//! # Microphone permission
//!
//! macOS gates capture behind TCC. A bundled app needs `NSMicrophoneUsageDescription`
//! in its `Info.plist`; a bare command-line binary inherits the permission of the
//! terminal that launched it, and the first attempt prompts the user. If
//! permission is refused the stream starts but delivers silence, which is why
//! [`crate::coreaudio::CoreAudioStream`] cannot detect it and the caller must.

#![allow(non_upper_case_globals)]

use std::ffi::c_void;
use std::fmt;
use std::ptr;

use core_foundation_sys::base::CFRelease;
use core_foundation_sys::string::{CFStringGetCString, CFStringRef, kCFStringEncodingUTF8};
use coreaudio_sys::{
    AudioBufferList, AudioDeviceCreateIOProcID, AudioDeviceDestroyIOProcID, AudioDeviceID,
    AudioDeviceIOProcID, AudioDeviceStart, AudioDeviceStop, AudioObjectGetPropertyData,
    AudioObjectGetPropertyDataSize, AudioObjectID, AudioObjectPropertyAddress,
    AudioObjectSetPropertyData, AudioTimeStamp, AudioValueRange, OSStatus,
    kAudioDevicePropertyAvailableNominalSampleRates, kAudioDevicePropertyBufferFrameSize,
    kAudioDevicePropertyDeviceUID, kAudioDevicePropertyLatency,
    kAudioDevicePropertyNominalSampleRate, kAudioDevicePropertySafetyOffset,
    kAudioDevicePropertyStreamConfiguration, kAudioHardwarePropertyDefaultInputDevice,
    kAudioHardwarePropertyDefaultOutputDevice, kAudioHardwarePropertyDevices,
    kAudioObjectPropertyElementMain, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyScopeInput, kAudioObjectPropertyScopeOutput, kAudioObjectSystemObject,
};

use crate::backend::AudioBackend;
use crate::device::{DeviceId, DeviceInfo};
use crate::error::AudioError;
use crate::stream::{AudioBuffers, AudioCallback, AudioStream, StreamConfig, StreamLatency};

/// Largest callback we will preallocate for.
///
/// The IOProc must not allocate, so its scratch buffers are sized once at open.
/// A device asking for more frames than this would overflow them, so the open
/// call clamps the request and re-reads what the device actually granted.
const MAX_BUFFER_FRAMES: usize = 8192;

/// The HAL's "not permitted" status, observed when microphone access is denied.
///
/// CoreAudio does not surface this as a named constant, and it does not arrive
/// promptly: the server retries `StartAndWaitForState` on a 30 second timeout,
/// so a denied stream stalls for minutes before failing. Recognising the code
/// lets the caller say what is actually wrong instead of appearing to hang.
const HAL_NOT_PERMITTED: OSStatus = 0x1000_4003_u32 as OSStatus;

/// Turn an `OSStatus` from start/stop into something a user can act on.
fn describe_status(status: OSStatus, operation: &str) -> AudioError {
    if status == HAL_NOT_PERMITTED {
        return AudioError::Backend(format!(
            "{operation} was refused by CoreAudio (status {status}). This is macOS \
             microphone permission being denied. Grant access under System Settings > \
             Privacy & Security > Microphone for the application running this code. \
             Note that TCC will not raise a prompt for a process launched in a \
             non-interactive background session - it refuses outright - so this must \
             be run from a foreground terminal or a bundled app at least once."
        ));
    }
    AudioError::Backend(format!("{operation} failed with OSStatus {status}"))
}

// ---------------------------------------------------------------------------
// Property helpers
// ---------------------------------------------------------------------------

fn address(selector: u32, scope: u32) -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress {
        mSelector: selector,
        mScope: scope,
        mElement: kAudioObjectPropertyElementMain,
    }
}

/// Read a variable-length property as raw bytes.
fn property_bytes(object: AudioObjectID, addr: &AudioObjectPropertyAddress) -> Option<Vec<u8>> {
    let mut size = 0_u32;
    // SAFETY: `addr` is a valid property address; CoreAudio only writes `size`.
    let status = unsafe { AudioObjectGetPropertyDataSize(object, addr, 0, ptr::null(), &mut size) };
    if status != 0 || size == 0 {
        return None;
    }
    let mut buf = vec![0_u8; size as usize];
    // SAFETY: `buf` is exactly `size` bytes, which is what CoreAudio asked for.
    let status = unsafe {
        AudioObjectGetPropertyData(
            object,
            addr,
            0,
            ptr::null(),
            &mut size,
            buf.as_mut_ptr().cast::<c_void>(),
        )
    };
    (status == 0).then_some(buf)
}

/// Read a fixed-size property.
fn scalar<T: Copy + Default>(object: AudioObjectID, selector: u32, scope: u32) -> Option<T> {
    let addr = address(selector, scope);
    let mut value = T::default();
    let mut size = size_of::<T>() as u32;
    // SAFETY: `value` is a `T` and `size` says so, so CoreAudio cannot overrun it.
    let status = unsafe {
        AudioObjectGetPropertyData(
            object,
            &addr,
            0,
            ptr::null(),
            &mut size,
            (&raw mut value).cast::<c_void>(),
        )
    };
    (status == 0).then_some(value)
}

/// Read a `CFString` property and copy it into an owned `String`.
fn cfstring_property(object: AudioObjectID, selector: u32) -> Option<String> {
    let addr = address(selector, kAudioObjectPropertyScopeGlobal);
    let mut cfstr: CFStringRef = ptr::null();
    let mut size = size_of::<CFStringRef>() as u32;
    // SAFETY: writes exactly one CFStringRef, which we own a reference to.
    let status = unsafe {
        AudioObjectGetPropertyData(
            object,
            &addr,
            0,
            ptr::null(),
            &mut size,
            (&raw mut cfstr).cast::<c_void>(),
        )
    };
    if status != 0 || cfstr.is_null() {
        return None;
    }
    let mut buf = [0_i8; 1024];
    // SAFETY: `cfstr` is a live CFString; `buf` is 1024 bytes and we say so.
    let ok = unsafe { CFStringGetCString(cfstr, buf.as_mut_ptr(), 1024, kCFStringEncodingUTF8) };
    // SAFETY: the Get rule does not apply here - AudioObjectGetPropertyData
    // returns a +1 reference for CFString properties, so we own and release it.
    unsafe { CFRelease(cfstr.cast()) };
    if ok == 0 {
        return None;
    }
    let bytes: Vec<u8> = buf
        .iter()
        .take_while(|b| **b != 0)
        .map(|b| *b as u8)
        .collect();
    Some(String::from_utf8_lossy(&bytes).into_owned())
}

/// Total channels in a scope, summed over the device's streams.
fn channel_count(device: AudioDeviceID, scope: u32) -> u32 {
    let addr = address(kAudioDevicePropertyStreamConfiguration, scope);
    let Some(bytes) = property_bytes(device, &addr) else {
        return 0;
    };
    if bytes.len() < size_of::<u32>() {
        return 0;
    }
    // SAFETY: CoreAudio returned a well-formed AudioBufferList of this length.
    unsafe {
        let list = bytes.as_ptr().cast::<AudioBufferList>();
        let count = (*list).mNumberBuffers as usize;
        let buffers = (*list).mBuffers.as_ptr();
        (0..count).map(|i| (*buffers.add(i)).mNumberChannels).sum()
    }
}

fn available_rates(device: AudioDeviceID) -> Vec<f64> {
    let addr = address(
        kAudioDevicePropertyAvailableNominalSampleRates,
        kAudioObjectPropertyScopeGlobal,
    );
    let Some(bytes) = property_bytes(device, &addr) else {
        return Vec::new();
    };
    let mut rates = Vec::new();
    for chunk in bytes.chunks_exact(size_of::<AudioValueRange>()) {
        // SAFETY: chunk is exactly one AudioValueRange, correctly aligned since
        // the allocation came from a Vec<u8> read of an array of them.
        let range = unsafe { ptr::read_unaligned(chunk.as_ptr().cast::<AudioValueRange>()) };
        rates.push(range.mMinimum);
        if (range.mMaximum - range.mMinimum).abs() > 0.5 {
            rates.push(range.mMaximum);
        }
    }
    rates.sort_by(f64::total_cmp);
    rates.dedup_by(|a, b| (*a - *b).abs() < 0.5);
    rates
}

fn all_device_ids() -> Vec<AudioDeviceID> {
    let addr = address(
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
    );
    let Some(bytes) = property_bytes(kAudioObjectSystemObject, &addr) else {
        return Vec::new();
    };
    bytes
        .chunks_exact(size_of::<AudioDeviceID>())
        .filter_map(|c| c.try_into().ok())
        .map(AudioDeviceID::from_ne_bytes)
        .collect()
}

fn default_device(selector: u32) -> Option<AudioDeviceID> {
    scalar::<AudioDeviceID>(
        kAudioObjectSystemObject,
        selector,
        kAudioObjectPropertyScopeGlobal,
    )
    .filter(|id| *id != 0)
}

fn describe(
    device: AudioDeviceID,
    default_in: Option<u32>,
    default_out: Option<u32>,
) -> Option<DeviceInfo> {
    // The UID is the stable identity; a device without one is not addressable
    // across restarts and is not worth offering.
    let uid = cfstring_property(device, kAudioDevicePropertyDeviceUID)?;
    let name = cfstring_property(device, kAudioObjectPropertyName)
        .unwrap_or_else(|| format!("Audio device {device}"));

    Some(DeviceInfo {
        id: DeviceId::new(uid),
        name,
        input_channels: channel_count(device, kAudioObjectPropertyScopeInput),
        output_channels: channel_count(device, kAudioObjectPropertyScopeOutput),
        default_sample_rate: scalar::<f64>(
            device,
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal,
        )
        .unwrap_or(0.0),
        supported_sample_rates: available_rates(device),
        is_default_input: default_in == Some(device),
        is_default_output: default_out == Some(device),
    })
}

fn resolve_uid(uid: &str) -> Option<AudioDeviceID> {
    all_device_ids()
        .into_iter()
        .find(|id| cfstring_property(*id, kAudioDevicePropertyDeviceUID).as_deref() == Some(uid))
}

// ---------------------------------------------------------------------------
// Backend
// ---------------------------------------------------------------------------

/// The macOS CoreAudio backend.
#[derive(Debug, Default, Clone, Copy)]
pub struct CoreAudioBackend;

impl CoreAudioBackend {
    /// Create the backend. Enumeration is done lazily, per call.
    pub fn new() -> Self {
        Self
    }

    /// Open a concrete [`CoreAudioStream`].
    ///
    /// # Errors
    ///
    /// [`AudioError::DeviceNotFound`] if the UID no longer resolves,
    /// [`AudioError::ChannelOutOfRange`] for a channel the device lacks,
    /// [`AudioError::UnsupportedSampleRate`] if the device will not run at the
    /// requested rate, or [`AudioError::Backend`] wrapping an `OSStatus`.
    pub fn open_input(
        &mut self,
        config: &StreamConfig,
        callback: Box<dyn AudioCallback>,
    ) -> Result<CoreAudioStream, AudioError> {
        config.validate()?;

        if !config.output_channels.is_empty() {
            return Err(AudioError::Backend(
                "the CoreAudio backend is input-only for now".into(),
            ));
        }
        let uid = config
            .input
            .as_ref()
            .ok_or_else(|| AudioError::DeviceNotFound("no input device selected".into()))?;
        let device =
            resolve_uid(uid.as_str()).ok_or_else(|| AudioError::DeviceNotFound(uid.to_string()))?;

        let name =
            cfstring_property(device, kAudioObjectPropertyName).unwrap_or_else(|| uid.to_string());
        let available = channel_count(device, kAudioObjectPropertyScopeInput);
        if available == 0 {
            return Err(AudioError::ChannelOutOfRange {
                device: name,
                channel: 0,
                available: 0,
            });
        }
        for &channel in &config.input_channels {
            if channel >= available {
                return Err(AudioError::ChannelOutOfRange {
                    device: name,
                    channel,
                    available,
                });
            }
        }

        set_sample_rate(device, config.sample_rate, &name)?;
        let granted_frames = set_buffer_frames(device, config.buffer_frames)?;

        let selected = config.input_channels.clone();
        let selected_count = selected.len();

        // Every buffer the IOProc touches is allocated here, once.
        let mut state = Box::new(IoProcState {
            callback,
            interleaved: vec![0.0; MAX_BUFFER_FRAMES * selected_count.max(1)],
            no_output: Vec::new(),
            selected,
        });

        let mut proc_id: AudioDeviceIOProcID = None;
        // SAFETY: `state` is boxed, so the pointer stays valid while the stream
        // lives; Drop destroys the IOProc before the box is freed.
        let status = unsafe {
            AudioDeviceCreateIOProcID(
                device,
                Some(io_proc),
                (&raw mut *state).cast::<c_void>(),
                &mut proc_id,
            )
        };
        if status != 0 || proc_id.is_none() {
            return Err(AudioError::Backend(format!(
                "AudioDeviceCreateIOProcID failed with OSStatus {status}"
            )));
        }

        let mut granted = config.clone();
        granted.buffer_frames = granted_frames;
        granted.sample_rate = scalar::<f64>(
            device,
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal,
        )
        .unwrap_or(config.sample_rate);

        Ok(CoreAudioStream {
            device,
            proc_id,
            state,
            config: granted,
            latency: read_latency(device),
            running: false,
        })
    }
}

fn set_sample_rate(device: AudioDeviceID, requested: f64, name: &str) -> Result<(), AudioError> {
    let current = scalar::<f64>(
        device,
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
    )
    .unwrap_or(0.0);
    if (current - requested).abs() < 0.5 {
        return Ok(());
    }

    let addr = address(
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
    );
    let value = requested;
    // SAFETY: writes exactly one f64, which is what this property expects.
    let status = unsafe {
        AudioObjectSetPropertyData(
            device,
            &addr,
            0,
            ptr::null(),
            size_of::<f64>() as u32,
            (&raw const value).cast::<c_void>(),
        )
    };
    if status != 0 {
        return Err(AudioError::UnsupportedSampleRate {
            device: name.to_owned(),
            requested,
        });
    }
    Ok(())
}

/// Ask for a buffer size and report what the device actually granted.
fn set_buffer_frames(device: AudioDeviceID, requested: u32) -> Result<u32, AudioError> {
    let wanted = requested.clamp(16, MAX_BUFFER_FRAMES as u32);
    let addr = address(
        kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal,
    );
    // SAFETY: writes exactly one u32, as the property expects.
    let _ = unsafe {
        AudioObjectSetPropertyData(
            device,
            &addr,
            0,
            ptr::null(),
            size_of::<u32>() as u32,
            (&raw const wanted).cast::<c_void>(),
        )
    };

    // The device may refuse or round, so trust only what it reports back.
    let granted = scalar::<u32>(
        device,
        kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal,
    )
    .unwrap_or(wanted);

    if granted as usize > MAX_BUFFER_FRAMES {
        return Err(AudioError::Backend(format!(
            "device insists on {granted}-frame buffers, more than the {MAX_BUFFER_FRAMES} preallocated"
        )));
    }
    Ok(granted)
}

fn read_latency(device: AudioDeviceID) -> StreamLatency {
    StreamLatency {
        input_frames: scalar::<u32>(
            device,
            kAudioDevicePropertyLatency,
            kAudioObjectPropertyScopeInput,
        )
        .unwrap_or(0),
        output_frames: 0,
        safety_offset_frames: scalar::<u32>(
            device,
            kAudioDevicePropertySafetyOffset,
            kAudioObjectPropertyScopeInput,
        )
        .unwrap_or(0),
    }
}

impl AudioBackend for CoreAudioBackend {
    fn name(&self) -> &str {
        "coreaudio"
    }

    fn devices(&self) -> Result<Vec<DeviceInfo>, AudioError> {
        let default_in = default_device(kAudioHardwarePropertyDefaultInputDevice);
        let default_out = default_device(kAudioHardwarePropertyDefaultOutputDevice);
        Ok(all_device_ids()
            .into_iter()
            .filter_map(|id| describe(id, default_in, default_out))
            .collect())
    }

    fn open(
        &mut self,
        config: &StreamConfig,
        callback: Box<dyn AudioCallback>,
    ) -> Result<Box<dyn AudioStream>, AudioError> {
        Ok(Box::new(self.open_input(config, callback)?))
    }
}

// ---------------------------------------------------------------------------
// Stream
// ---------------------------------------------------------------------------

/// State the IOProc reaches through its client pointer.
///
/// Boxed and never moved while the stream lives. Every buffer here is sized at
/// open so the IOProc allocates nothing.
struct IoProcState {
    callback: Box<dyn AudioCallback>,
    /// Selected channels, interleaved, refilled each callback.
    interleaved: Vec<f32>,
    /// Empty: this backend is input-only, and `AudioBuffers` still wants a slice.
    no_output: Vec<f32>,
    selected: Vec<u32>,
}

/// A running CoreAudio input stream.
pub struct CoreAudioStream {
    device: AudioDeviceID,
    proc_id: AudioDeviceIOProcID,
    /// Never read, and must not be deleted. CoreAudio holds a raw pointer into
    /// this allocation; the field's only job is to keep it alive until Drop has
    /// destroyed the IOProc. Removing it as "unused" would be a use-after-free.
    #[allow(dead_code)]
    state: Box<IoProcState>,
    config: StreamConfig,
    latency: StreamLatency,
    running: bool,
}

// SAFETY: the only shared state is `state`, reached solely by the IOProc while
// the stream is running. `start`/`stop` bracket that access, and Drop destroys
// the IOProc before the box is freed, so no reference outlives the allocation.
unsafe impl Send for CoreAudioStream {}

impl AudioStream for CoreAudioStream {
    fn start(&mut self) -> Result<(), AudioError> {
        if self.running {
            return Err(AudioError::AlreadyRunning);
        }
        // SAFETY: proc_id came from AudioDeviceCreateIOProcID on this device.
        let status = unsafe { AudioDeviceStart(self.device, self.proc_id) };
        if status != 0 {
            return Err(describe_status(status, "AudioDeviceStart"));
        }
        self.running = true;
        Ok(())
    }

    fn stop(&mut self) -> Result<(), AudioError> {
        if !self.running {
            return Ok(());
        }
        // SAFETY: as above; stopping an already-stopped device is harmless.
        let status = unsafe { AudioDeviceStop(self.device, self.proc_id) };
        self.running = false;
        if status != 0 {
            return Err(describe_status(status, "AudioDeviceStop"));
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

impl Drop for CoreAudioStream {
    fn drop(&mut self) {
        let _ = self.stop();
        // Must happen before `state` is freed: the IOProc holds a pointer to it.
        // SAFETY: proc_id belongs to this device and is destroyed exactly once.
        unsafe {
            AudioDeviceDestroyIOProcID(self.device, self.proc_id);
        }
    }
}

impl fmt::Debug for CoreAudioStream {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("CoreAudioStream")
            .field("device", &self.device)
            .field("running", &self.running)
            .field("buffer_frames", &self.config.buffer_frames)
            .field("sample_rate", &self.config.sample_rate)
            .finish_non_exhaustive()
    }
}

/// The real-time callback CoreAudio invokes.
///
/// Runs on a thread with a hard deadline. It gathers the selected channels into
/// a preallocated buffer and calls through; it must not allocate, lock or panic.
/// A panic here would unwind into C, which aborts.
unsafe extern "C" fn io_proc(
    _device: AudioObjectID,
    _now: *const AudioTimeStamp,
    input: *const AudioBufferList,
    _input_time: *const AudioTimeStamp,
    _output: *mut AudioBufferList,
    _output_time: *const AudioTimeStamp,
    client: *mut c_void,
) -> OSStatus {
    if client.is_null() || input.is_null() {
        return 0;
    }
    // SAFETY: `client` is the pointer handed to AudioDeviceCreateIOProcID, which
    // points at a live boxed IoProcState for as long as the IOProc can run.
    let state = unsafe { &mut *client.cast::<IoProcState>() };

    // Destructured so the gather can borrow `interleaved` mutably while the
    // callback still gets at `no_output` and `callback`. Disjoint fields.
    let IoProcState {
        callback,
        interleaved,
        no_output,
        selected,
    } = state;

    // SAFETY: CoreAudio guarantees a well-formed AudioBufferList here.
    let frames = unsafe { gather_input(input, selected, interleaved) };
    if frames == 0 {
        return 0;
    }

    let channels = selected.len();
    let gathered = interleaved.get(..frames * channels).unwrap_or(&[]);
    let mut buffers = AudioBuffers::new(gathered, no_output, channels, 0, frames);
    callback.process(&mut buffers);
    0
}

/// Copy the selected channels out of CoreAudio's buffer list into one
/// interleaved slice.
///
/// The HAL presents one `AudioBuffer` per stream, so a device may hand over a
/// single interleaved buffer or several. Both shapes are flattened here so the
/// rest of the system only ever sees interleaved frames.
///
/// # Safety
///
/// `list` must be a valid `AudioBufferList` with `Float32` samples, as the HAL
/// always provides.
unsafe fn gather_input(
    list: *const AudioBufferList,
    selected_channels: &[u32],
    interleaved: &mut [f32],
) -> usize {
    // SAFETY: caller guarantees a valid list.
    let (buffer_count, buffers) =
        unsafe { ((*list).mNumberBuffers as usize, (*list).mBuffers.as_ptr()) };
    if buffer_count == 0 {
        return 0;
    }

    let selected = selected_channels.len();
    if selected == 0 {
        return 0;
    }

    // Frames is the same across buffers; take it from the first.
    // SAFETY: index 0 exists because buffer_count > 0.
    let first = unsafe { &*buffers };
    let first_channels = first.mNumberChannels.max(1) as usize;
    let frames = (first.mDataByteSize as usize / size_of::<f32>()) / first_channels;
    let frames = frames.min(interleaved.len() / selected);
    if frames == 0 {
        return 0;
    }

    for (slot, &wanted) in selected_channels.iter().enumerate() {
        // Walk the buffer list to find which stream holds this device channel.
        let mut base = 0_u32;
        let mut source: Option<(*const f32, usize, usize)> = None;
        for index in 0..buffer_count {
            // SAFETY: index < buffer_count.
            let buffer = unsafe { &*buffers.add(index) };
            let channels = buffer.mNumberChannels;
            if wanted < base + channels {
                source = Some((
                    buffer.mData.cast::<f32>(),
                    (wanted - base) as usize,
                    channels.max(1) as usize,
                ));
                break;
            }
            base += channels;
        }

        let Some((data, offset, stride)) = source else {
            continue;
        };
        if data.is_null() {
            continue;
        }
        for frame in 0..frames {
            // SAFETY: `frame * stride + offset` stays inside this buffer, whose
            // byte size gave us `frames` above.
            let sample = unsafe { *data.add(frame * stride + offset) };
            if let Some(out) = interleaved.get_mut(frame * selected + slot) {
                *out = sample;
            }
        }
    }

    frames
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    /// Enumeration must not crash and must find something. Every Mac has at
    /// least a built-in output, so an empty list means the property calls are
    /// broken rather than that the machine is unusual.
    #[test]
    fn enumerates_devices() {
        let backend = CoreAudioBackend::new();
        let devices = backend.devices().expect("enumeration should succeed");
        assert!(!devices.is_empty(), "no CoreAudio devices found at all");

        for device in &devices {
            assert!(
                !device.id.as_str().is_empty(),
                "device UID must not be empty"
            );
            assert!(!device.name.is_empty(), "device name must not be empty");
        }
    }

    #[test]
    fn a_default_input_or_output_exists() {
        let backend = CoreAudioBackend::new();
        let has_default = backend.default_input().unwrap().is_some()
            || backend.default_output().unwrap().is_some();
        assert!(has_default, "expected at least one default device");
    }

    #[test]
    fn device_uids_resolve_back_to_numeric_ids() {
        let backend = CoreAudioBackend::new();
        for device in backend.devices().unwrap() {
            assert!(
                resolve_uid(device.id.as_str()).is_some(),
                "UID {} did not resolve back",
                device.id
            );
        }
    }

    #[test]
    fn unknown_uid_is_reported_not_panicked() {
        let mut backend = CoreAudioBackend::new();
        let config = StreamConfig {
            input: Some(DeviceId::new("no-such-device-uid")),
            output: None,
            sample_rate: 48_000.0,
            buffer_frames: 512,
            input_channels: vec![0],
            output_channels: Vec::new(),
        };
        let result = backend.open_input(&config, Box::new(|_: &mut AudioBuffers<'_>| {}));
        assert!(matches!(result, Err(AudioError::DeviceNotFound(_))));
    }

    #[test]
    fn output_channels_are_rejected_while_the_backend_is_input_only() {
        let mut backend = CoreAudioBackend::new();
        let config = StreamConfig {
            input: Some(DeviceId::new("whatever")),
            output: None,
            sample_rate: 48_000.0,
            buffer_frames: 512,
            input_channels: vec![0],
            output_channels: vec![0],
        };
        let result = backend.open_input(&config, Box::new(|_: &mut AudioBuffers<'_>| {}));
        assert!(matches!(result, Err(AudioError::Backend(_))));
    }
}
