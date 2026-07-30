//! Measuring the performance targets the plan commits to.
//!
//! Those targets were aspirations until something measured them. This is
//! deliberately not a micro-benchmark suite: nobody cares how many nanoseconds
//! one FFT takes, they care whether the analysis thread keeps up with the audio
//! thread and whether anything gets dropped over a long run.
//!
//! So the headline figure is **duty cycle** — CPU seconds spent per second of
//! audio analysed. Below 1.0 the chain keeps up; the plan asks for under 0.5 on
//! one performance core, leaving headroom for a machine that is also doing
//! something else.

use std::fmt::Write as _;
use std::time::{Duration, Instant};

use analyzer_dsp::{
    Averaging, Generator, Integration, LevelMeter, MeterWeighting, OctaveBands, Overlap, Signal,
    SpectrumAnalyzer, SpectrumConfig, TransferAveraging, TransferConfig, TransferFunction,
    WindowKind,
};
use analyzer_engine::{capture_ring, rt_section};

const RATE: f32 = 48_000.0;

/// Duty cycle the plan asks the analysis chain to stay under.
const DUTY_TARGET: f64 = 0.5;

/// Run every benchmark and return a report.
pub(crate) fn run(seconds: f64) -> String {
    let mut out = String::new();
    let _ = writeln!(out, "analyzer benchmarks");
    let _ = writeln!(out, "  sample rate: {RATE} Hz");
    let _ = writeln!(out, "  audio per case: {seconds:.1} s");
    let _ = writeln!(
        out,
        "  duty cycle = CPU seconds per second of audio; target < {DUTY_TARGET}\n"
    );

    spectrum_duty(&mut out, seconds);
    transfer_duty(&mut out, seconds);
    meter_duty(&mut out, seconds);
    octave_duty(&mut out, seconds);
    ring_soak(&mut out, seconds);

    out
}

fn audio(samples: usize, seed: u64) -> Vec<f32> {
    let mut generator = Generator::new(RATE, Signal::PinkNoise { amplitude: 0.5 }, seed);
    let mut buffer = vec![0.0; samples];
    generator.fill(&mut buffer);
    buffer
}

/// Duty cycle of the spectrum chain across the FFT sizes a user can pick.
fn spectrum_duty(out: &mut String, seconds: f64) {
    let _ = writeln!(out, "spectrum analysis, 75% overlap");
    let _ = writeln!(
        out,
        "  {:>7}  {:>9}  {:>10}  {:>8}  {:>7}",
        "fft", "frames/s", "duty", "realtime", "verdict"
    );

    let total = (RATE as f64 * seconds) as usize;
    let signal = audio(total, 1);

    for size in [1024_usize, 2048, 4096, 8192, 16_384] {
        let mut analyzer = SpectrumAnalyzer::new(SpectrumConfig {
            sample_rate: RATE,
            size,
            window: WindowKind::Hann,
            overlap: Overlap::ThreeQuarters,
            averaging: Averaging::Exponential { alpha: 0.2 },
        });
        let mut db = vec![0.0; analyzer.bins()];

        // Feed in realistic callback-sized blocks; one giant push would flatter
        // the result by amortising the per-call overhead away.
        let start = Instant::now();
        let mut frames = 0;
        for block in signal.chunks(512) {
            frames += analyzer.push(block);
        }
        // A UI reads at up to 120 Hz, so include that cost.
        for _ in 0..(seconds * 120.0) as usize {
            analyzer.write_db_fs(&mut db);
        }
        let elapsed = start.elapsed().as_secs_f64();

        let duty = elapsed / seconds;
        let _ = writeln!(
            out,
            "  {:>7}  {:>9.1}  {:>10.4}  {:>7.0}x  {:>7}",
            size,
            frames as f64 / seconds,
            duty,
            1.0 / duty,
            verdict(duty)
        );
    }
    let _ = writeln!(out);
}

/// The transfer function does two FFTs per frame plus the cross-spectrum, so it
/// is the most expensive thing in Milestone 1.
fn transfer_duty(out: &mut String, seconds: f64) {
    let _ = writeln!(out, "transfer function, two channels, 75% overlap");
    let _ = writeln!(
        out,
        "  {:>7}  {:>9}  {:>10}  {:>8}  {:>7}",
        "fft", "frames/s", "duty", "realtime", "verdict"
    );

    let total = (RATE as f64 * seconds) as usize;
    let reference = audio(total, 2);
    let measurement = audio(total, 3);

    for size in [2048_usize, 4096, 8192, 16_384] {
        let mut tf = TransferFunction::new(TransferConfig {
            sample_rate: RATE,
            size,
            window: WindowKind::Hann,
            overlap: Overlap::ThreeQuarters,
            averaging: TransferAveraging::Exponential { alpha: 0.2 },
        });
        let mut magnitude = vec![0.0; tf.bins()];
        let mut phase = vec![0.0; tf.bins()];
        let mut coherence = vec![0.0; tf.bins()];

        let start = Instant::now();
        let mut frames = 0;
        for (r, m) in reference.chunks(512).zip(measurement.chunks(512)) {
            frames += tf.push(r, m);
        }
        for _ in 0..(seconds * 120.0) as usize {
            tf.write_magnitude_db(&mut magnitude);
            tf.write_phase_degrees(&mut phase);
            tf.write_coherence(&mut coherence);
        }
        let elapsed = start.elapsed().as_secs_f64();

        let duty = elapsed / seconds;
        let _ = writeln!(
            out,
            "  {:>7}  {:>9.1}  {:>10.4}  {:>7.0}x  {:>7}",
            size,
            frames as f64 / seconds,
            duty,
            1.0 / duty,
            verdict(duty)
        );
    }
    let _ = writeln!(out);
}

