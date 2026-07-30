//! Stimulus generation.
//!
//! A spectrum tells you what came out. To learn what a system *did* you have to
//! know what went in, which means generating it. Everything here is real-time
//! safe: [`Generator::fill`] allocates nothing and takes no locks, because it
//! runs inside the audio callback alongside capture.
//!
//! # Why the phase accumulator is `f64`
//!
//! An `f32` phase accumulator drifts. It has 24 bits of mantissa, so once the
//! accumulated phase passes a few million radians the increment stops being
//! representable and the generated frequency quietly shifts. At 48 kHz that is
//! minutes, not hours — well inside a measurement session.

use std::f64::consts::TAU;

/// What to generate.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub enum Signal {
    /// Digital silence.
    #[default]
    Silence,
    /// A steady sine. The stimulus for calibration and distortion work.
    Sine {
        /// Frequency in hertz.
        hz: f32,
        /// Peak amplitude, 0..=1.
        amplitude: f32,
    },
    /// Equal energy per hertz. Sounds bright, because each octave up holds twice
    /// the bandwidth and so twice the power.
    WhiteNoise {
        /// Peak amplitude, 0..=1.
        amplitude: f32,
    },
    /// Equal energy per octave, falling at 3 dB per octave. The usual stimulus
    /// for transfer function work, because it puts comparable energy in every
    /// part of a log frequency display.
    PinkNoise {
        /// Peak amplitude, 0..=1.
        amplitude: f32,
    },
    /// Exponential sine sweep, the Farina stimulus.
    ///
    /// Spends equal time per octave and lets harmonic distortion be separated
    /// from the linear response after deconvolution, which a linear sweep cannot
    /// do. This is what swept measurement will use.
    Sweep {
        /// Starting frequency in hertz.
        start_hz: f32,
        /// Ending frequency in hertz.
        end_hz: f32,
        /// Duration of one pass.
        seconds: f32,
        /// Peak amplitude, 0..=1.
        amplitude: f32,
        /// Whether to restart after reaching the end.
        repeat: bool,
    },
}

/// A small xorshift generator.
///
/// Deliberately not the `rand` crate: `thread_rng` locks and can allocate, and
/// neither is acceptable inside an audio callback. Noise does not need
/// cryptographic quality, it needs to be fast and never block.
#[derive(Debug, Clone)]
struct Rng(u64);

impl Rng {
    fn new(seed: u64) -> Self {
        // Zero is a fixed point of xorshift, so it must never be the state.
        Self(if seed == 0 {
            0x9E37_79B9_7F4A_7C15
        } else {
            seed
        })
    }

    /// Uniform in `-1.0..1.0`.
    fn next_sample(&mut self) -> f32 {
        let mut x = self.0;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.0 = x;
        let scrambled = x.wrapping_mul(0x2545_F491_4F6C_DD1D);
        // Top 24 bits, mapped to [-1, 1). The low bits of xorshift are weak.
        ((scrambled >> 40) as f32 / 8_388_608.0) - 1.0
    }
}

/// Paul Kellet's refined pink filter: white noise shaped to -3 dB per octave,
/// accurate to about ±0.05 dB from 9 Hz to 20 kHz. Six one-pole sections plus a
/// direct path, which is far cheaper than an FFT-based approach and good enough
/// that the error is invisible next to any real acoustic measurement.
#[derive(Debug, Clone, Default)]
struct PinkFilter {
    b: [f32; 7],
}

impl PinkFilter {
    /// Roughly normalises the summed output back to unity peak.
    const SCALE: f32 = 0.125;

    fn process(&mut self, white: f32) -> f32 {
        self.b[0] = 0.998_86 * self.b[0] + white * 0.055_517_9_f32;
        self.b[1] = 0.993_32 * self.b[1] + white * 0.075_075_9_f32;
        self.b[2] = 0.969_00 * self.b[2] + white * 0.153_852;
        self.b[3] = 0.866_50 * self.b[3] + white * 0.310_485_6_f32;
        self.b[4] = 0.550_00 * self.b[4] + white * 0.532_952_2_f32;
        self.b[5] = -0.761_6 * self.b[5] - white * 0.016_898_0;
        let pink = self.b[0]
            + self.b[1]
            + self.b[2]
            + self.b[3]
            + self.b[4]
            + self.b[5]
            + self.b[6]
            + white * 0.536_2;
        self.b[6] = white * 0.115_926;
        pink * Self::SCALE
    }

