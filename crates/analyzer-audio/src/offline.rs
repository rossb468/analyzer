//! A backend that reads from memory instead of hardware.
//!
//! Two jobs. It drives the headless harness, which is how numerical correctness
//! gets proven before any UI exists. And it forces [`AudioBackend`] to have a
//! working implementation from the start, so the trait is shaped by something
//! real rather than by guesswork about what CoreAudio will want.
//!
//! It is explicitly **not** real-time: [`OfflineStream::run_to_end`] runs the
//! callback as fast as it can. That makes it useful for tests and useless for
//! measuring latency.

use std::fmt;

use crate::backend::AudioBackend;
use crate::device::{DeviceId, DeviceInfo};
use crate::error::AudioError;
use crate::stream::{AudioBuffers, AudioCallback, AudioStream, StreamConfig, StreamLatency};

/// Device id the offline backend reports.
pub const OFFLINE_DEVICE_ID: &str = "offline";

/// Interleaved sample material to feed a stream.
#[derive(Debug, Clone, PartialEq)]
pub struct Source {
    /// Interleaved samples, `frames * channels` long.
    pub samples: Vec<f32>,
    /// Channels the interleaving assumes.
    pub channels: usize,
    /// Nominal rate, reported through the device info.
    pub sample_rate: f64,
}

impl Source {
    /// Build a source from interleaved samples.
    ///
    /// # Panics
    ///
    /// Panics if `channels` is zero or the sample count is not a whole number of
    /// frames.
    pub fn new(samples: Vec<f32>, channels: usize, sample_rate: f64) -> Self {
        assert!(channels > 0, "source must have at least one channel");
        assert_eq!(
            samples.len() % channels,
            0,
            "sample count must be a whole number of frames"
        );
        Self {
            samples,
            channels,
            sample_rate,
        }
    }

    /// Build a single-channel source.
    pub fn mono(samples: Vec<f32>, sample_rate: f64) -> Self {
        Self::new(samples, 1, sample_rate)
    }

    /// Frames of material available.
    pub fn frames(&self) -> usize {
        self.samples.len() / self.channels
    }
}

/// [`AudioBackend`] over an in-memory [`Source`].
#[derive(Debug, Clone)]
pub struct OfflineBackend {
    source: Source,
    block_frames: usize,
}

impl OfflineBackend {
    /// Wrap `source`, delivering `block_frames` per callback.
    ///
    /// # Panics
    ///
    /// Panics if `block_frames` is zero.
    pub fn new(source: Source, block_frames: usize) -> Self {
        assert!(block_frames > 0, "block size must be non-zero");
        Self {
            source,
            block_frames,
        }
    }

    /// Open a concrete [`OfflineStream`].
    ///
    /// Prefer this over [`AudioBackend::open`] whenever the offline controls are
    /// needed. The trait returns `Box<dyn AudioStream>`, which erases
    /// [`OfflineStream::pump`], [`OfflineStream::run_to_end`] and
    /// [`OfflineStream::captured_output`] — and a hardware stream has no
    /// equivalent of those to justify putting them on the trait, since the OS
    /// drives it rather than the caller.
    ///
    /// # Errors
    ///
    /// Returns [`AudioError::ChannelOutOfRange`] for a channel the synthetic
    /// device does not have, or whatever [`StreamConfig::validate`] rejects.
    pub fn open_offline(
        &mut self,
        config: &StreamConfig,
        callback: Box<dyn AudioCallback>,
    ) -> Result<OfflineStream, AudioError> {
        config.validate()?;

        let device = self.device();
        for &channel in &config.input_channels {
            if channel >= device.input_channels {
                return Err(AudioError::ChannelOutOfRange {
                    device: device.name.clone(),
                    channel,
                    available: device.input_channels,
                });
            }
        }
        for &channel in &config.output_channels {
            if channel >= device.output_channels {
                return Err(AudioError::ChannelOutOfRange {
                    device: device.name.clone(),
                    channel,
                    available: device.output_channels,
                });
            }
        }

        let inputs = config.input_channels.len();
        let outputs = config.output_channels.len();
        let block = self.block_frames;

        Ok(OfflineStream {
            config: config.clone(),
            callback,
            source: self.source.clone(),
            position: 0,
            block,
            input_scratch: vec![0.0; block * inputs],
            output_scratch: vec![0.0; block * outputs],
            captured: Vec::new(),
            running: false,
        })
    }

    fn device(&self) -> DeviceInfo {
        DeviceInfo {
            id: DeviceId::new(OFFLINE_DEVICE_ID),
            name: "Offline (in-memory)".into(),
            input_channels: self.source.channels as u32,
            // Enough to exercise multi-channel routing without pretending to be
            // a real interface.
            output_channels: 2,
            default_sample_rate: self.source.sample_rate,
            supported_sample_rates: vec![self.source.sample_rate],
            is_default_input: true,
            is_default_output: true,
        }
    }
}

