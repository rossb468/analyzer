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
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use analyzer_dsp::{SpectrumAnalyzer, SpectrumConfig};

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
    pub bins: Vec<f32>,
    /// Hertz between adjacent bins.
    pub bin_spacing_hz: f32,
    /// Rate the analysis ran at.
    pub sample_rate: f32,
    /// Frames folded into the current average.
    pub frames_averaged: u32,
    /// Blocks the audio callback had to drop. Non-zero invalidates the
    /// measurement and the UI is expected to say so.
    pub overruns: u64,
}

impl SpectrumFrame {
    /// Centre frequency of bin `index`.
    pub fn bin_frequency(&self, index: usize) -> f32 {
        index as f32 * self.bin_spacing_hz
    }
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

        let (sink, source) = capture_ring(config.channels, config.ring_capacity_frames);

        let analyzer = SpectrumAnalyzer::new(config.spectrum);
        let initial = SpectrumFrame {
            sequence: 0,
            bins: vec![analyzer_dsp::spectrum::DB_FLOOR; analyzer.bins()],
            bin_spacing_hz: analyzer.bin_spacing_hz(),
            sample_rate: config.spectrum.sample_rate,
            frames_averaged: 0,
            overruns: 0,
        };
        let (publisher, reader) = snapshot_channel(initial);

        let stop = Arc::new(AtomicBool::new(false));
        let published = Arc::new(AtomicU64::new(0));

        let worker = {
            let stop = Arc::clone(&stop);
            let published = Arc::clone(&published);
            let channel = config.analysis_channel;
            let channels = config.channels;
            thread::Builder::new()
                .name("analyzer-analysis".into())
                .spawn(move || {
                    Worker {
                        source,
                        analyzer,
                        publisher,
                        stop,
                        published,
                        channel,
                        channels,
                        interleaved: vec![0.0; DRAIN_FRAMES * channels],
                        mono: vec![0.0; DRAIN_FRAMES],
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
    publisher: crate::snapshot::SnapshotPublisher<SpectrumFrame>,
    stop: Arc<AtomicBool>,
    published: Arc<AtomicU64>,
    channel: usize,
    channels: usize,
    interleaved: Vec<f32>,
    mono: Vec<f32>,
}

impl Worker {
    fn run(&mut self) {
        let mut sequence = 0_u64;

        while !self.stop.load(Ordering::Relaxed) {
            // Exactly one bounded pass per iteration. See DRAIN_FRAMES: looping
            // until the ring is empty lets a fast producer starve publication.
            let frames = self.source.read_interleaved(&mut self.interleaved);
            if frames == 0 {
                thread::sleep(IDLE_POLL);
                continue;
            }

            // Extract the channel under analysis. Interleaved storage is why a
            // two-channel transfer function will be able to stay sample aligned
            // here later.
            for (frame, slot) in self.mono.iter_mut().take(frames).enumerate() {
                *slot = self
                    .interleaved
                    .get(frame * self.channels + self.channel)
                    .copied()
                    .unwrap_or_default();
            }
            let produced = self.analyzer.push(self.mono.get(..frames).unwrap_or(&[]));

            if produced > 0 {
                sequence += 1;
                let analyzer = &self.analyzer;
                let overruns = self.source.overruns();
                self.publisher.publish_with(|frame| {
                    // The pending buffer is recycled and holds a value from two
                    // publishes ago, so every field is overwritten.
                    frame.sequence = sequence;
                    frame.bins.resize(analyzer.bins(), 0.0);
                    analyzer.write_db_fs(&mut frame.bins);
                    frame.bin_spacing_hz = analyzer.bin_spacing_hz();
                    frame.sample_rate = analyzer.bin_spacing_hz() * analyzer.size() as f32;
                    frame.frames_averaged = analyzer.frames();
                    frame.overruns = overruns;
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
            ring_capacity_frames: 16_384,
        }
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
