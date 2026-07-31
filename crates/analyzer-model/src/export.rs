//! Text export, in the format REW imports.
//!
//! This is the interoperability bridge the plan settles on. REW's own `.mdat` is
//! Java object serialisation and not worth reading from another language, but
//! its text import is a documented three-column format that every acoustics tool
//! understands. Emitting it exactly means measurements taken here can be opened
//! in REW, compared with a decade of existing files, and checked against an
//! independent implementation — which is also how the accuracy parity test works.
//!
//! ```text
//! * Measurement data
//! * Freq(Hz) SPL(dB) Phase(degrees)
//! 20.000000 72.4310 -14.2100
//! ```
//!
//! Phase is emitted only when it is real. Padding the column with zeros for data
//! that has no phase reference would be fabricating it, and a reader has no way
//! to tell the difference.

use std::fmt::Write as _;

use crate::measurement::{Measurement, MeasurementData};

/// Render a measurement as REW-compatible text.
///
/// `spl_reference` optionally converts dBFS to dB SPL on the way out; without it
/// the level column stays in dBFS and the header says so, rather than labelling
/// dBFS as SPL.
pub fn to_text(measurement: &Measurement) -> String {
    let mut out = String::new();
    let offset = measurement.references.spl_offset_db.unwrap_or(0.0);
    let calibrated = measurement.references.spl_offset_db.is_some();

    let _ = writeln!(out, "* Measurement data saved by analyzer");
    let _ = writeln!(out, "* Name: {}", measurement.name.replace('\n', " "));
    let _ = writeln!(out, "* Sample rate: {} Hz", measurement.sample_rate);
    if let Some(delay) = measurement.references.propagation_delay_seconds {
        let _ = writeln!(out, "* Propagation delay removed: {delay} s");
    }
    if calibrated {
        let _ = writeln!(out, "* SPL offset applied: {offset} dB");
    } else {
        let _ = writeln!(
            out,
            "* Levels are dBFS - this measurement is not SPL calibrated"
        );
    }

    match &measurement.data {
        MeasurementData::Spectrum {
            bins,
            bin_spacing_hz,
        } => {
            let _ = writeln!(out, "* Freq(Hz) SPL(dB) Phase(degrees)");
            for (index, bin) in bins.iter().enumerate() {
                let magnitude = bin.norm();
                let level = if magnitude > 0.0 {
                    20.0 * magnitude.log10() + offset
                } else {
                    -200.0
                };
                let _ = writeln!(
                    out,
                    "{:.6} {:.4} {:.4}",
                    index as f64 * bin_spacing_hz,
                    level,
                    bin.arg().to_degrees()
                );
            }
        }
        MeasurementData::PowerSpectrum {
            magnitude_db,
            bin_spacing_hz,
        } => {
            // Two columns, not three. REW's importer accepts a missing phase
            // column, and inventing one would be worse than omitting it.
            let _ = writeln!(out, "* Freq(Hz) SPL(dB)");
            for (index, level) in magnitude_db.iter().enumerate() {
                let _ = writeln!(
                    out,
                    "{:.6} {:.4}",
                    index as f64 * bin_spacing_hz,
                    level + offset
                );
            }
        }
        MeasurementData::TransferFunction {
            bins,
            coherence,
            bin_spacing_hz,
        } => {
            // Coherence is not part of REW's import format, so it rides along as
            // a comment column. A reader that ignores it loses nothing; one that
            // wants it can have it, rather than it being silently discarded.
            let _ = writeln!(out, "* Freq(Hz) SPL(dB) Phase(degrees) [coherence]");
            for (index, bin) in bins.iter().enumerate() {
                let magnitude = bin.norm();
                let level = if magnitude > 0.0 {
                    20.0 * magnitude.log10() + offset
                } else {
                    -200.0
                };
                let gamma = coherence.get(index).copied().unwrap_or(0.0);
                let _ = writeln!(
                    out,
                    "{:.6} {:.4} {:.4} * {gamma:.4}",
                    index as f64 * bin_spacing_hz,
                    level,
                    bin.arg().to_degrees()
                );
            }
        }
        MeasurementData::ImpulseResponse {
            samples,
            time_zero_samples,
        } => {
            // A different shape entirely: time against amplitude, with t = 0 at
            // the recorded arrival rather than at the first sample.
            let _ = writeln!(out, "* Time(s) Amplitude");
            let _ = writeln!(out, "* Time zero at sample {time_zero_samples}");
            let rate = if measurement.sample_rate > 0.0 {
                measurement.sample_rate
            } else {
                1.0
            };
            for (index, sample) in samples.iter().enumerate() {
                let seconds = (index as f64 - time_zero_samples) / rate;
                let _ = writeln!(out, "{seconds:.9} {sample:.9}");
            }
        }
    }
    out
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::measurement::{Complex64, MeasurementId};

    fn spectrum() -> Measurement {
        Measurement::new(
            MeasurementId(1),
            "Left",
            48_000.0,
            MeasurementData::Spectrum {
                // Unit magnitude at 0 degrees, then 0.5 at +90.
                bins: vec![Complex64::new(1.0, 0.0), Complex64::new(0.0, 0.5)],
                bin_spacing_hz: 10.0,
            },
        )
    }

    fn data_rows(text: &str) -> Vec<&str> {
        text.lines().filter(|line| !line.starts_with('*')).collect()
    }

    #[test]
    fn the_column_layout_matches_what_rew_imports() {
        let text = to_text(&spectrum());
        assert!(text.contains("* Freq(Hz) SPL(dB) Phase(degrees)"));

        let rows = data_rows(&text);
        assert_eq!(rows.len(), 2);
        let fields: Vec<&str> = rows[0].split_whitespace().collect();
        assert_eq!(fields.len(), 3, "three columns: {:?}", rows[0]);
        assert_eq!(fields[0], "0.000000");
        assert_eq!(fields[1], "0.0000", "unit magnitude is 0 dB");
    }

    #[test]
    fn frequencies_follow_the_bin_spacing() {
        let text = to_text(&spectrum());
        let rows = data_rows(&text);
        assert!(rows[1].starts_with("10.000000"));
    }

    #[test]
    fn phase_is_emitted_from_the_complex_value() {
        let text = to_text(&spectrum());
        let rows = data_rows(&text);
        let phase: f64 = rows[1].split_whitespace().nth(2).unwrap().parse().unwrap();
        assert!((phase - 90.0).abs() < 1e-3, "got {phase}");
    }

    /// An uncalibrated measurement must say so rather than labelling dBFS as SPL.
    #[test]
    fn uncalibrated_output_is_labelled_honestly() {
        let text = to_text(&spectrum());
        assert!(text.contains("not SPL calibrated"));

        let rows = data_rows(&text);
        let level: f64 = rows[0].split_whitespace().nth(1).unwrap().parse().unwrap();
        assert!((level - 0.0).abs() < 1e-6, "no offset should be applied");
    }

    #[test]
    fn a_calibrated_measurement_has_its_offset_applied() {
        let mut m = spectrum();
        m.references.spl_offset_db = Some(94.0);
        let text = to_text(&m);

        assert!(text.contains("SPL offset applied: 94"));
        let rows = data_rows(&text);
        let level: f64 = rows[0].split_whitespace().nth(1).unwrap().parse().unwrap();
        assert!((level - 94.0).abs() < 1e-6, "got {level}");
    }

    #[test]
    fn transfer_function_export_carries_coherence_as_a_comment() {
        let m = Measurement::new(
            MeasurementId(2),
            "TF",
            48_000.0,
            MeasurementData::TransferFunction {
                bins: vec![Complex64::new(1.0, 0.0)],
                coherence: vec![0.87],
                bin_spacing_hz: 10.0,
            },
        );
        let text = to_text(&m);
        let rows = data_rows(&text);
        assert!(rows[0].contains("* 0.8700"), "row was {:?}", rows[0]);
        // The three real columns still come first, so a plain reader works.
        let fields: Vec<&str> = rows[0].split_whitespace().collect();
        assert_eq!(fields.len(), 5);
    }

    /// t = 0 sits at the recorded arrival, not at the first sample, so samples
    /// before it must carry negative times.
    #[test]
    fn impulse_export_places_time_zero_at_the_arrival() {
        let m = Measurement::new(
            MeasurementId(3),
            "IR",
            48_000.0,
            MeasurementData::ImpulseResponse {
                samples: vec![0.0, 0.0, 1.0, 0.5],
                time_zero_samples: 2.0,
            },
        );
        let text = to_text(&m);
        let rows = data_rows(&text);
        assert_eq!(rows.len(), 4);

        let first: f64 = rows[0].split_whitespace().next().unwrap().parse().unwrap();
        let third: f64 = rows[2].split_whitespace().next().unwrap().parse().unwrap();
        assert!(
            first < 0.0,
            "samples before the arrival are negative: {first}"
        );
        assert!(third.abs() < 1e-12, "the arrival itself is t = 0");
    }

    #[test]
    fn a_silent_bin_floors_rather_than_writing_infinity() {
        let m = Measurement::new(
            MeasurementId(4),
            "silent",
            48_000.0,
            MeasurementData::Spectrum {
                bins: vec![Complex64::new(0.0, 0.0)],
                bin_spacing_hz: 1.0,
            },
        );
        let text = to_text(&m);
        let rows = data_rows(&text);
        let level: f64 = rows[0].split_whitespace().nth(1).unwrap().parse().unwrap();
        assert!(level.is_finite() && level <= -200.0);
    }

    #[test]
    fn newlines_in_a_name_do_not_break_the_header() {
        let mut m = spectrum();
        m.name = "two\nlines".into();
        let text = to_text(&m);
        let comment_lines = text.lines().filter(|l| l.starts_with('*')).count();
        assert!(
            text.contains("* Name: two lines"),
            "name should be flattened onto one line"
        );
        assert!(comment_lines >= 4);
    }
}
