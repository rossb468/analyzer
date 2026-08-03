//! The analysis worker and the graph it runs.
//!
//! [`ring`](crate::ring) and [`snapshot`](crate::snapshot) are plumbing; this is
//! the thing that owns a thread and connects them. It sits between an audio
//! callback and a user interface:
//!
//! 1. [`Engine::start`] hands back a [`CaptureSink`] for the audio callback and
//!    spawns a worker.
//! 2. The worker drains the ring, feeds the spectrum analyzer, and publishes a
//!    [`SpectrumFrame`] whenever new frames complete.
//! 3. The UI reads the newest frame whenever it feels like drawing.
//!
//! # Reconfiguration
//!
//! There is none. Changing FFT size, window or channel count means dropping the
//! engine and starting another. Live reconfiguration would need the analysis
//! buffers resized underneath a running audio callback, and the resulting
//! synchronisation is not worth it for something a user does by clicking a menu.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use analyzer_dsp::{
    Averaging, DelayFinder, SpectrumAnalyzer, SpectrumConfig, TransferAveraging, TransferConfig,
    TransferFunction,
};

use crate::ring::{CaptureSink, CaptureSource, capture_ring};
use crate::snapshot::{SnapshotReader, snapshot_channel};

/// How long the worker sleeps when the ring is empty.
///
/// A compromise. Blocking on a condvar would need the audio thread to signal,
/// and signalling is not something a thread on a hard deadline should do.
/// Spinning would burn a core. At 48 kHz a 128-frame block arrives every 2.7 ms,
/// so polling at 1 ms adds well under a millisecond of latency and costs almost
/// nothing, since the machine is already awake servicing audio.
const IDLE_POLL: Duration = Duration::from_millis(1);

/// Frames read from the ring per iteration.
///
/// This bound is load-bearing, not a tuning knob. Draining until the ring is
/// empty looks natural and is wrong: a producer that keeps the ring topped up
/// means the drain loop never exits and nothing is ever published. Reading a
/// bounded amount and then publishing guarantees the UI sees progress no matter
/// how fast audio arrives.
const DRAIN_FRAMES: usize = 4096;

/// One published analysis result.
///
/// Cheap to clone-free-publish: the worker mutates this in place inside the
/// triple buffer, so a frame costs no allocation once running.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct SpectrumFrame {
    /// Increments once per publication. Lets a reader tell a repeat from a
    /// genuinely new frame, and makes a torn read detectable in tests.
    pub sequence: u64,
    /// Level per bin in dBFS, 0 dBFS being a full-scale sine.
    ///
    /// The live trace: whatever averaging the user asked for, usually short so
    /// it tracks what is happening now.
    pub bins: Vec<f32>,
    /// The same spectrum under long-term averaging, accumulated independently.
    ///
    /// A separate analyzer rather than a smoothed copy of `bins`. Smoothing the
    /// already-averaged live trace would compound two time constants and give a
    /// curve that is neither responsive nor statistically better; two analyzers
    /// over the same samples give a genuinely lower-variance estimate while the
    /// live trace stays fast.
    pub average_bins: Vec<f32>,
    /// Frames folded into the long-term average, which is what makes it
    /// trustworthy. Reported so a UI can show progress rather than an
    /// indistinguishable curve.
    pub average_frames: u32,
    /// Hertz between adjacent bins.
    pub bin_spacing_hz: f32,
    /// Rate the analysis ran at.
    pub sample_rate: f32,
    /// Frames folded into the current average.
    pub frames_averaged: u32,
    /// Blocks the audio callback had to drop. Non-zero invalidates the
    /// measurement and the UI is expected to say so.
    pub overruns: u64,

    /// Transfer function magnitude per bin, in decibels.
    ///
    /// Empty in [`AnalysisMode::Spectrum`]. Kept on the same frame rather than
    /// published separately so a UI never draws a magnitude from one instant
    /// beside a coherence from another.
    pub transfer_magnitude_db: Vec<f32>,
    /// Transfer function phase per bin, in degrees.
    pub transfer_phase_degrees: Vec<f32>,
    /// Coherence per bin, `0..=1`.
    pub transfer_coherence: Vec<f32>,
    /// Samples of delay currently applied to the reference channel.
    pub transfer_delay_frames: u32,
    /// Frames folded into the transfer function average.
    ///
    /// Coherence is identically 1 for a single frame, so a UI should refuse to
    /// draw it until this is comfortably above one.
    pub transfer_frames: u32,
}

impl SpectrumFrame {
    /// Centre frequency of bin `index`.
    pub fn bin_frequency(&self, index: usize) -> f32 {
        index as f32 * self.bin_spacing_hz
    }
}

/// What the engine computes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum AnalysisMode {
    /// A single-channel spectrum. The live trace and its long-term average.
    #[default]
    Spectrum,
    /// A two-channel transfer function alongside the spectrum.
    ///
    /// The spectrum still runs, on the measurement channel, because a user
    /// wants to see the raw level as well as the response.
    Transfer {
        /// Channel carrying what was sent.
        reference_channel: usize,
        /// Channel carrying what was heard.
        measurement_channel: usize,
    },
}

