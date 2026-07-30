//! Stream configuration, the real-time callback contract, and stream control.

use std::fmt;

use crate::device::DeviceId;
use crate::error::AudioError;

/// What to open.
///
/// Input and output are named separately because measurement rigs routinely use
/// two different devices, and on macOS that means an aggregate device. Neither
/// side is required, so this also covers analyse-only and generate-only setups.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct StreamConfig {
    /// Capture device, if capturing.
    pub input: Option<DeviceId>,
    /// Playback device, if playing.
    pub output: Option<DeviceId>,
    /// Requested rate in hertz.
    pub sample_rate: f64,
    /// Requested callback size in frames. Backends may round this.
    pub buffer_frames: u32,
    /// Device channel indices to capture, in the order they should be delivered.
    pub input_channels: Vec<u32>,
    /// Device channel indices to play to, in the order buffers supply them.
    pub output_channels: Vec<u32>,
}

impl StreamConfig {
    /// Basic self-consistency check, before a backend touches the hardware.
    ///
    /// # Errors
    ///
    /// Returns [`AudioError::NothingToDo`] if neither direction has channels, and
    /// [`AudioError::UnsupportedSampleRate`] for a non-positive rate.
    pub fn validate(&self) -> Result<(), AudioError> {
        if self.input_channels.is_empty() && self.output_channels.is_empty() {
            return Err(AudioError::NothingToDo);
        }
        if self.sample_rate <= 0.0 {
            return Err(AudioError::UnsupportedSampleRate {
                device: "stream".into(),
                requested: self.sample_rate,
            });
        }
        Ok(())
    }
}

/// Where the hardware's latency actually sits.
///
/// This exists for the transfer-function reference problem. Measuring a system
/// needs to know what was sent as well as what was heard; the accurate way is a
/// physical loopback cable, but the convenient way is to use the generated
/// buffer as the reference and compensate for the round trip. That is only
/// possible if the backend reports these honestly, and the numbers are
/// approximate on every platform — hence a one-time user calibration on top.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct StreamLatency {
    /// Frames of delay between sound arriving and the callback seeing it.
    pub input_frames: u32,
    /// Frames of delay between the callback writing and sound emerging.
    pub output_frames: u32,
    /// Additional device safety offset the driver imposes.
    pub safety_offset_frames: u32,
}

impl StreamLatency {
    /// Total output-to-input delay in frames.
    pub fn round_trip_frames(&self) -> u32 {
        self.input_frames
            .saturating_add(self.output_frames)
            .saturating_add(self.safety_offset_frames)
    }

    /// Total output-to-input delay in seconds at `sample_rate`.
    pub fn round_trip_seconds(&self, sample_rate: f64) -> f64 {
        if sample_rate <= 0.0 {
            return 0.0;
        }
        f64::from(self.round_trip_frames()) / sample_rate
    }
}

/// Interleaved buffers for one callback invocation.
///
/// Both sides are interleaved as the hardware presents them. Deinterleaving is
/// the engine's job, not the backend's, so there is exactly one place that
/// decides the memory layout the analysis chain sees.
#[derive(Debug)]
pub struct AudioBuffers<'a> {
    input: &'a [f32],
    output: &'a mut [f32],
    input_channels: usize,
    output_channels: usize,
    frames: usize,
}

impl<'a> AudioBuffers<'a> {
    /// Wrap a backend's buffers.
    ///
    /// # Panics
    ///
    /// Panics if either slice length disagrees with `frames * channels`. A
    /// backend getting this wrong is a bug that must not be papered over on the
    /// real-time path.
    pub fn new(
        input: &'a [f32],
        output: &'a mut [f32],
        input_channels: usize,
        output_channels: usize,
        frames: usize,
    ) -> Self {
        assert_eq!(
            input.len(),
            frames * input_channels,
            "input buffer must be frames * input_channels"
        );
        assert_eq!(
            output.len(),
            frames * output_channels,
            "output buffer must be frames * output_channels"
        );
        Self {
            input,
            output,
            input_channels,
            output_channels,
            frames,
        }
    }

    /// Frames in this callback.
    pub fn frames(&self) -> usize {
        self.frames
    }

    /// Number of interleaved capture channels.
    pub fn input_channels(&self) -> usize {
        self.input_channels
    }

    /// Number of interleaved playback channels.
    pub fn output_channels(&self) -> usize {
        self.output_channels
    }

    /// The interleaved capture buffer.
    pub fn input(&self) -> &[f32] {
        self.input
    }

    /// The interleaved playback buffer.
    pub fn output_mut(&mut self) -> &mut [f32] {
        self.output
    }

    /// Samples of one capture channel, in order.
    ///
    /// Yields nothing if `channel` is out of range, rather than panicking: this
    /// runs on the real-time thread, where a panic is worse than silence.
    pub fn input_channel(&self, channel: usize) -> impl Iterator<Item = f32> + '_ {
        let stride = self.input_channels;
        let valid = channel < stride;
        self.input
            .iter()
            .skip(if valid { channel } else { 0 })
            .step_by(stride.max(1))
            .take(if valid { self.frames } else { 0 })
            .copied()
    }

    /// Write silence to the whole playback buffer.
    ///
    /// Backends do not guarantee the buffer arrives zeroed, and stale contents
    /// played back at full scale is the loudest possible bug.
    pub fn silence_output(&mut self) {
        self.output.fill(0.0);
    }
}

