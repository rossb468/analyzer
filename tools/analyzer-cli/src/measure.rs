//! End-to-end swept measurement.
//!
//! Takes a stimulus and a recorded response, deconvolves them into an impulse
//! response, and reports what falls out: arrival time and distance,
//! reverberation time, and the gated frequency response.
//!
//! The demo mode builds a synthetic room with a known geometry and decay, runs
//! the whole chain over it, and prints both what was constructed and what was
//! measured. That makes the Milestone 2 chain verifiable end to end with no
//! files, no hardware and no microphone permission — which is the same reason
//! the offline harness exists for Milestone 1.

use std::fmt::Write as _;
use std::path::Path;

use analyzer_dsp::{
    Deconvolver, Gate, Generator, ImpulseResponse, Signal, deconv::DEFAULT_REGULARISATION,
    gated_response, reverb_time, schroeder_decay,
};

/// Speed of sound used to turn a delay into a distance.
const SPEED_OF_SOUND: f32 = 343.0;

/// How a measurement was obtained.
#[derive(Debug, Clone)]
pub(crate) struct MeasureOptions {
    /// Sweep duration for the synthetic demo.
    pub(crate) seconds: f32,
    /// Gate length in milliseconds for the quasi-anechoic response.
    pub(crate) gate_ms: f32,
    /// Transform size for the gated response.
    pub(crate) fft: usize,
    /// Rows of frequency response to print.
    pub(crate) response_rows: usize,
}

impl Default for MeasureOptions {
    fn default() -> Self {
        Self {
            seconds: 1.0,
            gate_ms: 5.0,
            fft: 4096,
            response_rows: 24,
        }
    }
}

/// The synthetic room the demo measures.
///
/// Deliberately simple and exactly known, so the report can print the truth
/// beside the measurement and any discrepancy is visible rather than plausible.
struct SyntheticRoom {
    sample_rate: f32,
    /// Direct arrival in seconds.
    direct_seconds: f32,
    /// Discrete reflections as (seconds after direct, relative amplitude).
    reflections: [(f32, f32); 3],
    /// Reverberation time built into the tail.
    rt60: f32,
}

impl SyntheticRoom {
    fn new(sample_rate: f32) -> Self {
        Self {
            sample_rate,
            // 3.43 m, a plausible listening distance.
            direct_seconds: 0.010,
            reflections: [(0.006, 0.5), (0.011, 0.35), (0.018, 0.25)],
            rt60: 0.45,
        }
    }

    /// Convolve a stimulus with this room.
    ///
    /// The reverberant tail is built from sparse taps - a few thousand delayed,
    /// exponentially decaying copies scattered through the tail - rather than a
    /// dense impulse response. A dense convolution of a one second sweep with a
    /// one second tail is billions of operations for a demo, and sparse taps give
    /// the genuine exponential *energy* decay that a reverberation measurement
    /// actually reads.
    ///
    /// Adding a decaying noise burst straight to the response, which is the
    /// obvious shortcut, does not work: the tail then is not a filtered version
    /// of the stimulus, so deconvolution cannot recover it and reverberation
    /// reads about five times short.
    fn respond(&self, stimulus: &[f32], tail_seconds: f32) -> Vec<f32> {
        const TAIL_TAPS: usize = 1200;

        let tail = (tail_seconds * self.sample_rate) as usize;
        let direct = (self.direct_seconds * self.sample_rate) as usize;
        let mut out = vec![0.0_f32; stimulus.len() + direct + tail + 1];

        let mut arrivals = vec![(direct, 1.0_f32)];
        for (offset, gain) in self.reflections {
            arrivals.push((direct + (offset * self.sample_rate) as usize, gain));
        }

        // Sparse diffuse tail, amplitude falling as exp(-t/tau) so energy falls
        // at 60 dB over rt60 seconds.
        let tau = self.rt60 / 6.908;
        let mut generator =
            Generator::new(self.sample_rate, Signal::WhiteNoise { amplitude: 1.0 }, 99);
        let mut jitter = vec![0.0_f32; TAIL_TAPS * 2];
        generator.fill(&mut jitter);

        for tap in 0..TAIL_TAPS {
            // Spread the taps across the tail, nudged so they are not periodic -
            // regular spacing would produce comb filtering rather than diffusion.
            let base = (tap as f32 / TAIL_TAPS as f32) * tail as f32;
            let nudge = jitter.get(tap * 2).copied().unwrap_or(0.0) * (tail as f32 * 0.0005);
            let offset = (base + nudge).max(0.0) as usize;
            let t = offset as f32 / self.sample_rate;
            let sign = if jitter.get(tap * 2 + 1).copied().unwrap_or(0.0) < 0.0 {
                -1.0
            } else {
                1.0
            };
            // Scaled so the whole tail sits below the direct arrival.
            let gain = sign * 0.06 * (-t / tau).exp();
            arrivals.push((direct + offset, gain));
        }

        for (delay, gain) in arrivals {
            if gain.abs() < 1e-6 {
                continue;
            }
            for (index, sample) in stimulus.iter().enumerate() {
                if let Some(slot) = out.get_mut(index + delay) {
                    *slot += sample * gain;
                }
            }
        }
        out
    }
}