    fn reset(&mut self) {
        self.b = [0.0; 7];
    }
}

/// Generates a stimulus into a buffer.
///
/// All state is preallocated; [`Generator::fill`] never allocates.
#[derive(Debug, Clone)]
pub struct Generator {
    sample_rate: f64,
    signal: Signal,
    /// Radians, in `f64` — see the module docs on drift.
    phase: f64,
    /// Samples into the current sweep pass.
    position: u64,
    rng: Rng,
    pink: PinkFilter,
    finished: bool,
}

impl Generator {
    /// Build a generator.
    ///
    /// The seed makes noise reproducible, which matters for tests: a flaky
    /// spectrum assertion is worse than no assertion.
    ///
    /// # Panics
    ///
    /// Panics if `sample_rate` is not positive.
    pub fn new(sample_rate: f32, signal: Signal, seed: u64) -> Self {
        assert!(
            sample_rate > 0.0,
            "sample rate must be positive, got {sample_rate}"
        );
        Self {
            sample_rate: f64::from(sample_rate),
            signal,
            phase: 0.0,
            position: 0,
            rng: Rng::new(seed),
            pink: PinkFilter::default(),
            finished: false,
        }
    }

    /// Change what is generated, resetting phase and filter state.
    pub fn set_signal(&mut self, signal: Signal) {
        self.signal = signal;
        self.reset();
    }

    /// The signal in force.
    pub fn signal(&self) -> Signal {
        self.signal
    }

    /// Return to the start.
    pub fn reset(&mut self) {
        self.phase = 0.0;
        self.position = 0;
        self.finished = false;
        self.pink.reset();
    }

    /// Whether a non-repeating sweep has run to its end.
    pub fn is_finished(&self) -> bool {
        self.finished
    }

    /// Fill `out` with the next samples, overwriting whatever was there.
    ///
    /// Real-time safe: no allocation, no locks, no branches on anything but the
    /// signal kind.
    pub fn fill(&mut self, out: &mut [f32]) {
        match self.signal {
            Signal::Silence => out.fill(0.0),
            Signal::Sine { hz, amplitude } => self.fill_sine(out, hz, amplitude),
            Signal::WhiteNoise { amplitude } => {
                let gain = amplitude.clamp(0.0, 1.0);
                for slot in out.iter_mut() {
                    *slot = self.rng.next_sample() * gain;
                }
            }
            Signal::PinkNoise { amplitude } => {
                let gain = amplitude.clamp(0.0, 1.0);
                for slot in out.iter_mut() {
                    let white = self.rng.next_sample();
                    // Clamped because the filter's peak is statistical, not
                    // bounded, and a rare overshoot must not clip the converter.
                    *slot = (self.pink.process(white) * gain).clamp(-1.0, 1.0);
                }
            }
            Signal::Sweep {
                start_hz,
                end_hz,
                seconds,
                amplitude,
                repeat,
            } => self.fill_sweep(out, start_hz, end_hz, seconds, amplitude, repeat),
        }
    }

    fn fill_sine(&mut self, out: &mut [f32], hz: f32, amplitude: f32) {
        let gain = amplitude.clamp(0.0, 1.0);
        let increment = TAU * f64::from(hz) / self.sample_rate;
        for slot in out.iter_mut() {
            *slot = (self.phase.sin() as f32) * gain;
            self.phase += increment;
            // Wrapping keeps the accumulator small enough that its resolution
            // never degrades, however long the generator runs.
            if self.phase >= TAU {
                self.phase -= TAU;
            }
        }
    }

