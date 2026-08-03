//! Comparing two frequency responses.
//!
//! Built for the REW parity run, which is the project's one unmet commitment:
//! feed identical input through this analyser and through REW, and match its
//! exported magnitude to ±0.1 dB on synthetic signals and ±0.5 dB on a real
//! measurement, 20 Hz to 20 kHz.
//!
//! ## Why a constant offset is reported separately
//!
//! Two analysers can disagree for two very different reasons, and lumping them
//! together wastes the run.
//!
//! A **constant** offset across the whole band is a reference convention. `0
//! dBFS = full-scale sine` and `0 dBFS = full-scale square` differ by 3.01 dB
//! and both are defensible; so does reporting per-bin level against power per
//! hertz, which differs by the window's noise bandwidth. None of that is a
//! defect, and a run that fails on it tells you nothing you did not already
//! know.
//!
//! A **frequency-dependent** deviation is the thing worth finding: a window
//! amplitude correction applied where it should not be, an FFT scaling that
//! forgot a factor of two, a calibration constant. That is what
//! [`Comparison::deviation_after_offset`] isolates, and it is the number the
//! exit criterion should be read against once the conventions are reconciled.
//!
//! ## Interpolation
//!
//! The two files rarely share a frequency grid. The reference is interpolated
//! onto the subject's frequencies, linearly in decibels against **log**
//! frequency, matching how every other sparse curve in this codebase is read.
//!
//! Interpolation is itself a source of error, and near a sharp peak it can
//! exceed the tolerance being tested. [`Comparison::interpolated`] reports how
//! many points needed it, so a run that is mostly interpolation can be
//! recognised as one rather than believed.

use std::fmt::Write as _;

/// A frequency response read from a text file.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Response {
    /// `(hz, db)` pairs, sorted ascending by frequency.
    pub points: Vec<(f64, f64)>,
}

impl Response {
    /// Parse `frequency level` rows from text.
    ///
    /// Deliberately forgiving about shape, because the whole point is reading
    /// another application's export: comment markers (`*`, `#`, `;`), blank
    /// lines and header rows are skipped, and columns may be separated by tabs,
    /// commas, semicolons or spaces. A third column, which REW uses for phase,
    /// is ignored.
    ///
    /// Rows that are not two numbers are skipped rather than fatal. An export
    /// with a stray legend in the middle should still compare.
    pub fn parse(text: &str) -> Self {
        let mut points = Vec::new();

        for line in text.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with(['*', '#', ';', '/']) {
                continue;
            }

            let mut fields = line
                .split(['\t', ',', ';', ' '])
                .filter(|field| !field.is_empty());
            let (Some(first), Some(second)) = (fields.next(), fields.next()) else {
                continue;
            };
            let (Ok(hz), Ok(db)) = (first.parse::<f64>(), second.parse::<f64>()) else {
                continue;
            };
            // A log axis cannot represent DC, and a non-finite level is not a
            // measurement. Both appear in real exports.
            if hz > 0.0 && hz.is_finite() && db.is_finite() {
                points.push((hz, db));
            }
        }

        points.sort_by(|a, b| a.0.total_cmp(&b.0));
        Self { points }
    }

    /// Level at `hz`, interpolated in decibels against log frequency.
    ///
    /// Returns `None` outside the covered range rather than extrapolating: a
    /// file that stops at 20 kHz says nothing about 22 kHz, and inventing a
    /// value there would put fabricated data into a parity result.
    pub fn level_at(&self, hz: f64) -> Option<f64> {
        match self.points.as_slice() {
            [] => None,
            [(only_hz, db)] => (*only_hz == hz).then_some(*db),
            points => {
                let first = points[0];
                let last = points[points.len() - 1];
                if hz < first.0 || hz > last.0 {
                    return None;
                }
                if hz == first.0 {
                    return Some(first.1);
                }

                let index = points.partition_point(|(point_hz, _)| *point_hz < hz);
                let (high_hz, high_db) = points[index];
                if high_hz == hz {
                    return Some(high_db);
                }
                let (low_hz, low_db) = points[index - 1];
                if high_hz <= low_hz {
                    return Some(low_db);
                }

                let t = (hz / low_hz).ln() / (high_hz / low_hz).ln();
                Some(low_db + t * (high_db - low_db))
            }
        }
    }

    /// Whether a frequency falls exactly on a listed point.
    fn has_exact(&self, hz: f64) -> bool {
        self.points
            .binary_search_by(|(point_hz, _)| point_hz.total_cmp(&hz))
            .is_ok()
    }
}