/// How to set the engine up.
#[derive(Debug, Clone, PartialEq)]
pub struct EngineConfig {
    /// Channels the audio callback will deliver, interleaved.
    pub channels: usize,
    /// Which of them to analyse.
    pub analysis_channel: usize,
    /// Spectrum settings, including sample rate and FFT size.
    pub spectrum: SpectrumConfig,
    /// Averaging for the long-term trace. Everything else is taken from
    /// `spectrum`, so the two analyzers differ only in how they average.
    pub average: Averaging,
    /// What to compute.
    pub mode: AnalysisMode,
    /// Ring capacity. Sized by worst-case scheduling latency, not throughput —
    /// 8192 frames is roughly 170 ms of runway at 48 kHz.
    pub ring_capacity_frames: usize,
}

impl EngineConfig {
    /// Single-channel capture at the given rate, with sensible defaults.
    pub fn mono(sample_rate: f32) -> Self {
        Self {
            channels: 1,
            analysis_channel: 0,
            spectrum: SpectrumConfig {
                sample_rate,
                ..SpectrumConfig::default()
            },
            average: Averaging::Infinite,
            mode: AnalysisMode::Spectrum,
            ring_capacity_frames: 8192,
        }
    }
}

/// Owns the analysis thread and exposes the newest result.
///
/// Dropping stops the worker and joins it.
#[derive(Debug)]
pub struct Engine {
    worker: Option<JoinHandle<()>>,
    stop: Arc<AtomicBool>,
    reader: SnapshotReader<SpectrumFrame>,
    published: Arc<AtomicU64>,
    reset_average: Arc<AtomicBool>,
    applied_delay: Arc<AtomicU32>,
    estimate_delay: Arc<AtomicBool>,
    config: EngineConfig,
}

impl Engine {
    /// Spawn the worker and return it alongside the sink for the audio callback.
    ///
    /// The sink is deliberately separate: it is the only part that crosses onto
    /// the real-time thread, and it is not `Clone`, so a second producer is a
    /// compile error.
    ///
    /// # Panics
    ///
    /// Panics if the configuration is inconsistent — zero channels, an analysis
    /// channel that does not exist, or a zero-length ring.
    pub fn start(config: EngineConfig) -> (CaptureSink, Self) {
        assert!(config.channels > 0, "engine needs at least one channel");
        assert!(
            config.analysis_channel < config.channels,
            "analysis channel {} does not exist among {} channels",
            config.analysis_channel,
            config.channels
        );
        if let AnalysisMode::Transfer {
            reference_channel,
            measurement_channel,
        } = config.mode
        {
            assert!(
                reference_channel < config.channels && measurement_channel < config.channels,
                "transfer channels {reference_channel}/{measurement_channel} do not exist \
                 among {} channels",
                config.channels
            );
            assert_ne!(
                reference_channel, measurement_channel,
                "reference and measurement must be different channels; the same channel \
                 against itself is a wire, not a measurement"
            );
        }

        let (sink, source) = capture_ring(config.channels, config.ring_capacity_frames);

        let analyzer = SpectrumAnalyzer::new(config.spectrum);
        // Same transform, different averaging. Sharing the configuration is what
        // keeps the two traces directly comparable.
        let average = SpectrumAnalyzer::new(SpectrumConfig {
            averaging: config.average,
            ..config.spectrum
        });
        let initial = SpectrumFrame {
            sequence: 0,
            bins: vec![analyzer_dsp::spectrum::DB_FLOOR; analyzer.bins()],
            average_bins: vec![analyzer_dsp::spectrum::DB_FLOOR; analyzer.bins()],
            average_frames: 0,
            bin_spacing_hz: analyzer.bin_spacing_hz(),
            sample_rate: config.spectrum.sample_rate,
            frames_averaged: 0,
            overruns: 0,
            transfer_magnitude_db: Vec::new(),
            transfer_phase_degrees: Vec::new(),
            transfer_coherence: Vec::new(),
            transfer_delay_frames: 0,
            transfer_frames: 0,
        };
        // Only built when needed: a transfer function is two more FFTs per
        // frame, and a spectrum-only session should not pay for them.
        let transfer = match config.mode {
            AnalysisMode::Spectrum => None,
            AnalysisMode::Transfer { .. } => Some(TransferFunction::new(TransferConfig {
                sample_rate: config.spectrum.sample_rate,
                size: config.spectrum.size,
                window: config.spectrum.window,
                overlap: config.spectrum.overlap,
                averaging: TransferAveraging::Exponential { alpha: 0.15 },
            })),
        };

        let (publisher, reader) = snapshot_channel(initial);

        let stop = Arc::new(AtomicBool::new(false));
        let published = Arc::new(AtomicU64::new(0));
        // A flag rather than a channel: the worker checks it once per iteration,
        // and a missed reset would be indistinguishable from a late one.
        let reset_average = Arc::new(AtomicBool::new(false));
        let applied_delay = Arc::new(AtomicU32::new(0));
        let estimate_delay = Arc::new(AtomicBool::new(false));
        // A second of delay covers 343 m of air, which is more room than anyone
        // measures, and costs 192 kB at 48 kHz.
        let max_delay = config.spectrum.sample_rate.max(1.0) as usize;
        let finder = match config.mode {
            AnalysisMode::Spectrum => None,
            AnalysisMode::Transfer { .. } => {
                Some(DelayFinder::acoustic(config.spectrum.sample_rate))
            }
        };
        let finder_size = finder.as_ref().map_or(0, DelayFinder::size);

        let worker = {
            let stop = Arc::clone(&stop);
            let published = Arc::clone(&published);
            let reset_average = Arc::clone(&reset_average);
            let applied_delay = Arc::clone(&applied_delay);
            let estimate_delay = Arc::clone(&estimate_delay);
            let channel = config.analysis_channel;
            let channels = config.channels;
            let mode = config.mode;
            thread::Builder::new()
                .name("analyzer-analysis".into())
                .spawn(move || {
                    Worker {
                        source,
                        analyzer,
                        average,
                        publisher,
                        stop,
                        published,
                        reset_average,
                        channel,
                        channels,
                        transfer,
                        mode,
                        interleaved: vec![0.0; DRAIN_FRAMES * channels],
                        mono: vec![0.0; DRAIN_FRAMES],
                        raw_reference: vec![0.0; DRAIN_FRAMES],
                        reference: vec![0.0; DRAIN_FRAMES],
                        delay_line: DelayLine::new(max_delay + 1),
                        applied_delay,
                        estimate_delay,
                        finder,
                        finder_reference: vec![0.0; finder_size],
                        finder_measurement: vec![0.0; finder_size],
                        finder_filled: 0,
                    }
                    .run();
                })
                .expect("spawning the analysis thread")
        };

        (
            sink,
            Self {
                worker: Some(worker),
                stop,
                reader,
                published,
                reset_average,
                applied_delay,
                estimate_delay,
                config,
            },
        )
    }

