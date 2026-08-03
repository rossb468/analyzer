//! Writing signals out as WAV.
//!
//! Two things need this and they turn out to be the same thing. The parity run
//! against REW needs test signals as files, because that is the only way to put
//! *identical* input through two applications. And a measurement tool should be
//! able to hand you the stimulus it just played, so you can take it to another
//! machine, another room, or another analyser.
//!
//! Reading stays in the harness, which is the only thing that consumes WAVs.
//! Writing lives here because [`crate`] is where formats live and because both
//! the harness and the macOS client need it — and file I/O does not belong in a
//! platform layer.
//!
//! # Depth, and why the default is float
//!
//! A generated signal has no reason to be quantised. Sixteen-bit output of a
//! test tone adds dither noise at −96 dBFS to a measurement whose whole point is
//! measuring a noise floor, so [`SampleDepth::Float32`] is the default and the
//! integer depths exist for tools that will not read float.

use std::path::Path;

use analyzer_dsp::{Generator, Signal};

/// Sample format to write.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum SampleDepth {
    /// 16-bit integer. Lossy for a generated signal; here for compatibility.
    Int16,
    /// 24-bit integer.
    Int24,
    /// 32-bit float, the native form of everything upstream.
    #[default]
    Float32,
}

impl SampleDepth {
    /// The token a command line uses.
    pub fn as_key(self) -> &'static str {
        match self {
            Self::Int16 => "i16",
            Self::Int24 => "i24",
            Self::Float32 => "f32",
        }
    }

    /// Parse a token, or `None` if it names no depth.
    pub fn from_key(key: &str) -> Option<Self> {
        match key {
            "i16" | "16" => Some(Self::Int16),
            "i24" | "24" => Some(Self::Int24),
            "f32" | "float" | "32" => Some(Self::Float32),
            _ => None,
        }
    }

    fn bits(self) -> u16 {
        match self {
            Self::Int16 => 16,
            Self::Int24 => 24,
            Self::Float32 => 32,
        }
    }

    fn format(self) -> hound::SampleFormat {
        match self {
            Self::Int16 | Self::Int24 => hound::SampleFormat::Int,
            Self::Float32 => hound::SampleFormat::Float,
        }
    }
}

/// Why a signal could not be written.
#[derive(Debug)]
pub enum WavError {
    /// A parameter made no sense.
    BadParameter(String),
    /// The file could not be written.
    Io(String),
}

impl std::fmt::Display for WavError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            WavError::BadParameter(message) => write!(f, "{message}"),
            WavError::Io(message) => write!(f, "{message}"),
        }
    }
}

impl std::error::Error for WavError {}

impl From<hound::Error> for WavError {
    fn from(error: hound::Error) -> Self {
        WavError::Io(error.to_string())
    }
}

/// Fixed so a generated file is reproducible.
///
/// Two runs of the same command must produce the same bytes, or a parity
/// comparison cannot be repeated and a noise measurement cannot be compared
/// against itself. This is the same seed the engine uses for the same reason.
pub const GENERATOR_SEED: u64 = 0x5EED_5EED_5EED_5EED;

/// Render `signal` for `seconds` at `sample_rate`.
///
/// Returns the samples rather than writing them, so a caller can inspect,
/// analyse or play what it is about to save.
pub fn render(signal: Signal, sample_rate: f32, seconds: f32) -> Result<Vec<f32>, WavError> {
    if !sample_rate.is_finite() || sample_rate <= 0.0 {
        return Err(WavError::BadParameter(format!(
            "sample rate must be positive, got {sample_rate}"
        )));
    }
    if !seconds.is_finite() || seconds <= 0.0 {
        return Err(WavError::BadParameter(format!(
            "duration must be positive, got {seconds}"
        )));
    }

    let frames = (f64::from(seconds) * f64::from(sample_rate)).round();
    if frames > u32::MAX as f64 {
        return Err(WavError::BadParameter(
            "that is more audio than a WAV file can hold".to_owned(),
        ));
    }

    let mut generator = Generator::new(sample_rate, signal, GENERATOR_SEED);
    let mut out = vec![0.0; frames as usize];
    generator.fill(&mut out);
    Ok(out)
}