/// The result of comparing two responses over a band.
#[derive(Debug, Clone, PartialEq)]
pub struct Comparison {
    /// Points compared.
    pub compared: usize,
    /// How many of those needed the reference to be interpolated.
    pub interpolated: usize,
    /// Largest absolute difference, in decibels.
    pub max_deviation: f64,
    /// Frequency at which that occurred.
    pub max_deviation_hz: f64,
    /// Root-mean-square difference, in decibels.
    pub rms_deviation: f64,
    /// Mean difference: the constant offset between the two.
    pub mean_offset: f64,
    /// Largest absolute difference once `mean_offset` is removed.
    pub max_deviation_after_offset: f64,
    /// Frequency at which that occurred.
    pub max_deviation_after_offset_hz: f64,
    /// Root-mean-square difference once `mean_offset` is removed.
    pub rms_deviation_after_offset: f64,
    /// Low end of the band compared.
    pub from_hz: f64,
    /// High end of the band compared.
    pub to_hz: f64,
}

impl Comparison {
    /// Whether the shapes agree to `tolerance`, ignoring a constant offset.
    ///
    /// This is what the parity criterion should be read against once the
    /// reference conventions have been reconciled, because a convention
    /// mismatch is not a defect.
    pub fn agrees_within(&self, tolerance: f64) -> bool {
        self.compared > 0 && self.max_deviation_after_offset <= tolerance
    }

    /// A human-readable report.
    pub fn report(&self) -> String {
        let mut out = String::new();
        let _ = writeln!(out, "# spectrum comparison");
        let _ = writeln!(
            out,
            "# band: {:.1} Hz to {:.1} Hz",
            self.from_hz, self.to_hz
        );
        let _ = writeln!(
            out,
            "# points: {} ({} interpolated)",
            self.compared, self.interpolated
        );
        if self.compared == 0 {
            let _ = writeln!(out, "#");
            let _ = writeln!(
                out,
                "# nothing overlapped. Check the two files cover the same band."
            );
            return out;
        }
        let _ = writeln!(out, "#");
        let _ = writeln!(out, "# as measured");
        let _ = writeln!(
            out,
            "#   max deviation   {:+.4} dB at {:.1} Hz",
            self.max_deviation, self.max_deviation_hz
        );
        let _ = writeln!(out, "#   rms deviation   {:.4} dB", self.rms_deviation);
        let _ = writeln!(out, "#");
        let _ = writeln!(
            out,
            "# constant offset  {:+.4} dB  (a reference convention, not a defect)",
            self.mean_offset
        );
        let _ = writeln!(out, "#");
        let _ = writeln!(out, "# with that offset removed - the number that matters");
        let _ = writeln!(
            out,
            "#   max deviation   {:+.4} dB at {:.1} Hz",
            self.max_deviation_after_offset, self.max_deviation_after_offset_hz
        );
        let _ = writeln!(
            out,
            "#   rms deviation   {:.4} dB",
            self.rms_deviation_after_offset
        );
        out
    }
}