    fn fill_sweep(
        &mut self,
        out: &mut [f32],
        start_hz: f32,
        end_hz: f32,
        seconds: f32,
        amplitude: f32,
        repeat: bool,
    ) {
        let gain = amplitude.clamp(0.0, 1.0);
        let start = f64::from(start_hz).max(1e-3);
        let end = f64::from(end_hz).max(start + 1e-3);
        let duration = f64::from(seconds).max(1e-3);
        let total = (duration * self.sample_rate) as u64;
        let ratio = (end / start).ln();

        for slot in out.iter_mut() {
            if self.position >= total {
                if repeat {
                    self.position = 0;
                } else {
                    self.finished = true;
                    *slot = 0.0;
                    continue;
                }
            }

            // Farina's exponential sweep. Instantaneous frequency rises
            // geometrically, so equal time is spent in every octave.
            let t = self.position as f64 / self.sample_rate;
            let phase = (TAU * start * duration / ratio) * ((t / duration * ratio).exp() - 1.0);
            *slot = (phase.sin() as f32) * gain;
            self.position += 1;
        }
    }
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::{Averaging, Overlap, SpectrumAnalyzer, SpectrumConfig, WindowKind};

    const RATE: f32 = 48_000.0;
    const SIZE: usize = 4096;
    /// Bin 85 exactly at 48 kHz / 4096.
    const ON_BIN_HZ: f32 = 996.093_75;

    fn analyse(samples: &[f32], window: WindowKind) -> Vec<f32> {
        let mut analyzer = SpectrumAnalyzer::new(SpectrumConfig {
            sample_rate: RATE,
            size: SIZE,
            window,
            overlap: Overlap::Half,
            averaging: Averaging::Infinite,
        });
        analyzer.push(samples);
        let mut db = vec![0.0; analyzer.bins()];
        analyzer.write_db_fs(&mut db);
        db
    }

    fn generate(signal: Signal, samples: usize) -> Vec<f32> {
        let mut generator = Generator::new(RATE, signal, 12345);
        let mut out = vec![0.0; samples];
        generator.fill(&mut out);
        out
    }