    /// The newest published frame. Never blocks.
    pub fn latest(&mut self) -> &SpectrumFrame {
        self.reader.read()
    }

    /// Whether a new frame has arrived since the last [`Engine::latest`].
    ///
    /// Lets a UI skip a redraw entirely when nothing changed, which is how the
    /// idle-CPU target gets met — a timer redrawing identical data is the real
    /// battery cost.
    pub fn has_new_frame(&self) -> bool {
        self.reader.has_update()
    }

    /// Total frames published since start, readable without touching the
    /// snapshot. Useful for tests and for a throughput readout.
    pub fn published_count(&self) -> u64 {
        self.published.load(Ordering::Relaxed)
    }

    /// Restart the long-term average without disturbing the live trace.
    ///
    /// Takes effect on the worker's next pass, so a caller should not expect the
    /// very next frame to show a cleared average.
    pub fn reset_average(&self) {
        self.reset_average.store(true, Ordering::Relaxed);
    }

    /// Delay applied to the reference channel, in samples.
    pub fn reference_delay(&self) -> u32 {
        self.applied_delay.load(Ordering::Relaxed)
    }

    /// Set the reference delay by hand, in samples.
    pub fn set_reference_delay(&self, samples: u32) {
        self.applied_delay.store(samples, Ordering::Relaxed);
    }

    /// Ask the worker to measure the delay and apply it.
    ///
    /// Takes effect once enough signal has passed through, so this returns
    /// immediately and the answer appears on a later frame. Requesting during
    /// silence simply waits, which is better than answering zero.
    pub fn estimate_reference_delay(&self) {
        self.estimate_delay.store(true, Ordering::Relaxed);
    }

    /// Whether a delay estimate is still pending.
    pub fn delay_estimate_pending(&self) -> bool {
        self.estimate_delay.load(Ordering::Relaxed)
    }

    /// The configuration in force.
    pub fn config(&self) -> &EngineConfig {
        &self.config
    }

    /// Stop the worker and wait for it. Idempotent; [`Drop`] calls it too.
    pub fn stop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(worker) = self.worker.take() {
            // A worker panic should not poison the caller; it has already been
            // reported by the default hook.
            let _ = worker.join();
        }
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        self.stop();
    }
}

struct Worker {
    source: CaptureSource,
    analyzer: SpectrumAnalyzer,
    average: SpectrumAnalyzer,
    transfer: Option<TransferFunction>,
    mode: AnalysisMode,
    publisher: crate::snapshot::SnapshotPublisher<SpectrumFrame>,
    stop: Arc<AtomicBool>,
    published: Arc<AtomicU64>,
    reset_average: Arc<AtomicBool>,
    channel: usize,
    channels: usize,
    interleaved: Vec<f32>,
    mono: Vec<f32>,
    /// Reference channel as captured, before delay compensation.
    raw_reference: Vec<f32>,
    /// Reference channel after delay compensation. What the transfer sees.
    reference: Vec<f32>,
    delay_line: DelayLine,
    applied_delay: Arc<AtomicU32>,
    estimate_delay: Arc<AtomicBool>,
    finder: Option<DelayFinder>,
    /// Accumulators for the delay finder, which needs a longer view than one
    /// drain pass provides.
    finder_reference: Vec<f32>,
    finder_measurement: Vec<f32>,
    finder_filled: usize,
}