/// The real-time audio callback.
///
/// # Real-time contract
///
/// [`AudioCallback::process`] runs on a thread with a hard deadline — typically
/// 2.67 ms at 128 frames and 48 kHz. Returning late means the hardware plays
/// whatever was in the buffer, which is an audible click, and for a measurement
/// tool it silently corrupts the data being collected.
///
/// So `process` must not:
///
/// - allocate or free memory (unbounded worst case),
/// - take a lock (a lower-priority thread holding it inverts priority),
/// - log, touch the filesystem, or make any syscall,
/// - do anything that can page-fault or block.
///
/// In practice it should deinterleave into a lock-free queue and return. The
/// expensive work belongs on an analysis thread reading the other end.
///
/// This is enforced rather than trusted: debug and test builds run the callback
/// inside an allocation trap that aborts on violation.
pub trait AudioCallback: Send + 'static {
    /// Consume `buffers.input()` and fill `buffers.output_mut()`.
    fn process(&mut self, buffers: &mut AudioBuffers<'_>);
}

impl<F> AudioCallback for F
where
    F: FnMut(&mut AudioBuffers<'_>) + Send + 'static,
{
    fn process(&mut self, buffers: &mut AudioBuffers<'_>) {
        self(buffers);
    }
}

/// A configured, controllable stream.
pub trait AudioStream: Send + fmt::Debug {
    /// Begin calling the callback.
    ///
    /// # Errors
    ///
    /// Returns [`AudioError::AlreadyRunning`] if already started, or a backend
    /// error if the hardware refuses.
    fn start(&mut self) -> Result<(), AudioError>;

    /// Stop calling the callback. Idempotent.
    ///
    /// # Errors
    ///
    /// Returns a backend error if the platform fails to stop the stream.
    fn stop(&mut self) -> Result<(), AudioError>;

    /// Whether the callback is currently being invoked.
    fn is_running(&self) -> bool;

    /// What the backend actually granted.
    ///
    /// Rarely identical to what was requested: buffer sizes get rounded and
    /// rates get substituted, and everything downstream must use these values
    /// rather than the requested ones.
    fn config(&self) -> &StreamConfig;

    /// Hardware latency, for reference-signal compensation.
    fn latency(&self) -> StreamLatency;
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    #[test]
    fn validate_rejects_empty_channel_sets() {
        let config = StreamConfig {
            sample_rate: 48_000.0,
            ..Default::default()
        };
        assert!(matches!(config.validate(), Err(AudioError::NothingToDo)));
    }

    #[test]
    fn validate_rejects_non_positive_rate() {
        let config = StreamConfig {
            sample_rate: 0.0,
            input_channels: vec![0],
            ..Default::default()
        };
        assert!(matches!(
            config.validate(),
            Err(AudioError::UnsupportedSampleRate { .. })
        ));
    }

    #[test]
    fn validate_accepts_input_only() {
        let config = StreamConfig {
            sample_rate: 48_000.0,
            buffer_frames: 128,
            input_channels: vec![0, 1],
            ..Default::default()
        };
        assert!(config.validate().is_ok());
    }

    #[test]
    fn round_trip_latency_sums_all_three_parts() {
        let latency = StreamLatency {
            input_frames: 100,
            output_frames: 200,
            safety_offset_frames: 33,
        };
        assert_eq!(latency.round_trip_frames(), 333);
        let seconds = latency.round_trip_seconds(48_000.0);
        assert!((seconds - 333.0 / 48_000.0).abs() < 1e-12);
        // A zero rate must not divide by zero.
        assert_eq!(latency.round_trip_seconds(0.0), 0.0);
    }

    #[test]
    fn input_channel_deinterleaves() {
        // Two channels, three frames: L0 R0 L1 R1 L2 R2
        let input = [1.0, -1.0, 2.0, -2.0, 3.0, -3.0];
        let mut output = [0.0; 3];
        let buffers = AudioBuffers::new(&input, &mut output, 2, 1, 3);

        let left: Vec<f32> = buffers.input_channel(0).collect();
        let right: Vec<f32> = buffers.input_channel(1).collect();
        assert_eq!(left, vec![1.0, 2.0, 3.0]);
        assert_eq!(right, vec![-1.0, -2.0, -3.0]);
    }

    #[test]
    fn out_of_range_channel_yields_nothing_rather_than_panicking() {
        let input = [1.0, 2.0];
        let mut output = [0.0; 2];
        let buffers = AudioBuffers::new(&input, &mut output, 1, 1, 2);
        assert_eq!(buffers.input_channel(5).count(), 0);
    }

    #[test]
    fn silence_output_zeroes_everything() {
        let input: [f32; 0] = [];
        let mut output = [0.5, -0.5, 0.25, -0.25];
        let mut buffers = AudioBuffers::new(&input, &mut output, 0, 2, 2);
        buffers.silence_output();
        assert!(buffers.output_mut().iter().all(|s| *s == 0.0));
    }

    #[test]
    #[should_panic(expected = "input buffer must be")]
    fn mismatched_input_length_panics() {
        let input = [1.0, 2.0, 3.0];
        let mut output = [0.0; 2];
        let _ = AudioBuffers::new(&input, &mut output, 2, 1, 2);
    }

    #[test]
    fn closures_are_callbacks() {
        fn takes_callback(mut callback: impl AudioCallback) {
            let input: [f32; 0] = [];
            let mut output = [1.0, 1.0];
            let mut buffers = AudioBuffers::new(&input, &mut output, 0, 1, 2);
            callback.process(&mut buffers);
            assert!(output.iter().all(|s| *s == 0.0));
        }
        takes_callback(|buffers: &mut AudioBuffers<'_>| buffers.silence_output());
    }
}