fn meter_duty(out: &mut String, seconds: f64) {
    let _ = writeln!(out, "level meters, per-sample filtering");
    let total = (RATE as f64 * seconds) as usize;
    let signal = audio(total, 4);

    for weighting in [MeterWeighting::Z, MeterWeighting::A, MeterWeighting::C] {
        let mut meter = LevelMeter::new(RATE, weighting, Integration::Fast);
        let start = Instant::now();
        for block in signal.chunks(512) {
            meter.push(block);
        }
        let duty = start.elapsed().as_secs_f64() / seconds;
        let _ = writeln!(
            out,
            "  {:>7?}  {:>10.5}  {:>7.0}x  {:>7}",
            weighting,
            duty,
            1.0 / duty,
            verdict(duty)
        );
    }
    let _ = writeln!(out);
}

fn octave_duty(out: &mut String, seconds: f64) {
    let _ = writeln!(out, "octave banding, applied at 120 Hz");
    let bins = vec![-60.0_f32; 4097];
    let spacing = RATE / 8192.0;

    for fraction in [1_u32, 3, 12, 48] {
        let bands = OctaveBands::new(fraction, 20.0, 20_000.0);
        let mut levels = vec![0.0; bands.len()];

        let start = Instant::now();
        let iterations = (seconds * 120.0) as usize;
        for _ in 0..iterations {
            bands.apply(&bins, spacing, &mut levels);
        }
        let duty = start.elapsed().as_secs_f64() / seconds;
        let _ = writeln!(
            out,
            "  1/{:<5} {:>4} bands  {:>10.5}  {:>7}",
            fraction,
            bands.len(),
            duty,
            verdict(duty)
        );
    }
    let _ = writeln!(out);
}

/// Push audio through the ring at wall-clock rate and count what gets dropped.
///
/// This is the target that matters most, because an overrun is not a slow frame
/// - it is a hole in the data that makes the measurement wrong.
fn ring_soak(out: &mut String, seconds: f64) {
    let _ = writeln!(out, "ring soak, 128-frame blocks at wall-clock rate");

    let block_frames = 128_usize;
    let (mut sink, mut source) = capture_ring(2, 8192);
    let blocks = ((RATE as f64 * seconds) / block_frames as f64) as usize;

    let mut analyzer = SpectrumAnalyzer::new(SpectrumConfig {
        sample_rate: RATE,
        size: 8192,
        window: WindowKind::Hann,
        overlap: Overlap::ThreeQuarters,
        averaging: Averaging::Exponential { alpha: 0.2 },
    });

    let block = audio(block_frames * 2, 5);
    let mut interleaved = vec![0.0_f32; 4096 * 2];
    let mut mono = vec![0.0_f32; 4096];

    let mut worst_callback = Duration::ZERO;
    let start = Instant::now();

    for _ in 0..blocks {
        let callback_start = Instant::now();
        // Exactly what the audio thread does, inside the same guard.
        rt_section(|| {
            sink.write_interleaved(&block);
        });
        worst_callback = worst_callback.max(callback_start.elapsed());

        // And exactly what the analysis thread does.
        let frames = source.read_interleaved(&mut interleaved);
        if frames > 0 {
            for (frame, slot) in mono.iter_mut().take(frames).enumerate() {
                *slot = interleaved.get(frame * 2).copied().unwrap_or_default();
            }
            analyzer.push(mono.get(..frames).unwrap_or(&[]));
        }
    }

    let elapsed = start.elapsed().as_secs_f64();
    let budget_micros = block_frames as f64 / f64::from(RATE) * 1e6;

    let _ = writeln!(out, "  blocks:          {blocks}");
    let _ = writeln!(out, "  overruns:        {}", sink.overruns());
    let _ = writeln!(
        out,
        "  worst callback:  {:.1} us (budget {:.0} us)",
        worst_callback.as_secs_f64() * 1e6,
        budget_micros
    );
    let _ = writeln!(out, "  total duty:      {:.4}", elapsed / seconds);
    let _ = writeln!(
        out,
        "  verdict:         {}",
        if sink.overruns() == 0 { "PASS" } else { "FAIL" }
    );
}

fn verdict(duty: f64) -> &'static str {
    if duty < DUTY_TARGET { "PASS" } else { "FAIL" }
}