/// A fixed-capacity delay line.
///
/// Preallocated at engine start so applying a delay never touches the
/// allocator, and circular so a delay change costs nothing.
struct DelayLine {
    buffer: Vec<f32>,
    write: usize,
}

impl DelayLine {
    fn new(capacity: usize) -> Self {
        Self {
            buffer: vec![0.0; capacity.max(1)],
            write: 0,
        }
    }

    /// The longest delay this line can hold.
    fn capacity(&self) -> usize {
        self.buffer.len() - 1
    }

    /// Write `input` and read it back `delay` samples later.
    fn process(&mut self, input: &[f32], out: &mut [f32], delay: usize) {
        let delay = delay.min(self.capacity());
        let len = self.buffer.len();
        for (sample, slot) in input.iter().zip(out.iter_mut()) {
            self.buffer[self.write] = *sample;
            self.write = (self.write + 1) % len;
            *slot = self.buffer[(self.write + len - delay - 1) % len];
        }
    }
}

impl Worker {
    /// Re-measure the reference-to-measurement delay, if asked to.
    ///
    /// Cross-correlation needs a window far longer than one drain pass, so
    /// blocks are accumulated until the finder has enough. The request flag is
    /// only cleared once an answer is produced, so asking during silence waits
    /// for signal rather than returning zero.
    fn estimate_delay(&mut self, frames: usize) {
        if !self.estimate_delay.load(Ordering::Relaxed) {
            self.finder_filled = 0;
            return;
        }
        let Some(finder) = self.finder.as_mut() else {
            self.estimate_delay.store(false, Ordering::Relaxed);
            return;
        };

        let take = (finder.size() - self.finder_filled).min(frames);
        let at = self.finder_filled;
        self.finder_reference[at..at + take].copy_from_slice(&self.raw_reference[..take]);
        self.finder_measurement[at..at + take].copy_from_slice(&self.mono[..take]);
        self.finder_filled += take;

        if self.finder_filled < finder.size() {
            return;
        }
        self.finder_filled = 0;

        if let Some(estimate) = finder.find(&self.finder_reference, &self.finder_measurement) {
            // A negative delay means the measurement arrived first, which is
            // physically impossible for an acoustic path and in practice means
            // the two channels are swapped. Clamping to zero is honest: the
            // curve then plainly shows the problem.
            let samples = estimate.samples.max(0.0).round() as u32;
            self.applied_delay.store(
                samples.min(self.delay_line.capacity() as u32),
                Ordering::Relaxed,
            );
            self.estimate_delay.store(false, Ordering::Relaxed);
        }
    }
}

