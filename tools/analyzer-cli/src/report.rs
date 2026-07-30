//! Rendering a spectrum as text.
//!
//! Shared by the offline and live paths so both emit byte-identical formats.
//! The layout deliberately mirrors REW's text import — `#` comments then
//! whitespace-separated columns — since that is the cheap interoperability
//! bridge in both directions.

use std::fmt::Write as _;

use analyzer_dsp::{Averaging, Overlap, WindowKind};
use analyzer_engine::SpectrumFrame;

/// Everything the header needs that the frame itself does not carry.
#[derive(Debug, Clone)]
pub(crate) struct Meta {
    /// Where the audio came from, for the header.
    pub(crate) source: String,
    /// Channels the source delivered.
    pub(crate) channels: usize,
    /// Which one was analysed.
    pub(crate) channel: usize,
    /// Rate the analysis ran at.
    pub(crate) sample_rate: f64,
    /// Analysis window.
    pub(crate) window: WindowKind,
    /// Frame overlap.
    pub(crate) overlap: Overlap,
    /// Averaging mode.
    pub(crate) averaging: Averaging,
    /// Effective noise bandwidth of one bin.
    pub(crate) enbw_hz: f32,
    /// FFT size.
    pub(crate) fft_size: usize,
    /// Samples between frames.
    pub(crate) hop: usize,
}

/// Render `frame` as text.
///
/// `min_db` omits bins below a level; `peak_only` emits just the loudest bin.
pub(crate) fn render(
    frame: &SpectrumFrame,
    meta: &Meta,
    min_db: Option<f32>,
    peak_only: bool,
) -> String {
    let mut out = String::with_capacity(frame.bins.len() * 24 + 640);

    let _ = writeln!(out, "# analyzer-cli spectrum");
    let _ = writeln!(out, "# source: {}", meta.source);
    let _ = writeln!(out, "# sample rate: {} Hz", meta.sample_rate);
    let _ = writeln!(
        out,
        "# channels: {} (analysed channel {})",
        meta.channels, meta.channel
    );
    let _ = writeln!(out, "# fft size: {}", meta.fft_size);
    let _ = writeln!(
        out,
        "# window: {:?}, ENBW {:.4} Hz",
        meta.window, meta.enbw_hz
    );
    let _ = writeln!(
        out,
        "# overlap: {:.1}%, hop {} frames",
        meta.overlap.fraction() * 100.0,
        meta.hop
    );
    let _ = writeln!(out, "# averaging: {:?}", meta.averaging);
    let _ = writeln!(out, "# frames averaged: {}", frame.frames_averaged);
    let _ = writeln!(out, "# bin spacing: {:.6} Hz", frame.bin_spacing_hz);
    if frame.overruns > 0 {
        let _ = writeln!(out, "# WARNING: {} dropped block(s)", frame.overruns);
    }
    let _ = writeln!(out, "# level reference: 0 dBFS = full-scale sine");
    let _ = writeln!(out, "# frequency_hz\tlevel_db");

    if peak_only {
        if let Some((bin, level)) = frame
            .bins
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.total_cmp(b.1))
        {
            let _ = writeln!(out, "{:.6}\t{level:.4}", frame.bin_frequency(bin));
        }
        return out;
    }

    for (bin, level) in frame.bins.iter().enumerate() {
        if min_db.is_some_and(|floor| *level < floor) {
            continue;
        }
        let _ = writeln!(out, "{:.6}\t{level:.4}", frame.bin_frequency(bin));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn meta() -> Meta {
        Meta {
            source: "test".into(),
            channels: 1,
            channel: 0,
            sample_rate: 48_000.0,
            window: WindowKind::Hann,
            overlap: Overlap::ThreeQuarters,
            averaging: Averaging::Infinite,
            enbw_hz: 17.6,
            fft_size: 8,
            hop: 2,
        }
    }

    fn frame() -> SpectrumFrame {
        SpectrumFrame {
            sequence: 1,
            bins: vec![-90.0, -6.0, -70.0, -80.0, -95.0],
            bin_spacing_hz: 100.0,
            sample_rate: 48_000.0,
            frames_averaged: 12,
            overruns: 0,
            ..SpectrumFrame::default()
        }
    }

    #[test]
    fn emits_one_row_per_bin_with_a_header() {
        let text = render(&frame(), &meta(), None, false);
        assert!(text.contains("# sample rate: 48000 Hz"));
        assert!(text.contains("# frames averaged: 12"));
        let rows = text.lines().filter(|l| !l.starts_with('#')).count();
        assert_eq!(rows, 5);
    }

    #[test]
    fn peak_only_emits_the_loudest_bin() {
        let text = render(&frame(), &meta(), None, true);
        let rows: Vec<&str> = text.lines().filter(|l| !l.starts_with('#')).collect();
        assert_eq!(rows.len(), 1);
        assert!(rows[0].starts_with("100.000000"), "row was {:?}", rows[0]);
    }

    #[test]
    fn min_db_drops_quiet_bins() {
        let text = render(&frame(), &meta(), Some(-75.0), false);
        let rows = text.lines().filter(|l| !l.starts_with('#')).count();
        assert_eq!(rows, 2, "only -6 and -70 clear a -75 floor");
    }

    #[test]
    fn overruns_are_surfaced_in_the_header() {
        let mut frame = frame();
        frame.overruns = 3;
        let text = render(&frame, &meta(), None, true);
        assert!(text.contains("WARNING: 3 dropped block(s)"));
    }

    #[test]
    fn a_clean_run_carries_no_warning() {
        assert!(!render(&frame(), &meta(), None, true).contains("WARNING"));
    }
}