/// Run the synthetic demo.
pub(crate) fn demo(options: &MeasureOptions) -> Result<String, String> {
    let sample_rate = 48_000.0_f32;
    let room = SyntheticRoom::new(sample_rate);

    let samples = (sample_rate * options.seconds) as usize;
    let mut generator = Generator::new(
        sample_rate,
        Signal::Sweep {
            start_hz: 20.0,
            end_hz: 20_000.0,
            seconds: options.seconds,
            amplitude: 0.5,
            repeat: false,
        },
        1,
    );
    let mut stimulus = vec![0.0_f32; samples];
    generator.fill(&mut stimulus);

    let response = room.respond(&stimulus, room.rt60 * 3.0);

    let mut out = String::new();
    let _ = writeln!(out, "# synthetic room measurement");
    let _ = writeln!(out, "#");
    let _ = writeln!(out, "# constructed:");
    let _ = writeln!(
        out,
        "#   direct arrival     {:.2} ms ({:.2} m)",
        room.direct_seconds * 1000.0,
        room.direct_seconds * SPEED_OF_SOUND
    );
    for (offset, gain) in room.reflections {
        let _ = writeln!(
            out,
            "#   reflection         +{:.2} ms at {:.0}%",
            offset * 1000.0,
            gain * 100.0
        );
    }
    let _ = writeln!(out, "#   reverberation      {:.3} s", room.rt60);
    let _ = writeln!(out, "#");

    let report = analyse(&stimulus, &response, sample_rate, options)?;
    out.push_str(&report);
    Ok(out)
}

/// Measure from a stimulus and response already in memory.
pub(crate) fn analyse(
    stimulus: &[f32],
    response: &[f32],
    sample_rate: f32,
    options: &MeasureOptions,
) -> Result<String, String> {
    let longest = stimulus.len().max(response.len());
    let mut deconvolver = Deconvolver::new(sample_rate, longest);
    let ir = deconvolver
        .deconvolve(stimulus, response, DEFAULT_REGULARISATION)
        .ok_or("deconvolution failed - is either signal silent?")?;

    let mut out = String::new();
    let _ = writeln!(out, "# measured:");
    report_impulse(&mut out, &ir);
    report_reverberation(&mut out, &ir);
    report_response(&mut out, &ir, options)?;
    Ok(out)
}

