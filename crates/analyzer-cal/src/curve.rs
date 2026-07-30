//! Frequency response correction curves.
//!
//! A measurement microphone ships with a calibration file: a list of frequencies
//! and the decibels by which that specific capsule deviates from flat. Applying
//! it is the difference between measuring the room and measuring the room plus
//! the microphone.
//!
//! Interpolation is linear in dB against **log** frequency. Calibration files are
//! sparse and roughly log-spaced — a few dozen points across three decades — so
//! interpolating against linear frequency would badly misplace everything below
//! a few hundred hertz.

use std::fmt;

/// A sparse frequency response, sorted ascending by frequency.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct ResponseCurve {
    points: Vec<(f32, f32)>,
}

impl ResponseCurve {
    /// Build from `(hz, db)` pairs.
    ///
    /// Points are sorted and non-positive frequencies dropped, so a file listing
    /// a DC row or arriving out of order still works.
    pub fn new(mut points: Vec<(f32, f32)>) -> Self {
        points.retain(|(hz, db)| *hz > 0.0 && hz.is_finite() && db.is_finite());
        points.sort_by(|a, b| a.0.total_cmp(&b.0));
        points.dedup_by(|a, b| (a.0 - b.0).abs() < f32::EPSILON);
        Self { points }
    }

    /// A curve that corrects nothing.
    pub fn flat() -> Self {
        Self { points: Vec::new() }
    }

    /// Whether this curve would change anything.
    pub fn is_flat(&self) -> bool {
        self.points.is_empty()
    }

    /// The points, ascending by frequency.
    pub fn points(&self) -> &[(f32, f32)] {
        &self.points
    }

    /// Correction in decibels at `hz`.
    ///
    /// Outside the curve's range the nearest endpoint is held rather than
    /// extrapolated. Extrapolating a microphone's response past where it was
    /// actually measured invents data, and the error grows fastest exactly where
    /// the curve is steepest.
    pub fn db_at(&self, hz: f32) -> f32 {
        if self.points.is_empty() || !hz.is_finite() || hz <= 0.0 {
            return 0.0;
        }
        let Some(&(first_hz, first_db)) = self.points.first() else {
            return 0.0;
        };
        let Some(&(last_hz, last_db)) = self.points.last() else {
            return 0.0;
        };
        if hz <= first_hz {
            return first_db;
        }
        if hz >= last_hz {
            return last_db;
        }

        // Binary search for the bracketing pair.
        let upper = self.points.partition_point(|(f, _)| *f < hz);
        let (Some(&(low_hz, low_db)), Some(&(high_hz, high_db))) =
            (self.points.get(upper - 1), self.points.get(upper))
        else {
            return 0.0;
        };

        let span = high_hz.log10() - low_hz.log10();
        if span <= 0.0 {
            return low_db;
        }
        let t = (hz.log10() - low_hz.log10()) / span;
        low_db + (high_db - low_db) * t
    }

    /// Parse a calibration file.
    ///
    /// Accepts the format every microphone vendor and REW uses: comment lines
    /// starting with `*`, `#` or `;`, then whitespace or comma separated
    /// `frequency level [phase]`. Phase is ignored; magnitude correction is what
    /// a capsule file is for.
    ///
    /// Unparseable lines are skipped rather than failing the whole file, because
    /// vendor files routinely carry stray headers and trailing junk.
    pub fn parse(text: &str) -> Self {
        let mut points = Vec::new();
        for line in text.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with(['*', '#', ';', '"']) {
                continue;
            }
            let mut fields = line
                .split(|c: char| c.is_whitespace() || c == ',')
                .filter(|f| !f.is_empty());
            let (Some(hz), Some(db)) = (fields.next(), fields.next()) else {
                continue;
            };
            let (Ok(hz), Ok(db)) = (hz.parse::<f32>(), db.parse::<f32>()) else {
                continue;
            };
            points.push((hz, db));
        }
        Self::new(points)
    }
}

impl fmt::Display for ResponseCurve {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.is_flat() {
            return f.write_str("flat");
        }
        let (Some(first), Some(last)) = (self.points.first(), self.points.last()) else {
            return f.write_str("flat");
        };
        write!(
            f,
            "{} points, {:.0}-{:.0} Hz",
            self.points.len(),
            first.0,
            last.0
        )
    }
}

#[cfg(test)]
#[allow(clippy::indexing_slicing)]
mod tests {
    use super::*;

    fn curve() -> ResponseCurve {
        ResponseCurve::new(vec![
            (20.0, -2.0),
            (100.0, -0.5),
            (1000.0, 0.0),
            (10_000.0, 1.5),
            (20_000.0, 3.0),
        ])
    }