impl AudioBackend for OfflineBackend {
    fn name(&self) -> &str {
        "offline"
    }

    fn devices(&self) -> Result<Vec<DeviceInfo>, AudioError> {
        Ok(vec![self.device()])
    }

    fn open(
        &mut self,
        config: &StreamConfig,
        callback: Box<dyn AudioCallback>,
    ) -> Result<Box<dyn AudioStream>, AudioError> {
        Ok(Box::new(self.open_offline(config, callback)?))
    }
}

/// A stream that pulls from memory. Not real-time.
pub struct OfflineStream {
    config: StreamConfig,
    callback: Box<dyn AudioCallback>,
    source: Source,
    position: usize,
    block: usize,
    input_scratch: Vec<f32>,
    output_scratch: Vec<f32>,
    captured: Vec<f32>,
    running: bool,
}

impl OfflineStream {
    /// Run one block through the callback, returning frames processed.
    ///
    /// Returns zero once the source is exhausted.
    pub fn pump(&mut self) -> usize {
        let Self {
            config,
            callback,
            source,
            position,
            block,
            input_scratch,
            output_scratch,
            captured,
            ..
        } = self;

        let remaining = source.frames().saturating_sub(*position);
        if remaining == 0 {
            return 0;
        }
        let frames = (*block).min(remaining);

        let inputs = config.input_channels.len();
        let outputs = config.output_channels.len();

        // Gather only the selected channels, in the order requested.
        for frame in 0..frames {
            let src_base = (*position + frame) * source.channels;
            for (slot, &channel) in config.input_channels.iter().enumerate() {
                input_scratch[frame * inputs + slot] = source.samples[src_base + channel as usize];
            }
        }

        // Never hand the callback stale output contents.
        output_scratch[..frames * outputs].fill(0.0);

        let mut buffers = AudioBuffers::new(
            &input_scratch[..frames * inputs],
            &mut output_scratch[..frames * outputs],
            inputs,
            outputs,
            frames,
        );
        callback.process(&mut buffers);

        captured.extend_from_slice(&output_scratch[..frames * outputs]);
        *position += frames;
        frames
    }

    /// Run the whole source through the callback, returning total frames.
    pub fn run_to_end(&mut self) -> usize {
        let mut total = 0;
        loop {
            let frames = self.pump();
            if frames == 0 {
                return total;
            }
            total += frames;
        }
    }

    /// Everything the callback wrote, interleaved across the selected output
    /// channels.
    pub fn captured_output(&self) -> &[f32] {
        &self.captured
    }

    /// Frames consumed so far.
    pub fn position(&self) -> usize {
        self.position
    }

    /// Rewind and discard captured output.
    pub fn rewind(&mut self) {
        self.position = 0;
        self.captured.clear();
    }
}

impl AudioStream for OfflineStream {
    fn start(&mut self) -> Result<(), AudioError> {
        if self.running {
            return Err(AudioError::AlreadyRunning);
        }
        self.running = true;
        Ok(())
    }

    fn stop(&mut self) -> Result<(), AudioError> {
        self.running = false;
        Ok(())
    }

    fn is_running(&self) -> bool {
        self.running
    }

    fn config(&self) -> &StreamConfig {
        &self.config
    }

    fn latency(&self) -> StreamLatency {
        // Nothing physical is involved, so there is genuinely no latency to
        // report. Claiming a plausible-looking number would be worse than zero.
        StreamLatency::default()
    }
}

impl fmt::Debug for OfflineStream {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("OfflineStream")
            .field("position", &self.position)
            .field("frames", &self.source.frames())
            .field("block", &self.block)
            .field("running", &self.running)
            .finish_non_exhaustive()
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    fn ramp(frames: usize, channels: usize) -> Source {
        // Frame n, channel c holds n + c/10, so both axes are identifiable.
        let samples = (0..frames)
            .flat_map(|n| (0..channels).map(move |c| n as f32 + c as f32 / 10.0))
            .collect();
        Source::new(samples, channels, 48_000.0)
    }

    fn config(inputs: Vec<u32>, outputs: Vec<u32>) -> StreamConfig {
        StreamConfig {
            input: Some(DeviceId::new(OFFLINE_DEVICE_ID)),
            output: Some(DeviceId::new(OFFLINE_DEVICE_ID)),
            sample_rate: 48_000.0,
            buffer_frames: 4,
            input_channels: inputs,
            output_channels: outputs,
        }
    }

    #[test]
    fn source_reports_frames() {
        assert_eq!(ramp(10, 2).frames(), 10);
        assert_eq!(Source::mono(vec![0.0; 7], 48_000.0).frames(), 7);
    }

    #[test]
    #[should_panic(expected = "whole number of frames")]
    fn ragged_source_is_rejected() {
        let _ = Source::new(vec![0.0; 5], 2, 48_000.0);
    }