fn report_impulse(out: &mut String, ir: &ImpulseResponse) {
    let arrival_seconds = ir.peak_samples / ir.sample_rate;
    let _ = writeln!(
        out,
        "#   direct arrival     {:.2} ms ({:.2} m)",
        arrival_seconds * 1000.0,
        arrival_seconds * SPEED_OF_SOUND
    );
    let _ = writeln!(
        out,
        "#   impulse length     {:.0} samples ({:.3} s)",
        ir.samples.len(),
        ir.duration_seconds()
    );

    // Discrete arrivals standing clear of the local background, which is what a
    // reflection looks like in an impulse response.
    let peak = ir.peak_amplitude();
    let start = ir.peak_samples as usize;
    let window = (0.030 * ir.sample_rate) as usize;
    let mut found = 0;
    for index in (start + 8)..(start + window).min(ir.samples.len()) {
        let Some(&value) = ir.samples.get(index) else {
            continue;
        };
        let magnitude = value.abs();
        if magnitude < peak * 0.15 {
            continue;
        }
        let is_local_peak = ir
            .samples
            .get(index - 1)
            .is_some_and(|previous| previous.abs() < magnitude)
            && ir
                .samples
                .get(index + 1)
                .is_some_and(|next| next.abs() <= magnitude);
        if !is_local_peak {
            continue;
        }
        let _ = writeln!(
            out,
            "#   reflection         +{:.2} ms at {:.0}%",
            (index as f32 - ir.peak_samples) / ir.sample_rate * 1000.0,
            magnitude / peak * 100.0
        );
        found += 1;
        if found >= 6 {
            break;
        }
    }
}

fn report_reverberation(out: &mut String, ir: &ImpulseResponse) {
    let decay = schroeder_decay(ir);
    let rt = reverb_time(&decay, ir.sample_rate);

    let show = |label: &str, value: Option<f32>| -> String {
        match value {
            Some(seconds) => format!("{label} {seconds:.3} s"),
            None => format!("{label} (insufficient range)"),
        }
    };
    let _ = writeln!(out, "#   {}", show("EDT               ", rt.edt));
    let _ = writeln!(out, "#   {}", show("T20               ", rt.t20));
    let _ = writeln!(out, "#   {}", show("T30               ", rt.t30));

    if let Some(spread) = rt.spread() {
        let verdict = if spread < 0.1 {
            "consistent"
        } else {
            "estimates disagree - the decay is not a straight line"
        };
        let _ = writeln!(
            out,
            "#   agreement          {:.1}% ({verdict})",
            spread * 100.0
        );
    }
}

fn report_response(
    out: &mut String,
    ir: &ImpulseResponse,
    options: &MeasureOptions,
) -> Result<(), String> {
    let gate = Gate::anechoic(options.gate_ms / 1000.0);
    let response = gated_response(ir, &gate, options.fft)
        .ok_or("the gate kept no samples - is it shorter than the arrival?")?;

    let _ = writeln!(out, "#");
    let _ = writeln!(
        out,
        "# gated response: {:.1} ms window, valid above {:.0} Hz",
        gate.length_seconds() * 1000.0,
        response.resolution_hz
    );
    let _ = writeln!(out, "# frequency_hz\tlevel_db\ttrustworthy");

    // Log-spaced rows, because a linear listing of 2048 bins helps nobody.
    let lowest = 20.0_f32;
    let highest = (ir.sample_rate / 2.0).min(20_000.0);
    let steps = options.response_rows.max(2);
    let ratio = (highest / lowest).powf(1.0 / (steps - 1) as f32);

    let mut hz = lowest;
    for _ in 0..steps {
        let bin = (hz / response.bin_spacing_hz).round() as usize;
        if let Some(level) = response.magnitude_db.get(bin) {
            let _ = writeln!(
                out,
                "{hz:.1}\t{level:.2}\t{}",
                if response.is_trustworthy(hz) {
                    "yes"
                } else {
                    "no"
                }
            );
        }
        hz *= ratio;
    }
    Ok(())
}