    #[test]
    fn a_flat_curve_corrects_nothing() {
        let flat = ResponseCurve::flat();
        assert!(flat.is_flat());
        for hz in [20.0, 1000.0, 20_000.0] {
            assert_eq!(flat.db_at(hz), 0.0);
        }
    }

    #[test]
    fn exact_points_return_their_own_value() {
        let curve = curve();
        assert!((curve.db_at(20.0) - -2.0).abs() < 1e-4);
        assert!((curve.db_at(1000.0) - 0.0).abs() < 1e-4);
        assert!((curve.db_at(20_000.0) - 3.0).abs() < 1e-4);
    }

    /// Interpolation is logarithmic in frequency. The geometric midpoint between
    /// 100 Hz and 10 kHz is 1 kHz, so a value there must land halfway between
    /// the two endpoints' levels — linear-in-frequency interpolation would put
    /// the halfway point at 5050 Hz and be badly wrong down low.
    #[test]
    fn interpolation_is_logarithmic_in_frequency() {
        let curve = ResponseCurve::new(vec![(100.0, 0.0), (10_000.0, 10.0)]);
        assert!(
            (curve.db_at(1000.0) - 5.0).abs() < 1e-3,
            "got {}",
            curve.db_at(1000.0)
        );
        // A linear interpolator would read about 0.9 dB here.
        assert!(curve.db_at(1000.0) > 4.0);
    }

    #[test]
    fn outside_the_range_the_endpoints_are_held() {
        let curve = curve();
        assert!(
            (curve.db_at(1.0) - -2.0).abs() < 1e-4,
            "below the first point"
        );
        assert!(
            (curve.db_at(96_000.0) - 3.0).abs() < 1e-4,
            "above the last point"
        );
    }

    #[test]
    fn points_are_sorted_and_deduplicated() {
        let curve = ResponseCurve::new(vec![
            (1000.0, 1.0),
            (20.0, -1.0),
            (1000.0, 9.0),
            (100.0, 0.0),
        ]);
        let points = curve.points();
        assert_eq!(points.len(), 3);
        for pair in points.windows(2) {
            assert!(pair[0].0 < pair[1].0, "not sorted: {points:?}");
        }
    }

    #[test]
    fn non_positive_and_non_finite_points_are_dropped() {
        let curve = ResponseCurve::new(vec![
            (0.0, 5.0),
            (-100.0, 5.0),
            (f32::NAN, 1.0),
            (100.0, f32::INFINITY),
            (1000.0, 0.5),
        ]);
        assert_eq!(curve.points().len(), 1);
        assert!((curve.db_at(1000.0) - 0.5).abs() < 1e-4);
    }

    #[test]
    fn parses_a_typical_vendor_file() {
        let text = "\
* Microphone calibration
* Serial 12345
\x20
20.0    -2.00   0.0
100.0   -0.50   0.0
1000.0   0.00   0.0
10000.0  1.50   0.0
";
        let curve = ResponseCurve::parse(text);
        assert_eq!(curve.points().len(), 4);
        assert!((curve.db_at(1000.0)).abs() < 1e-4);
        assert!((curve.db_at(20.0) - -2.0).abs() < 1e-4);
    }

    #[test]
    fn parses_comma_separated_and_hash_comments() {
        let curve = ResponseCurve::parse("# header\n20,-1.5\n1000,0\n20000,2.5\n");
        assert_eq!(curve.points().len(), 3);
        assert!((curve.db_at(20_000.0) - 2.5).abs() < 1e-4);
    }

    /// Vendor files carry stray junk. Skipping bad lines beats rejecting a file
    /// that is 99% usable.
    #[test]
    fn unparseable_lines_are_skipped_not_fatal() {
        let curve = ResponseCurve::parse(
            "Sens Factor =-.4dB, SERNO: 1234\n\
             not numbers at all\n\
             20.0 -1.0\n\
             1000.0 0.0\n\
             trailing junk\n",
        );
        assert_eq!(curve.points().len(), 2);
    }

    #[test]
    fn an_empty_file_yields_a_flat_curve() {
        assert!(ResponseCurve::parse("").is_flat());
        assert!(ResponseCurve::parse("* only comments\n# nothing else\n").is_flat());
    }

    #[test]
    fn display_summarises_the_range() {
        assert_eq!(format!("{}", ResponseCurve::flat()), "flat");
        assert_eq!(format!("{}", curve()), "5 points, 20-20000 Hz");
    }

    #[test]
    fn a_single_point_curve_is_a_constant_offset() {
        let curve = ResponseCurve::new(vec![(1000.0, 2.5)]);
        for hz in [10.0, 1000.0, 20_000.0] {
            assert!((curve.db_at(hz) - 2.5).abs() < 1e-4);
        }
    }
}