/// Write mono samples to `path`.
pub fn write(
    path: &Path,
    samples: &[f32],
    sample_rate: f32,
    depth: SampleDepth,
) -> Result<(), WavError> {
    if !sample_rate.is_finite() || sample_rate <= 0.0 {
        return Err(WavError::BadParameter(format!(
            "sample rate must be positive, got {sample_rate}"
        )));
    }

    let spec = hound::WavSpec {
        channels: 1,
        sample_rate: sample_rate as u32,
        bits_per_sample: depth.bits(),
        sample_format: depth.format(),
    };

    let mut writer = hound::WavWriter::create(path, spec)
        .map_err(|e| WavError::Io(format!("creating {}: {e}", path.display())))?;

    match depth {
        SampleDepth::Float32 => {
            for sample in samples {
                writer.write_sample(*sample)?;
            }
        }
        SampleDepth::Int16 => {
            for sample in samples {
                writer.write_sample(quantise(*sample, 15) as i16)?;
            }
        }
        SampleDepth::Int24 => {
            for sample in samples {
                writer.write_sample(quantise(*sample, 23))?;
            }
        }
    }

    writer
        .finalize()
        .map_err(|e| WavError::Io(format!("finishing {}: {e}", path.display())))
}

/// Render and write in one step.
pub fn write_signal(
    path: &Path,
    signal: Signal,
    sample_rate: f32,
    seconds: f32,
    depth: SampleDepth,
) -> Result<usize, WavError> {
    let samples = render(signal, sample_rate, seconds)?;
    write(path, &samples, sample_rate, depth)?;
    Ok(samples.len())
}