impl Worker {
    fn run(&mut self) {
        let mut sequence = 0_u64;

        while !self.stop.load(Ordering::Relaxed) {
            if self.reset_average.swap(false, Ordering::Relaxed) {
                self.average.reset();
            }

            // Exactly one bounded pass per iteration. See DRAIN_FRAMES: looping
            // until the ring is empty lets a fast producer starve publication.
            let frames = self.source.read_interleaved(&mut self.interleaved);
            if frames == 0 {
                thread::sleep(IDLE_POLL);
                continue;
            }

            // Extract the channel under analysis. Interleaved storage is what
            // keeps a two-channel transfer function sample aligned: both
            // channels are lifted out of the same block, so no amount of
            // scheduling jitter can slide one against the other.
            //
            // In transfer mode the spectrum follows the measurement channel, so
            // the level shown is the level being measured rather than the
            // stimulus.
            let analysed = match self.mode {
                AnalysisMode::Spectrum => self.channel,
                AnalysisMode::Transfer {
                    measurement_channel,
                    ..
                } => measurement_channel,
            };
            for (frame, slot) in self.mono.iter_mut().take(frames).enumerate() {
                *slot = self
                    .interleaved
                    .get(frame * self.channels + analysed)
                    .copied()
                    .unwrap_or_default();
            }

            if let AnalysisMode::Transfer {
                reference_channel, ..
            } = self.mode
            {
                for (index, slot) in self.raw_reference.iter_mut().take(frames).enumerate() {
                    *slot = self
                        .interleaved
                        .get(index * self.channels + reference_channel)
                        .copied()
                        .unwrap_or_default();
                }
                self.estimate_delay(frames);

                // Delay the reference, not the measurement. Sound takes time to
                // reach the microphone, so the reference is the early one, and
                // holding it back is what makes the two describe the same
                // instant. Left uncompensated the phase curve winds through
                // hundreds of turns and coherence collapses well before 1 kHz.
                let delay = self.applied_delay.load(Ordering::Relaxed) as usize;
                self.delay_line.process(
                    &self.raw_reference[..frames],
                    &mut self.reference[..frames],
                    delay,
                );

                if let Some(transfer) = self.transfer.as_mut() {
                    transfer.push(&self.reference[..frames], &self.mono[..frames]);
                }
            }

            let mono = self.mono.get(..frames).unwrap_or(&[]);
            let produced = self.analyzer.push(mono);
            // Both see the same samples, so the two traces describe the same
            // audio and any difference between them is averaging alone.
            self.average.push(mono);

            if produced > 0 {
                sequence += 1;
                let analyzer = &self.analyzer;
                let average = &self.average;
                let transfer = self.transfer.as_ref();
                let applied_delay = &self.applied_delay;
                let overruns = self.source.overruns();
                self.publisher.publish_with(|frame| {
                    // The pending buffer is recycled and holds a value from two
                    // publishes ago, so every field is overwritten.
                    frame.sequence = sequence;
                    frame.bins.resize(analyzer.bins(), 0.0);
                    analyzer.write_db_fs(&mut frame.bins);
                    frame.average_bins.resize(average.bins(), 0.0);
                    average.write_db_fs(&mut frame.average_bins);
                    frame.average_frames = average.frames();
                    frame.bin_spacing_hz = analyzer.bin_spacing_hz();
                    frame.sample_rate = analyzer.bin_spacing_hz() * analyzer.size() as f32;
                    frame.frames_averaged = analyzer.frames();
                    frame.overruns = overruns;

                    match transfer {
                        Some(transfer) => {
                            frame.transfer_magnitude_db.resize(transfer.bins(), 0.0);
                            frame.transfer_phase_degrees.resize(transfer.bins(), 0.0);
                            frame.transfer_coherence.resize(transfer.bins(), 0.0);
                            transfer.write_magnitude_db(&mut frame.transfer_magnitude_db);
                            transfer.write_phase_degrees(&mut frame.transfer_phase_degrees);
                            transfer.write_coherence(&mut frame.transfer_coherence);
                            frame.transfer_delay_frames = applied_delay.load(Ordering::Relaxed);
                            frame.transfer_frames = transfer.frames();
                        }
                        None => {
                            // The pending buffer is recycled, so a mode change
                            // would otherwise leave a stale curve behind.
                            frame.transfer_magnitude_db.clear();
                            frame.transfer_phase_degrees.clear();
                            frame.transfer_coherence.clear();
                            frame.transfer_frames = 0;
                            frame.transfer_delay_frames = 0;
                        }
                    }
                });
                self.published.store(sequence, Ordering::Relaxed);
            }
        }
    }
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;
    use analyzer_dsp::{Averaging, Overlap, WindowKind};
    use std::f32::consts::TAU;
    use std::time::Instant;

    const RATE: f32 = 48_000.0;
    const SIZE: usize = 4096;
    /// Bin 85 exactly, so there is no scalloping loss to reason about.
    const ON_BIN_HZ: f32 = 996.093_75;

    fn config(channels: usize, analysis_channel: usize) -> EngineConfig {
        EngineConfig {
            channels,
            analysis_channel,
            spectrum: SpectrumConfig {
                sample_rate: RATE,
                size: SIZE,
                window: WindowKind::Hann,
                overlap: Overlap::None,
                averaging: Averaging::Infinite,
            },
            average: Averaging::Infinite,
            mode: AnalysisMode::Spectrum,
            ring_capacity_frames: 16_384,
        }
    }

    /// Reproducible broadband noise. A transfer function needs energy in every
    /// bin, which a tone by definition does not provide.
    fn noise(frames: usize, seed: u64) -> Vec<f32> {
        let mut state = seed | 1;
        (0..frames)
            .map(|_| {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                ((state >> 40) as f32 / 8_388_608.0) - 1.0
            })
            .collect()
    }

    /// A continuous two-channel stream: a reference, and the same signal
    /// scaled and delayed.
    ///
    /// Continuous rather than one block replayed, and that is not fussiness. A
    /// repeated block is periodic, and cross-correlation genuinely cannot tell
    /// a delay of `d` from a delay of `d` minus the period - the first version
    /// of this helper repeated a 4096-frame block and the delay finder
    /// correctly reported -3968 for a 128-sample delay.
    struct Echo {
        data: Vec<f32>,
        block: usize,
        cursor: usize,
    }

    impl Echo {
        fn new(blocks: usize, block: usize, gain: f32, delay: usize) -> Self {
            let frames = blocks * block;
            let source = noise(frames + delay, 0x1234_5678);
            let mut data = vec![0.0; frames * 2];
            for frame in 0..frames {
                data[frame * 2] = source[frame + delay];
                data[frame * 2 + 1] = gain * source[frame];
            }
            Self {
                data,
                block,
                cursor: 0,
            }
        }

        /// The next block, wrapping once the stream is exhausted.
        fn next_block(&mut self) -> &[f32] {
            let stride = self.block * 2;
            if self.cursor + stride > self.data.len() {
                self.cursor = 0;
            }
            let at = self.cursor;
            self.cursor += stride;
            &self.data[at..at + stride]
        }
    }

    fn transfer_config() -> EngineConfig {
        let mut config = config(2, 1);
        config.mode = AnalysisMode::Transfer {
            reference_channel: 0,
            measurement_channel: 1,
        };
        config
    }