    #[test]
    fn enumerates_one_device() {
        let backend = OfflineBackend::new(ramp(4, 2), 2);
        let devices = backend.devices().unwrap();
        assert_eq!(devices.len(), 1);
        assert!(devices[0].has_input() && devices[0].has_output());
        assert!(backend.default_input().unwrap().is_some());
        assert!(backend.default_output().unwrap().is_some());
    }

    #[test]
    fn rejects_out_of_range_input_channel() {
        let mut backend = OfflineBackend::new(ramp(4, 2), 2);
        let result = backend.open(
            &config(vec![7], vec![]),
            Box::new(|_: &mut AudioBuffers<'_>| {}),
        );
        assert!(matches!(
            result,
            Err(AudioError::ChannelOutOfRange { channel: 7, .. })
        ));
    }

    #[test]
    fn delivers_selected_channels_in_requested_order() {
        let mut backend = OfflineBackend::new(ramp(3, 2), 3);
        let mut stream = backend
            .open_offline(
                // Reversed on purpose: order must follow the request, not the device.
                &config(vec![1, 0], vec![]),
                Box::new(|buffers: &mut AudioBuffers<'_>| {
                    // ramp() makes channel 1 larger than channel 0 in every
                    // frame, so slot 0 holding the larger value proves the
                    // requested order was honoured rather than the device order.
                    let slot0 = buffers.input_channel(0).next().unwrap_or_default();
                    let slot1 = buffers.input_channel(1).next().unwrap_or_default();
                    assert!(
                        slot0 > slot1,
                        "slot 0 should carry device channel 1: {slot0} vs {slot1}"
                    );
                }),
            )
            .unwrap();

        assert!(stream.start().is_ok());
        // 3 frames of material, block of 3.
        assert_eq!(stream.run_to_end(), 3);
    }

    #[test]
    fn blocks_split_the_source_and_stop_at_the_end() {
        let mut backend = OfflineBackend::new(ramp(10, 1), 4);
        let mut blocks = 0_usize;
        let mut stream = backend
            .open_offline(
                &config(vec![0], vec![]),
                Box::new(move |_: &mut AudioBuffers<'_>| {}),
            )
            .unwrap();

        loop {
            let frames = stream.pump();
            if frames == 0 {
                break;
            }
            blocks += 1;
        }
        // 4 + 4 + 2
        assert_eq!(blocks, 3);
        assert_eq!(stream.position(), 10);
        assert_eq!(stream.pump(), 0, "exhausted stream stays exhausted");
    }

    #[test]
    fn captures_what_the_callback_writes() {
        let mut backend = OfflineBackend::new(ramp(4, 1), 2);
        let mut stream = backend
            .open_offline(
                &config(vec![0], vec![0, 1]),
                Box::new(|buffers: &mut AudioBuffers<'_>| {
                    buffers.output_mut().fill(0.25);
                }),
            )
            .unwrap();

        stream.run_to_end();
        // 4 frames x 2 output channels.
        assert_eq!(stream.captured_output().len(), 8);
        assert!(stream.captured_output().iter().all(|s| *s == 0.25));
    }

    #[test]
    fn output_buffer_is_cleared_between_blocks() {
        let mut backend = OfflineBackend::new(ramp(4, 1), 2);
        let mut first = true;
        let mut stream = backend
            .open_offline(
                &config(vec![0], vec![0]),
                Box::new(move |buffers: &mut AudioBuffers<'_>| {
                    if first {
                        buffers.output_mut().fill(1.0);
                        first = false;
                    } else {
                        // Second block must not inherit the first block's data.
                        assert!(buffers.output_mut().iter().all(|s| *s == 0.0));
                    }
                }),
            )
            .unwrap();
        assert_eq!(stream.run_to_end(), 4);
    }

    #[test]
    fn rewind_replays_from_the_start() {
        let mut backend = OfflineBackend::new(ramp(4, 1), 4);
        let mut stream = backend
            .open_offline(
                &config(vec![0], vec![0]),
                Box::new(|buffers: &mut AudioBuffers<'_>| buffers.output_mut().fill(1.0)),
            )
            .unwrap();

        stream.run_to_end();
        assert_eq!(stream.captured_output().len(), 4);

        stream.rewind();
        assert_eq!(stream.position(), 0);
        assert!(stream.captured_output().is_empty());
        assert_eq!(stream.run_to_end(), 4);
    }

    #[test]
    fn double_start_is_an_error_and_stop_is_idempotent() {
        let mut backend = OfflineBackend::new(ramp(2, 1), 2);
        let mut stream = backend
            .open_offline(
                &config(vec![0], vec![]),
                Box::new(|_: &mut AudioBuffers<'_>| {}),
            )
            .unwrap();

        assert!(stream.start().is_ok());
        assert!(matches!(stream.start(), Err(AudioError::AlreadyRunning)));
        assert!(stream.stop().is_ok());
        assert!(stream.stop().is_ok());
        assert!(!stream.is_running());
    }
}