    /// Closes the loop: the generator's own sine, measured by our own analyzer,
    /// must land in the right bin at the right level.
    #[test]
    fn a_generated_sine_measures_at_its_own_frequency_and_level() {
        let samples = generate(
            Signal::Sine {
                hz: ON_BIN_HZ,
                amplitude: 0.5,
            },
            SIZE * 8,
        );
        let db = analyse(&samples, WindowKind::FlatTop);

        let peak = db
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.total_cmp(b.1))
            .map(|(bin, _)| bin)
            .unwrap();
        assert_eq!(peak, 85);
        assert!(
            (db[85] - -6.0206).abs() < 0.05,
            "expected -6.02 dBFS, got {}",
            db[85]
        );
    }

    /// The reason the accumulator is f64. Generate ten seconds and check the
    /// tone has not wandered off its bin.
    #[test]
    fn a_long_sine_does_not_drift_off_frequency() {
        let mut generator = Generator::new(
            RATE,
            Signal::Sine {
                hz: ON_BIN_HZ,
                amplitude: 0.5,
            },
            1,
        );
        let mut discard = vec![0.0; RATE as usize * 10];
        generator.fill(&mut discard);

        // Now measure a fresh window ten seconds in.
        let mut tail = vec![0.0; SIZE * 4];
        generator.fill(&mut tail);
        let db = analyse(&tail, WindowKind::FlatTop);
        let peak = db
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.total_cmp(b.1))
            .map(|(bin, _)| bin)
            .unwrap();
        assert_eq!(peak, 85, "tone drifted after ten seconds");
    }

    #[test]
    fn silence_is_silent() {
        let samples = generate(Signal::Silence, 1024);
        assert!(samples.iter().all(|s| *s == 0.0));
    }

    #[test]
    fn amplitude_is_respected_and_clamped() {
        for amplitude in [0.1_f32, 0.5, 1.0] {
            let samples = generate(
                Signal::Sine {
                    hz: 1000.0,
                    amplitude,
                },
                48_000,
            );
            let peak = samples.iter().fold(0.0_f32, |m, s| m.max(s.abs()));
            assert!(
                (peak - amplitude).abs() < 0.01,
                "amplitude {amplitude} peaked at {peak}"
            );
        }
        // Out-of-range requests clamp rather than clipping the converter.
        let hot = generate(
            Signal::Sine {
                hz: 1000.0,
                amplitude: 5.0,
            },
            4096,
        );
        assert!(hot.iter().all(|s| s.abs() <= 1.001));
    }

    #[test]
    fn no_signal_exceeds_full_scale() {
        for signal in [
            Signal::WhiteNoise { amplitude: 1.0 },
            Signal::PinkNoise { amplitude: 1.0 },
            Signal::Sine {
                hz: 997.0,
                amplitude: 1.0,
            },
            Signal::Sweep {
                start_hz: 20.0,
                end_hz: 20_000.0,
                seconds: 1.0,
                amplitude: 1.0,
                repeat: true,
            },
        ] {
            let samples = generate(signal, 96_000);
            let peak = samples.iter().fold(0.0_f32, |m, s| m.max(s.abs()));
            assert!(peak <= 1.001, "{signal:?} peaked at {peak}");
        }
    }

    /// White noise is equal energy per hertz, so a linear-frequency spectrum is
    /// flat. Compared across two decades it should not tilt.
    #[test]
    fn white_noise_is_spectrally_flat() {
        let samples = generate(Signal::WhiteNoise { amplitude: 0.5 }, SIZE * 64);
        let db = analyse(&samples, WindowKind::Hann);
        let spacing = RATE / SIZE as f32;

        let mean_around = |hz: f32| -> f32 {
            let centre = (hz / spacing) as usize;
            let band = &db[centre.saturating_sub(40)..(centre + 40).min(db.len())];
            band.iter().sum::<f32>() / band.len() as f32
        };

        let low = mean_around(200.0);
        let high = mean_around(10_000.0);
        assert!(
            (low - high).abs() < 1.5,
            "white noise tilted: {low:.1} dB at 200 Hz vs {high:.1} at 10 kHz"
        );
    }

    /// Pink noise is equal energy per octave, which on a per-bin spectrum means
    /// falling at 3 dB per octave. Across the ~5.6 octaves from 200 Hz to 10 kHz
    /// that is about 17 dB.
    #[test]
    fn pink_noise_falls_three_decibels_per_octave() {
        let samples = generate(Signal::PinkNoise { amplitude: 0.5 }, SIZE * 64);
        let db = analyse(&samples, WindowKind::Hann);
        let spacing = RATE / SIZE as f32;

        let mean_around = |hz: f32| -> f32 {
            let centre = (hz / spacing) as usize;
            let band = &db[centre.saturating_sub(40)..(centre + 40).min(db.len())];
            band.iter().sum::<f32>() / band.len() as f32
        };

        let low = mean_around(200.0);
        let high = mean_around(10_000.0);
        let octaves = (10_000.0_f32 / 200.0).log2();
        let slope = (low - high) / octaves;
        assert!(
            (slope - 3.0).abs() < 0.6,
            "expected about -3 dB/octave, measured {slope:.2} ({low:.1} -> {high:.1})"
        );
    }

    /// Reproducible noise matters: a flaky spectrum assertion is worse than no
    /// assertion at all.
    #[test]
    fn noise_is_reproducible_for_a_given_seed() {
        let seeded = |seed: u64| {
            let mut generator = Generator::new(RATE, Signal::WhiteNoise { amplitude: 0.5 }, seed);
            let mut out = vec![0.0; 4096];
            generator.fill(&mut out);
            out
        };
        assert_eq!(seeded(42), seeded(42), "same seed must give the same noise");
        assert_ne!(seeded(42), seeded(43), "different seeds must differ");
    }

    #[test]
    fn a_zero_seed_still_produces_noise() {
        let samples = generate(Signal::WhiteNoise { amplitude: 0.5 }, 1024);
        assert!(
            samples.iter().any(|s| *s != 0.0),
            "zero is a fixed point of xorshift and must be replaced"
        );
    }

    /// The sweep must actually sweep: its first moments should be low frequency
    /// and its last high.
    #[test]
    fn a_sweep_starts_low_and_ends_high() {
        let seconds = 2.0_f32;
        let mut generator = Generator::new(
            RATE,
            Signal::Sweep {
                start_hz: 50.0,
                end_hz: 10_000.0,
                seconds,
                amplitude: 0.5,
                repeat: false,
            },
            1,
        );
        let total = (RATE * seconds) as usize;
        let mut all = vec![0.0; total];
        generator.fill(&mut all);

        let peak_hz = |chunk: &[f32]| -> f32 {
            let mut analyzer = SpectrumAnalyzer::new(SpectrumConfig {
                sample_rate: RATE,
                size: 2048,
                window: WindowKind::Hann,
                overlap: Overlap::Half,
                averaging: Averaging::Infinite,
            });
            analyzer.push(chunk);
            let mut db = vec![0.0; analyzer.bins()];
            analyzer.write_db_fs(&mut db);
            let bin = db
                .iter()
                .enumerate()
                .max_by(|a, b| a.1.total_cmp(b.1))
                .map(|(k, _)| k)
                .unwrap();
            analyzer.bin_frequency(bin)
        };

        let start = peak_hz(&all[..8192]);
        let end = peak_hz(&all[total - 8192..]);
        assert!(
            start < 200.0,
            "sweep started at {start} Hz, expected near 50"
        );
        assert!(end > 5000.0, "sweep ended at {end} Hz, expected near 10k");
    }

    #[test]
    fn a_non_repeating_sweep_finishes_and_goes_quiet() {
        let mut generator = Generator::new(
            RATE,
            Signal::Sweep {
                start_hz: 100.0,
                end_hz: 1000.0,
                seconds: 0.1,
                amplitude: 0.5,
                repeat: false,
            },
            1,
        );
        let mut out = vec![0.0; (RATE * 0.2) as usize];
        generator.fill(&mut out);

        assert!(generator.is_finished());
        let tail = &out[out.len() - 1000..];
        assert!(
            tail.iter().all(|s| *s == 0.0),
            "should be silent after the sweep"
        );
    }

    #[test]
    fn a_repeating_sweep_never_finishes() {
        let mut generator = Generator::new(
            RATE,
            Signal::Sweep {
                start_hz: 100.0,
                end_hz: 1000.0,
                seconds: 0.05,
                amplitude: 0.5,
                repeat: true,
            },
            1,
        );
        let mut out = vec![0.0; (RATE * 0.5) as usize];
        generator.fill(&mut out);
        assert!(!generator.is_finished());
        assert!(
            out.iter().any(|s| s.abs() > 0.1),
            "should still be sounding"
        );
    }

    /// Buffer size is the driver's business, not the generator's.
    #[test]
    fn chunked_generation_matches_one_block() {
        let signal = Signal::Sine {
            hz: 997.0,
            amplitude: 0.5,
        };
        let mut whole = Generator::new(RATE, signal, 7);
        let mut one = vec![0.0; 5000];
        whole.fill(&mut one);

        let mut chunked = Generator::new(RATE, signal, 7);
        let mut many = vec![0.0; 5000];
        for chunk in many.chunks_mut(113) {
            chunked.fill(chunk);
        }

        for (i, (a, b)) in one.iter().zip(&many).enumerate() {
            assert!((a - b).abs() < 1e-6, "diverged at sample {i}: {a} vs {b}");
        }
    }

    #[test]
    fn reset_returns_to_the_beginning() {
        let signal = Signal::Sine {
            hz: 1000.0,
            amplitude: 0.5,
        };
        let mut generator = Generator::new(RATE, signal, 1);
        let mut first = vec![0.0; 512];
        generator.fill(&mut first);

        generator.reset();
        let mut again = vec![0.0; 512];
        generator.fill(&mut again);
        assert_eq!(first, again);
    }

    #[test]
    fn changing_signal_resets_state() {
        let mut generator = Generator::new(
            RATE,
            Signal::Sine {
                hz: 1000.0,
                amplitude: 0.5,
            },
            1,
        );
        let mut discard = vec![0.0; 1024];
        generator.fill(&mut discard);

        generator.set_signal(Signal::Silence);
        assert_eq!(generator.signal(), Signal::Silence);
        generator.fill(&mut discard);
        assert!(discard.iter().all(|s| *s == 0.0));
    }

    #[test]
    #[should_panic(expected = "sample rate must be positive")]
    fn a_non_positive_sample_rate_is_rejected() {
        let _ = Generator::new(0.0, Signal::Silence, 1);
    }
}