    /// Pump until the engine has folded in `want` transfer frames.
    fn pump_transfer(
        sink: &mut CaptureSink,
        engine: &mut Engine,
        echo: &mut Echo,
        want: u32,
    ) -> SpectrumFrame {
        for _ in 0..400 {
            sink.write_interleaved(echo.next_block());
            thread::sleep(Duration::from_millis(2));
            if engine.latest().transfer_frames >= want {
                break;
            }
        }
        engine.latest().clone()
    }

    /// A delay line must hold a sample back by exactly the requested count.
    #[test]
    fn the_delay_line_delays_by_the_requested_amount() {
        let mut line = DelayLine::new(64);
        let mut input = vec![0.0_f32; 32];
        input[0] = 1.0;
        let mut out = vec![0.0_f32; 32];

        line.process(&input, &mut out, 5);
        assert_eq!(out[5], 1.0, "impulse should land 5 samples late");
        assert!(out.iter().enumerate().all(|(i, v)| i == 5 || *v == 0.0));
    }

    /// Zero delay must be a pass-through, not an off-by-one.
    #[test]
    fn a_zero_delay_line_passes_samples_straight_through() {
        let mut line = DelayLine::new(64);
        let input: Vec<f32> = (0..16).map(|i| i as f32).collect();
        let mut out = vec![0.0_f32; 16];
        line.process(&input, &mut out, 0);
        assert_eq!(out, input);
    }

    /// The delay must survive across calls, since a block boundary falls
    /// wherever the device chooses and carries no meaning.
    #[test]
    fn the_delay_line_carries_across_blocks() {
        let mut line = DelayLine::new(64);
        let mut out = vec![0.0_f32; 4];
        let mut first = vec![0.0_f32; 4];
        first[1] = 1.0;
        line.process(&first, &mut out, 6);
        assert!(out.iter().all(|v| *v == 0.0), "too early to have arrived");
        line.process(&[0.0; 4], &mut out, 6);
        assert_eq!(out[3], 1.0, "impulse should arrive in the next block");
    }

    /// Asking for more delay than the line holds must clamp, not panic or wrap.
    #[test]
    fn an_oversized_delay_clamps() {
        let mut line = DelayLine::new(8);
        let mut out = vec![0.0_f32; 4];
        line.process(&[1.0, 0.0, 0.0, 0.0], &mut out, 10_000);
        assert!(out.iter().all(|v| v.is_finite()));
    }

    /// A known gain between two channels must come back as that gain.
    #[test]
    fn a_transfer_function_recovers_a_known_gain() {
        let (mut sink, mut engine) = Engine::start(transfer_config());
        let mut echo = Echo::new(16, 4096, 0.5, 0);
        let frame = pump_transfer(&mut sink, &mut engine, &mut echo, 8);
        engine.stop();

        assert!(
            frame.transfer_frames >= 8,
            "no transfer frames were produced"
        );
        assert_eq!(frame.transfer_magnitude_db.len(), SIZE / 2 + 1);

        // Skip the extremes: the lowest bins hold too little noise energy to
        // settle, and the topmost bin is a half-bin special case.
        let band = &frame.transfer_magnitude_db[20..1800];
        let mean = band.iter().sum::<f32>() / band.len() as f32;
        assert!(
            (mean - -6.02).abs() < 0.5,
            "half amplitude should read about -6 dB, got {mean}"
        );

        let coherence = &frame.transfer_coherence[20..1800];
        let worst = coherence.iter().copied().fold(1.0_f32, f32::min);
        assert!(
            worst > 0.99,
            "a noiseless path should be fully coherent, worst was {worst}"
        );
    }

    /// A pure delay must show up as coherent but phase-wound, and compensating
    /// it must flatten the phase back out.
    #[test]
    fn compensating_a_known_delay_flattens_the_phase() {
        let delay = 64;
        let (mut sink, mut engine) = Engine::start(transfer_config());
        let mut echo = Echo::new(16, 4096, 1.0, delay);

        engine.set_reference_delay(delay as u32);
        let frame = pump_transfer(&mut sink, &mut engine, &mut echo, 8);
        engine.stop();

        assert!(frame.transfer_frames >= 8);
        assert_eq!(frame.transfer_delay_frames, delay as u32);

        let phase = &frame.transfer_phase_degrees[20..1800];
        let worst = phase
            .iter()
            .copied()
            .fold(0.0_f32, |acc, p| acc.max(p.abs()));
        assert!(
            worst < 5.0,
            "compensated phase should be flat, worst was {worst} deg"
        );
    }

    /// The finder must recover a delay nobody told it about.
    #[test]
    fn the_engine_can_measure_the_delay_itself() {
        let delay = 128;
        let (mut sink, mut engine) = Engine::start(transfer_config());
        let mut echo = Echo::new(16, 4096, 1.0, delay);

        engine.estimate_reference_delay();
        pump_transfer(&mut sink, &mut engine, &mut echo, 8);
        let found = engine.reference_delay();
        engine.stop();

        assert!(
            found.abs_diff(delay as u32) <= 1,
            "expected about {delay} samples of delay, found {found}"
        );
    }