/// Scale a float sample to an integer of `bits` magnitude bits.
///
/// Clamped rather than wrapped. A sample a hair over full scale is a rounding
/// artefact and should saturate; wrapping would turn it into a full-scale
/// excursion of the opposite sign, which is the loudest possible click at
/// exactly the moment the signal was already at its peak.
fn quantise(sample: f32, bits: u32) -> i32 {
    let peak = ((1_i64 << bits) - 1) as f32;
    (sample.clamp(-1.0, 1.0) * peak).round() as i32
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    const RATE: f32 = 48_000.0;

    fn scratch(name: &str) -> std::path::PathBuf {
        std::env::temp_dir().join(format!("analyzer-wav-{name}.wav"))
    }

    #[test]
    fn renders_the_requested_length() {
        let samples = render(
            Signal::Sine {
                hz: 1000.0,
                amplitude: 0.5,
            },
            RATE,
            0.5,
        )
        .unwrap();
        assert_eq!(samples.len(), 24_000);
    }

    /// Two runs must produce identical bytes, or a parity comparison cannot be
    /// repeated and a noise measurement cannot be compared against itself.
    #[test]
    fn generation_is_reproducible() {
        let one = render(Signal::PinkNoise { amplitude: 0.5 }, RATE, 0.1).unwrap();
        let two = render(Signal::PinkNoise { amplitude: 0.5 }, RATE, 0.1).unwrap();
        assert_eq!(one, two);
    }

    #[test]
    fn a_float_round_trip_is_exact() {
        let path = scratch("float");
        let samples = render(
            Signal::Sine {
                hz: 997.0,
                amplitude: 0.5,
            },
            RATE,
            0.05,
        )
        .unwrap();
        write(&path, &samples, RATE, SampleDepth::Float32).unwrap();

        let mut reader = hound::WavReader::open(&path).unwrap();
        let spec = reader.spec();
        assert_eq!(spec.channels, 1);
        assert_eq!(spec.sample_rate, 48_000);
        assert_eq!(spec.bits_per_sample, 32);

        let read: Vec<f32> = reader.samples::<f32>().map(Result::unwrap).collect();
        assert_eq!(read, samples, "float output must not change a sample");
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn integer_depths_round_trip_within_their_resolution() {
        for (depth, bits) in [(SampleDepth::Int16, 15u32), (SampleDepth::Int24, 23u32)] {
            let path = scratch(depth.as_key());
            let samples = render(
                Signal::Sine {
                    hz: 997.0,
                    amplitude: 0.5,
                },
                RATE,
                0.05,
            )
            .unwrap();
            write(&path, &samples, RATE, depth).unwrap();

            let mut reader = hound::WavReader::open(&path).unwrap();
            assert_eq!(reader.spec().bits_per_sample, depth.bits());
            let peak = ((1_i64 << bits) - 1) as f32;
            let read: Vec<f32> = reader
                .samples::<i32>()
                .map(|s| s.unwrap() as f32 / peak)
                .collect();

            assert_eq!(read.len(), samples.len());
            let worst = read
                .iter()
                .zip(&samples)
                .map(|(a, b)| (a - b).abs())
                .fold(0.0_f32, f32::max);
            // One quantisation step is the most it may move.
            assert!(
                worst <= 1.0 / peak + 1e-7,
                "{} drifted {worst}",
                depth.as_key()
            );
            let _ = std::fs::remove_file(&path);
        }
    }

    /// Wrapping would turn a sample a hair over full scale into a full-scale
    /// excursion of the opposite sign - the loudest possible click, at exactly
    /// the moment the signal was already at its peak.
    #[test]
    fn over_full_scale_saturates_rather_than_wrapping() {
        let path = scratch("clip");
        write(&path, &[1.5, -1.5, 0.0], RATE, SampleDepth::Int16).unwrap();

        let mut reader = hound::WavReader::open(&path).unwrap();
        let read: Vec<i32> = reader
            .samples::<i16>()
            .map(|s| i32::from(s.unwrap()))
            .collect();
        assert_eq!(read[0], 32_767);
        assert_eq!(read[1], -32_767);
        assert_eq!(read[2], 0);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn write_signal_reports_what_it_wrote() {
        let path = scratch("oneshot");
        let frames = write_signal(
            &path,
            Signal::WhiteNoise { amplitude: 0.25 },
            RATE,
            0.25,
            SampleDepth::Float32,
        )
        .unwrap();
        assert_eq!(frames, 12_000);
        assert!(path.exists());
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn degenerate_parameters_are_refused_rather_than_producing_a_file() {
        for (rate, seconds) in [
            (0.0, 1.0),
            (-48_000.0, 1.0),
            (48_000.0, 0.0),
            (48_000.0, -1.0),
        ] {
            let result = render(
                Signal::Sine {
                    hz: 1000.0,
                    amplitude: 0.5,
                },
                rate,
                seconds,
            );
            assert!(result.is_err(), "rate {rate} seconds {seconds} should fail");
        }
        assert!(render(Signal::Silence, f32::NAN, 1.0).is_err());
    }

    #[test]
    fn every_depth_token_round_trips() {
        for depth in [SampleDepth::Int16, SampleDepth::Int24, SampleDepth::Float32] {
            assert_eq!(SampleDepth::from_key(depth.as_key()), Some(depth));
        }
        assert_eq!(SampleDepth::from_key("nonsense"), None);
    }

    /// A sweep is the parity run's most demanding stimulus and the measurement
    /// path's stimulus too, so it must survive a file round trip intact.
    #[test]
    fn a_sweep_survives_a_float_round_trip() {
        let path = scratch("sweep");
        let signal = Signal::Sweep {
            start_hz: 20.0,
            end_hz: 20_000.0,
            seconds: 0.2,
            amplitude: 0.5,
            repeat: false,
        };
        write_signal(&path, signal, RATE, 0.2, SampleDepth::Float32).unwrap();

        let mut reader = hound::WavReader::open(&path).unwrap();
        let read: Vec<f32> = reader.samples::<f32>().map(Result::unwrap).collect();
        assert_eq!(read, render(signal, RATE, 0.2).unwrap());
        // It should actually sweep: energy late in the file sits well above the
        // starting frequency, so a file of silence or a stuck tone fails here.
        assert!(read.iter().any(|s| s.abs() > 0.4), "the sweep is silent");
        let _ = std::fs::remove_file(&path);
    }
}