/// Compare `subject` against `reference` over `[from_hz, to_hz]`.
///
/// Every point of `subject` inside the band is compared against `reference`
/// interpolated to the same frequency. Points the reference does not cover are
/// skipped rather than counted as agreement.
pub fn compare(subject: &Response, reference: &Response, from_hz: f64, to_hz: f64) -> Comparison {
    let mut differences: Vec<(f64, f64)> = Vec::new();
    let mut interpolated = 0;

    for (hz, level) in &subject.points {
        if *hz < from_hz || *hz > to_hz {
            continue;
        }
        let Some(other) = reference.level_at(*hz) else {
            continue;
        };
        if !reference.has_exact(*hz) {
            interpolated += 1;
        }
        differences.push((*hz, level - other));
    }

    let compared = differences.len();
    if compared == 0 {
        return Comparison {
            compared: 0,
            interpolated: 0,
            max_deviation: 0.0,
            max_deviation_hz: 0.0,
            rms_deviation: 0.0,
            mean_offset: 0.0,
            max_deviation_after_offset: 0.0,
            max_deviation_after_offset_hz: 0.0,
            rms_deviation_after_offset: 0.0,
            from_hz,
            to_hz,
        };
    }

    let mean_offset = differences.iter().map(|(_, d)| d).sum::<f64>() / compared as f64;

    let worst = |select: &dyn Fn(f64) -> f64| -> (f64, f64) {
        differences
            .iter()
            .map(|(hz, d)| (*hz, select(*d)))
            .max_by(|a, b| a.1.abs().total_cmp(&b.1.abs()))
            .unwrap_or((0.0, 0.0))
    };
    let rms = |select: &dyn Fn(f64) -> f64| -> f64 {
        (differences
            .iter()
            .map(|(_, d)| select(*d).powi(2))
            .sum::<f64>()
            / compared as f64)
            .sqrt()
    };

    let (max_hz, max_deviation) = worst(&|d| d);
    let (offset_hz, max_after) = worst(&|d| d - mean_offset);

    Comparison {
        compared,
        interpolated,
        max_deviation,
        max_deviation_hz: max_hz,
        rms_deviation: rms(&|d| d),
        mean_offset,
        max_deviation_after_offset: max_after.abs(),
        max_deviation_after_offset_hz: offset_hz,
        rms_deviation_after_offset: rms(&|d| d - mean_offset),
        from_hz,
        to_hz,
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    fn response(points: &[(f64, f64)]) -> Response {
        Response {
            points: points.to_vec(),
        }
    }

    #[test]
    fn parses_our_own_output_format() {
        let text = "\
            # analyzer-cli spectrum\n\
            # sample rate: 48000 Hz\n\
            # frequency_hz\tlevel_db\n\
            20.000000\t-40.1000\n\
            1000.000000\t-6.0206\n\
            20000.000000\t-45.0000\n";
        let parsed = Response::parse(text);
        assert_eq!(parsed.points.len(), 3);
        assert_eq!(parsed.points[1], (1000.0, -6.0206));
    }

    /// REW's export uses `*` for comments and carries a third phase column.
    #[test]
    fn parses_a_rew_style_export() {
        let text = "\
            * Measurement data measured by REW V5.19\n\
            * Freq(Hz) SPL(dB) Phase(degrees)\n\
            20.000 72.431 -14.210\n\
            1000.000 80.100 3.400\n";
        let parsed = Response::parse(text);
        assert_eq!(parsed.points.len(), 2);
        assert_eq!(parsed.points[0], (20.0, 72.431));
        assert_eq!(parsed.points[1], (1000.0, 80.1));
    }

    #[test]
    fn skips_rows_that_are_not_numbers_rather_than_failing() {
        let text = "20 -40\nlegend goes here\n\n1000 -6\nnot,numbers\n2000 -7\n";
        let parsed = Response::parse(text);
        assert_eq!(parsed.points.len(), 3);
    }

    #[test]
    fn drops_dc_and_non_finite_rows() {
        let parsed = Response::parse("0 -100\n-5 -90\n100 NaN\n1000 -6\n");
        assert_eq!(parsed.points, vec![(1000.0, -6.0)]);
    }

    #[test]
    fn unsorted_input_is_sorted() {
        let parsed = Response::parse("1000 -6\n20 -40\n500 -10\n");
        assert_eq!(
            parsed.points.iter().map(|p| p.0).collect::<Vec<_>>(),
            vec![20.0, 500.0, 1000.0]
        );
    }

    /// Interpolating in linear frequency would put the midpoint of 100 and 1000
    /// at 550 Hz; in log frequency it is at about 316, which is what a curve on
    /// a log axis means.
    #[test]
    fn interpolation_is_logarithmic() {
        let curve = response(&[(100.0, 0.0), (1000.0, 10.0)]);
        let mid = curve.level_at(316.227_766).unwrap();
        assert!((mid - 5.0).abs() < 1e-3, "got {mid}");
    }

    /// A file that stops at 20 kHz says nothing about 22 kHz, and inventing a
    /// value would put fabricated data into a parity result.
    #[test]
    fn outside_the_covered_range_there_is_no_answer() {
        let curve = response(&[(100.0, 0.0), (1000.0, 10.0)]);
        assert!(curve.level_at(50.0).is_none());
        assert!(curve.level_at(2000.0).is_none());
        assert_eq!(curve.level_at(100.0), Some(0.0));
        assert_eq!(curve.level_at(1000.0), Some(10.0));
    }

    #[test]
    fn identical_responses_agree_exactly() {
        let curve = response(&[(20.0, -40.0), (1000.0, -6.0), (20_000.0, -45.0)]);
        let result = compare(&curve, &curve, 20.0, 20_000.0);
        assert_eq!(result.compared, 3);
        assert_eq!(result.interpolated, 0);
        assert_eq!(result.max_deviation, 0.0);
        assert_eq!(result.mean_offset, 0.0);
        assert!(result.agrees_within(0.1));
    }

    /// The distinction the whole module exists for: a pure convention
    /// difference must not read as a failure.
    #[test]
    fn a_constant_offset_is_separated_from_the_shape() {
        let ours = response(&[(20.0, -40.0), (1000.0, -6.0), (20_000.0, -45.0)]);
        let theirs = response(&[(20.0, -43.01), (1000.0, -9.01), (20_000.0, -48.01)]);

        let result = compare(&ours, &theirs, 20.0, 20_000.0);
        assert!((result.mean_offset - 3.01).abs() < 1e-9);
        assert!((result.max_deviation - 3.01).abs() < 1e-9);
        assert!(
            result.max_deviation_after_offset < 1e-9,
            "the shapes are identical"
        );
        assert!(
            result.agrees_within(0.1),
            "a 3 dB reference convention must not read as a parity failure"
        );
    }

    /// And the converse: a deviation that varies with frequency must survive
    /// offset removal, because that is the defect worth finding.
    #[test]
    fn a_frequency_dependent_deviation_survives_offset_removal() {
        let ours = response(&[(20.0, 0.0), (1000.0, 0.0), (20_000.0, 0.0)]);
        let theirs = response(&[(20.0, -1.0), (1000.0, 0.0), (20_000.0, 1.0)]);

        let result = compare(&ours, &theirs, 20.0, 20_000.0);
        assert!(result.mean_offset.abs() < 1e-9, "no net offset");
        assert!((result.max_deviation_after_offset - 1.0).abs() < 1e-9);
        assert!(!result.agrees_within(0.1), "a 1 dB tilt must fail");
    }

    #[test]
    fn only_the_requested_band_is_compared() {
        let ours = response(&[(10.0, 50.0), (1000.0, 0.0), (30_000.0, 50.0)]);
        let theirs = response(&[(10.0, 0.0), (1000.0, 0.0), (30_000.0, 0.0)]);

        let result = compare(&ours, &theirs, 20.0, 20_000.0);
        assert_eq!(result.compared, 1, "only the 1 kHz point is in band");
        assert_eq!(result.max_deviation, 0.0);
    }

    /// A run that is mostly interpolation should be recognisable as one.
    #[test]
    fn interpolated_points_are_counted() {
        let ours = response(&[(100.0, 0.0), (200.0, 0.0), (1000.0, 0.0)]);
        let theirs = response(&[(100.0, 0.0), (1000.0, 0.0)]);

        let result = compare(&ours, &theirs, 20.0, 20_000.0);
        assert_eq!(result.compared, 3);
        assert_eq!(result.interpolated, 1, "only 200 Hz needed interpolating");
    }

    #[test]
    fn points_the_reference_does_not_cover_are_skipped_not_counted_as_agreement() {
        let ours = response(&[(20.0, 0.0), (1000.0, 0.0)]);
        let theirs = response(&[(500.0, 0.0), (2000.0, 0.0)]);

        let result = compare(&ours, &theirs, 20.0, 20_000.0);
        assert_eq!(result.compared, 1, "20 Hz is outside the reference");
    }

    #[test]
    fn no_overlap_reports_nothing_rather_than_perfect_agreement() {
        let ours = response(&[(20.0, 0.0)]);
        let theirs = response(&[(10_000.0, 0.0)]);

        let result = compare(&ours, &theirs, 20.0, 20_000.0);
        assert_eq!(result.compared, 0);
        assert!(
            !result.agrees_within(100.0),
            "nothing compared is not a pass"
        );
        assert!(result.report().contains("nothing overlapped"));
    }

    #[test]
    fn empty_input_is_survivable() {
        let empty = Response::default();
        let result = compare(&empty, &empty, 20.0, 20_000.0);
        assert_eq!(result.compared, 0);
        assert!(!result.report().is_empty());
    }

    #[test]
    fn the_report_names_both_numbers() {
        let ours = response(&[(1000.0, 0.0), (2000.0, 0.0)]);
        let theirs = response(&[(1000.0, -3.0), (2000.0, -3.0)]);
        let report = compare(&ours, &theirs, 20.0, 20_000.0).report();
        assert!(report.contains("constant offset"), "{report}");
        assert!(report.contains("+3.0000 dB"), "{report}");
        assert!(report.contains("offset removed"), "{report}");
    }
}