    /// Spectrum mode must not publish a stale or empty transfer curve that a UI
    /// could mistake for a real one.
    #[test]
    fn spectrum_mode_publishes_no_transfer_curves() {
        let (mut sink, mut engine) = Engine::start(config(1, 0));
        let data = tone(4096, 1, 0, 0.5);
        for _ in 0..40 {
            sink.write_interleaved(&data);
            thread::sleep(Duration::from_millis(2));
            if engine.latest().frames_averaged > 0 {
                break;
            }
        }
        let frame = engine.latest().clone();
        engine.stop();

        assert!(frame.frames_averaged > 0, "the spectrum should still run");
        assert_eq!(frame.transfer_frames, 0);
        assert!(frame.transfer_magnitude_db.is_empty());
        assert!(frame.transfer_coherence.is_empty());
    }

    /// In transfer mode the spectrum follows the measurement channel, so the
    /// level on screen is the level being measured.
    #[test]
    fn the_spectrum_follows_the_measurement_channel_in_transfer_mode() {
        let (mut sink, mut engine) = Engine::start(transfer_config());
        // Loud reference on channel 0, quiet measurement on channel 1.
        let mut echo = Echo::new(16, 4096, 0.01, 0);
        let frame = pump_transfer(&mut sink, &mut engine, &mut echo, 4);
        engine.stop();

        let peak = frame.bins.iter().copied().fold(f32::NEG_INFINITY, f32::max);
        assert!(
            peak < -40.0,
            "the spectrum showed the reference, not the measurement: peak {peak} dB"
        );
    }

    /// Interleaved frames with a tone on `tone_channel` and silence elsewhere.
    fn tone(frames: usize, channels: usize, tone_channel: usize, amplitude: f32) -> Vec<f32> {
        let mut out = vec![0.0; frames * channels];
        for frame in 0..frames {
            let phase = TAU * ON_BIN_HZ * frame as f32 / RATE;
            out[frame * channels + tone_channel] = amplitude * phase.sin();
        }
        out
    }

    /// Feed blocks until the engine has published, or give up.
    fn feed_until_published(
        sink: &mut CaptureSink,
        engine: &Engine,
        samples: &[f32],
        block: usize,
    ) {
        let deadline = Instant::now() + Duration::from_secs(5);
        let mut offset = 0;
        while engine.published_count() == 0 {
            if Instant::now() > deadline {
                panic!("engine never published");
            }
            if offset >= samples.len() {
                offset = 0;
            }
            let end = (offset + block).min(samples.len());
            // Respect backpressure rather than flooding. A real audio callback
            // produces at wall-clock rate into a ring sized to absorb it; a test
            // that hammers as fast as it can would overrun by construction and
            // say nothing useful about the engine.
            let wanted = (end - offset) / sink.channels();
            if sink.frames_free() >= wanted && sink.write_interleaved(&samples[offset..end]) {
                offset = end;
            } else {
                thread::sleep(Duration::from_millis(1));
            }
        }
    }