/// Measure from two WAV files.
pub(crate) fn from_files(
    stimulus_path: &Path,
    response_path: &Path,
    options: &MeasureOptions,
) -> Result<String, String> {
    let stimulus = crate::read_wav(stimulus_path)?;
    let response = crate::read_wav(response_path)?;

    if (stimulus.sample_rate - response.sample_rate).abs() > 0.5 {
        return Err(format!(
            "sample rate mismatch: stimulus is {} Hz, response is {} Hz",
            stimulus.sample_rate, response.sample_rate
        ));
    }

    // Both files are reduced to their first channel. A stimulus is
    // single-channel by nature, and a multi-channel recording needs an explicit
    // choice rather than a silent mixdown.
    let take_first = |source: &analyzer_audio::Source| -> Vec<f32> {
        source
            .samples
            .iter()
            .step_by(source.channels)
            .copied()
            .collect()
    };

    let mut out = String::new();
    let _ = writeln!(out, "# swept measurement");
    let _ = writeln!(out, "#   stimulus  {}", stimulus_path.display());
    let _ = writeln!(out, "#   response  {}", response_path.display());
    let _ = writeln!(out, "#   rate      {} Hz", stimulus.sample_rate);
    let _ = writeln!(out, "#");

    let report = analyse(
        &take_first(&stimulus),
        &take_first(&response),
        stimulus.sample_rate as f32,
        options,
    )?;
    out.push_str(&report);
    Ok(out)
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    fn field(report: &str, label: &str) -> Option<f32> {
        report
            .lines()
            .find(|line| line.contains(label))?
            .split_whitespace()
            .find_map(|token| token.parse::<f32>().ok())
    }

    /// The whole Milestone 2 chain over a room whose answers are known in
    /// advance: sweep, convolve, deconvolve, measure.
    #[test]
    fn the_demo_recovers_the_room_it_built() {
        let report = demo(&MeasureOptions::default()).unwrap();

        // The direct arrival was built at 10 ms.
        let measured: Vec<&str> = report
            .lines()
            .skip_while(|line| !line.contains("# measured:"))
            .collect();
        let arrival = measured
            .iter()
            .find(|line| line.contains("direct arrival"))
            .and_then(|line| line.split_whitespace().nth(3))
            .and_then(|token| token.parse::<f32>().ok())
            .expect("an arrival should be reported");
        assert!(
            (arrival - 10.0).abs() < 0.2,
            "arrival measured at {arrival} ms, built at 10"
        );
    }

    #[test]
    fn the_demo_recovers_the_reverberation_it_built() {
        let report = demo(&MeasureOptions::default()).unwrap();
        let t20 = field(&report, "T20").expect("T20 should be reported");
        // The tail was built with a 0.45 s reverberation time.
        assert!(
            (t20 - 0.45).abs() < 0.1,
            "T20 measured {t20:.3} s, built 0.45"
        );
    }

    #[test]
    fn the_demo_finds_the_reflections_it_built() {
        let report = demo(&MeasureOptions::default()).unwrap();
        let measured: String = report
            .lines()
            .skip_while(|line| !line.contains("# measured:"))
            .collect::<Vec<_>>()
            .join("\n");
        let reflections = measured.matches("reflection").count();
        assert!(
            reflections >= 2,
            "expected to find several reflections, found {reflections}"
        );
    }

    #[test]
    fn the_report_marks_which_frequencies_the_gate_can_support() {
        let report = demo(&MeasureOptions::default()).unwrap();
        assert!(report.contains("valid above"));
        // A 5 ms gate cannot support 20 Hz, but can support 10 kHz.
        let rows: Vec<&str> = report
            .lines()
            .filter(|line| !line.starts_with('#') && line.contains('\t'))
            .collect();
        assert!(!rows.is_empty());
        assert!(
            rows.first().unwrap().ends_with("no"),
            "20 Hz is not trustworthy"
        );
        assert!(rows.last().unwrap().ends_with("yes"), "20 kHz is");
    }

    #[test]
    fn a_longer_gate_lowers_the_trustworthy_limit() {
        let tight = demo(&MeasureOptions {
            gate_ms: 5.0,
            ..MeasureOptions::default()
        })
        .unwrap();
        let wide = demo(&MeasureOptions {
            gate_ms: 40.0,
            ..MeasureOptions::default()
        })
        .unwrap();

        let limit = |report: &str| -> f32 {
            report
                .lines()
                .find(|line| line.contains("valid above"))
                .and_then(|line| {
                    line.split_whitespace()
                        .rev()
                        .nth(1)
                        .and_then(|t| t.parse().ok())
                })
                .unwrap_or(f32::NAN)
        };
        assert!(
            limit(&wide) < limit(&tight),
            "a wider gate should reach lower: {} vs {}",
            limit(&wide),
            limit(&tight)
        );
    }

    #[test]
    fn silence_is_reported_rather_than_producing_a_report() {
        let silence = vec![0.0_f32; 4096];
        let result = analyse(&silence, &silence, 48_000.0, &MeasureOptions::default());
        assert!(result.is_err());
    }
}