    #[test]
    fn publishes_a_frame_with_the_tone_in_the_right_bin() {
        let (mut sink, mut engine) = Engine::start(config(1, 0));
        let samples = tone(SIZE * 2, 1, 0, 0.5);
        feed_until_published(&mut sink, &engine, &samples, 128);

        let frame = engine.latest().clone();
        assert!(frame.sequence > 0);
        assert_eq!(frame.bins.len(), SIZE / 2 + 1);
        assert_eq!(frame.overruns, 0);

        let peak = frame
            .bins
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.total_cmp(b.1))
            .map(|(bin, _)| bin)
            .unwrap();
        assert_eq!(peak, 85, "tone should land in bin 85");
        assert!(
            (frame.bins[85] + 6.0206).abs() < 0.05,
            "expected -6.02 dBFS, got {}",
            frame.bins[85]
        );
        assert!((frame.bin_spacing_hz - RATE / SIZE as f32).abs() < 1e-3);
        assert!((frame.sample_rate - RATE).abs() < 0.5);
    }

    #[test]
    fn analyses_the_selected_channel_only() {
        // Tone on channel 1, silence on channel 0.
        let (mut sink, mut engine) = Engine::start(config(2, 1));
        let samples = tone(SIZE * 2, 2, 1, 0.5);
        feed_until_published(&mut sink, &engine, &samples, 128);
        assert!((engine.latest().bins[85] + 6.0206).abs() < 0.05);
        drop(engine);

        let (mut sink, mut engine) = Engine::start(config(2, 0));
        let samples = tone(SIZE * 2, 2, 1, 0.5);
        feed_until_published(&mut sink, &engine, &samples, 128);
        assert!(
            engine.latest().bins[85] < -80.0,
            "silent channel picked up the tone: {}",
            engine.latest().bins[85]
        );
    }

    /// Both traces must describe the same audio, differing only in how they
    /// average. A steady tone settles to the same answer either way.
    #[test]
    fn the_average_trace_tracks_the_same_signal() {
        let (mut sink, mut engine) = Engine::start(config(1, 0));
        let samples = tone(SIZE * 8, 1, 0, 0.5);
        feed_until_published(&mut sink, &engine, &samples, 256);

        // Give the average a few frames to settle.
        let deadline = Instant::now() + Duration::from_secs(5);
        while engine.published_count() < 4 && Instant::now() < deadline {
            let _ = sink.write_interleaved(&samples[..256.min(samples.len())]);
            thread::sleep(Duration::from_millis(1));
        }

        let frame = engine.latest().clone();
        assert_eq!(
            frame.average_bins.len(),
            frame.bins.len(),
            "both traces span the same bins"
        );
        assert!(frame.average_frames > 0, "the average should have run");
        assert!(
            (frame.average_bins[85] - frame.bins[85]).abs() < 1.0,
            "on a steady tone they should agree: live {} vs average {}",
            frame.bins[85],
            frame.average_bins[85]
        );
    }

    /// Resetting the average must not disturb the live trace, which is the whole
    /// point of running two analyzers.
    #[test]
    fn resetting_the_average_leaves_the_live_trace_alone() {
        let (mut sink, mut engine) = Engine::start(config(1, 0));
        let samples = tone(SIZE * 4, 1, 0, 0.5);
        feed_until_published(&mut sink, &engine, &samples, 256);

        let before = engine.latest().clone();
        assert!(before.average_frames > 0);

        engine.reset_average();

        // Feed enough for the worker to see the flag and publish again, as a
        // continuous stream rather than the same block over and over.
        //
        // Re-sending `samples[..256]` would not be the tone: 256 samples is
        // 5.3125 cycles at this frequency, so repeating it restarts the phase
        // every block and the discontinuity smears energy off bin 85. The live
        // trace would then genuinely move, and whether this test passed would
        // depend on how many such frames landed before `latest()` was read.
        // The whole buffer is exactly 340 cycles, so wrapping it is seamless.
        let deadline = Instant::now() + Duration::from_secs(5);
        let target = engine.published_count() + 2;
        let mut offset = 0;
        while engine.published_count() < target && Instant::now() < deadline {
            if offset >= samples.len() {
                offset = 0;
            }
            let end = (offset + 256).min(samples.len());
            if sink.write_interleaved(&samples[offset..end]) {
                offset = end;
            }
            thread::sleep(Duration::from_millis(1));
        }

        let after = engine.latest().clone();
        assert!(
            after.average_frames < before.average_frames + 2,
            "the average should have restarted: {} then {}",
            before.average_frames,
            after.average_frames
        );
        assert!(
            (after.bins[85] - before.bins[85]).abs() < 1.0,
            "the live trace must be undisturbed"
        );
    }

    #[test]
    fn has_new_frame_tracks_publication() {
        let (mut sink, mut engine) = Engine::start(config(1, 0));
        let samples = tone(SIZE * 2, 1, 0, 0.5);
        feed_until_published(&mut sink, &engine, &samples, 256);

        assert!(engine.has_new_frame());
        let _ = engine.latest();
        assert!(!engine.has_new_frame(), "consumed frame should clear");
    }

    #[test]
    fn reports_overruns_from_a_starved_analysis_thread() {
        let mut config = config(1, 0);
        // Tiny ring, so filling it faster than the worker drains is easy.
        config.ring_capacity_frames = 256;
        let (mut sink, engine) = Engine::start(config);

        let block = vec![0.0_f32; 256];
        let mut refused = 0;
        for _ in 0..2_000 {
            if !sink.write_interleaved(&block) {
                refused += 1;
            }
        }
        assert!(refused > 0, "should have overrun a 256-frame ring");
        assert!(sink.overruns() >= refused);
        drop(engine);
    }

    /// Regression test. An earlier version drained the ring until it was empty
    /// before publishing, which looks natural and is wrong: a producer that
    /// keeps the ring topped up means the drain loop never exits and nothing is
    /// ever published. This floods as hard as it can and still expects progress.
    #[test]
    fn a_fast_producer_cannot_starve_publication() {
        let mut config = config(1, 0);
        config.ring_capacity_frames = 32_768;
        let (mut sink, engine) = Engine::start(config);

        let block = vec![0.25_f32; 1024];
        let deadline = Instant::now() + Duration::from_secs(5);
        while engine.published_count() == 0 {
            assert!(
                Instant::now() < deadline,
                "publication starved by a producer that keeps the ring full"
            );
            // Deliberately unpaced, refusals ignored: the harshest case.
            let _ = sink.write_interleaved(&block);
        }
    }

    #[test]
    fn stop_is_idempotent_and_drop_joins() {
        let (_sink, mut engine) = Engine::start(config(1, 0));
        engine.stop();
        engine.stop();
        // Drop must not hang or double-join.
    }

    #[test]
    fn idle_engine_publishes_nothing() {
        let (_sink, engine) = Engine::start(config(1, 0));
        thread::sleep(Duration::from_millis(50));
        assert_eq!(
            engine.published_count(),
            0,
            "an engine with no input must not publish"
        );
    }

    #[test]
    #[should_panic(expected = "analysis channel 2 does not exist")]
    fn rejects_an_analysis_channel_that_does_not_exist() {
        let _ = Engine::start(config(2, 2));
    }
}
