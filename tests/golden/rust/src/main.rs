//! Writes the golden fixtures that `analyzer-cli` has no flag for: saved
//! measurements (`.anlz`), REW text export, filter export for REW / Equalizer
//! APO / miniDSP, the settings file, and the one input WAV the CLI cannot make
//! (a synthetic "room" response for `--measure`).
//!
//! Every input is hard-coded and deliberately exactly representable (dyadic
//! fractions), so a C++ implementation can rebuild the same measurement without
//! depending on libm. Nothing here reads a clock or a random source.
//!
//! Usage: golden-fixtures <fixtures-dir>   (writes into <dir>/model and <dir>/wav)

use std::fs;
use std::path::{Path, PathBuf};

use analyzer_dsp::{FilterBand, FilterKind, Signal};
use analyzer_model::export::to_text as rew_text;
use analyzer_model::filter_export::{self, FilterFormat};
use analyzer_model::measurement::{Complex64, Measurement, MeasurementData, MeasurementId};
use analyzer_model::settings::{AveragingChoice, Settings, WindowChoice};
use analyzer_model::{SampleDepth, format, wav};

fn main() {
    let root = PathBuf::from(
        std::env::args()
            .nth(1)
            .expect("usage: golden-fixtures <dir>"),
    );
    let model = root.join("model");
    let wavs = root.join("wav");
    fs::create_dir_all(&model).expect("create model dir");
    fs::create_dir_all(&wavs).expect("create wav dir");

    measurements(&model);
    filters(&model);
    settings(&model);
    room_response(&wavs);
}

fn put(dir: &Path, name: &str, bytes: impl AsRef<[u8]>) {
    fs::write(dir.join(name), bytes).unwrap_or_else(|e| panic!("writing {name}: {e}"));
}

/// Exactly representable ramp in [-0.5, 0.5): `((i * mul) % 64) / 64 - 0.5`.
fn ramp(i: usize, mul: usize) -> f64 {
    ((i * mul) % 64) as f64 / 64.0 - 0.5
}

/// One measurement of each kind, saved as `.anlz` and exported as REW text.
fn measurements(dir: &Path) {
    // Every reference set, captured_at pinned, free text with a newline and a
    // backslash so the header escaping is covered.
    let mut impulse = Measurement::new(
        MeasurementId(7),
        "Living room, left\nsecond line",
        48_000.0,
        MeasurementData::ImpulseResponse {
            samples: (0..256).map(|i| ramp(i, 37)).collect(),
            time_zero_samples: 12.5,
        },
    );
    impulse.notes = "mic at 1 m\\on axis\nline two".into();
    impulse.captured_at = 1_700_000_000;
    impulse.channels = 2;
    impulse.references.spl_offset_db = Some(134.25);
    impulse.references.full_scale_input_volts = Some(1.5);
    impulse.references.full_scale_output_volts = Some(2.0);
    impulse.references.reference_resistance_ohms = Some(10.0);
    impulse.references.propagation_delay_seconds = Some(0.0078125);

    // Complex spectrum, SPL calibrated: levels are bin magnitude plus offset.
    // Bin 0 is zero magnitude so the -200 dB floor is covered.
    let mut spectrum = Measurement::new(
        MeasurementId(8),
        "Spectrum",
        48_000.0,
        MeasurementData::Spectrum {
            bins: (0..64)
                .map(|k| {
                    if k == 0 {
                        Complex64::new(0.0, 0.0)
                    } else {
                        Complex64::new(ramp(k, 5) * 2.0, ramp(k, 11))
                    }
                })
                .collect(),
            bin_spacing_hz: 750.0,
        },
    );
    spectrum.captured_at = 1_700_000_001;
    spectrum.references.spl_offset_db = Some(94.0);

    // Magnitude-only, no references at all: the "not calibrated" branch, and
    // the proof None is written as an absent line rather than zero.
    let power = Measurement::new(
        MeasurementId(9),
        "Power spectrum",
        44_100.0,
        MeasurementData::PowerSpectrum {
            magnitude_db: (0..32).map(|k| -20.0 - ramp(k, 3) * 40.0).collect(),
            bin_spacing_hz: 1378.125,
        },
    );

    // Two-channel transfer function with coherence; propagation delay set so the
    // export's "delay removed" header line is covered.
    let mut transfer = Measurement::new(
        MeasurementId(10),
        "Transfer",
        48_000.0,
        MeasurementData::TransferFunction {
            bins: (0..32)
                .map(|k| Complex64::new(1.0 + ramp(k, 7), ramp(k, 13)))
                .collect(),
            coherence: (0..32).map(|k| 0.5 + (k % 8) as f64 / 16.0).collect(),
            bin_spacing_hz: 93.75,
        },
    );
    transfer.captured_at = 1_700_000_002;
    transfer.channels = 2;
    transfer.references.propagation_delay_seconds = Some(0.005);
    transfer.references.spl_offset_db = Some(0.0); // measured as needing none

    for (stem, m) in [
        ("measurement_impulse_response", &impulse),
        ("measurement_spectrum", &spectrum),
        ("measurement_power_spectrum", &power),
        ("measurement_transfer_function", &transfer),
    ] {
        let bytes = format::write(m);
        // Round trip is part of the contract: what we wrote must read back equal.
        assert_eq!(&format::read(&bytes).expect("read back"), m, "{stem}");
        put(dir, &format!("{stem}.anlz"), bytes);
        put(dir, &format!("{stem}.rew.txt"), rew_text(m));
    }
}

/// Ten bands covering every shape, a disabled slot, and a transparent band.
fn filter_bands() -> Vec<FilterBand> {
    let band = |kind, hz, gain_db, q, enabled| FilterBand {
        kind,
        hz,
        gain_db,
        q,
        enabled,
    };
    vec![
        band(FilterKind::Peaking, 63.0, -5.5, 4.0, true),
        band(FilterKind::LowShelf, 100.0, 3.0, 0.707, true),
        band(FilterKind::HighShelf, 8000.0, -2.5, 0.5, true),
        band(FilterKind::HighPass, 20.0, 0.0, 0.707, true),
        band(FilterKind::LowPass, 18000.0, 0.0, 0.707, true),
        band(FilterKind::BandPass, 1000.0, 0.0, 2.0, true),
        band(FilterKind::Notch, 50.0, 0.0, 10.0, true),
        band(FilterKind::AllPass, 500.0, 0.0, 1.0, true),
        band(FilterKind::Peaking, 250.0, 4.0, 1.5, false), // present but OFF
        band(FilterKind::Peaking, 1000.0, 0.0, 1.5, true), // transparent
    ]
}

/// Every exporter, with bands and with none, and miniDSP at two rates.
fn filters(dir: &Path) {
    let bands = filter_bands();
    let preamp = -6.25_f32;

    put(
        dir,
        "filters_all_kinds.rew.txt",
        filter_export::to_text(FilterFormat::Rew, &bands, preamp, 48_000.0),
    );
    put(
        dir,
        "filters_all_kinds.apo.txt",
        filter_export::to_text(FilterFormat::EqualizerApo, &bands, preamp, 48_000.0),
    );
    put(
        dir,
        "filters_all_kinds.minidsp_48k.txt",
        filter_export::to_text(FilterFormat::MiniDsp, &bands, preamp, 48_000.0),
    );
    put(
        dir,
        "filters_all_kinds.minidsp_96k.txt",
        filter_export::to_text(FilterFormat::MiniDsp, &bands, preamp, 96_000.0),
    );

    // No bands: miniDSP must still carry the trim, as a gain-only section.
    put(
        dir,
        "filters_empty.rew.txt",
        filter_export::to_text(FilterFormat::Rew, &[], -3.0, 48_000.0),
    );
    put(
        dir,
        "filters_empty.apo.txt",
        filter_export::to_text(FilterFormat::EqualizerApo, &[], -3.0, 48_000.0),
    );
    put(
        dir,
        "filters_empty.minidsp_48k.txt",
        filter_export::to_text(FilterFormat::MiniDsp, &[], -3.0, 48_000.0),
    );
}

/// Default settings, a fully custom set, and a hand-edited file that the
/// infallible parser has to repair.
fn settings(dir: &Path) {
    put(dir, "settings_default.cfg", Settings::default().to_text());

    let custom = Settings {
        fft_size: 16_384,
        window: WindowChoice::FlatTop,
        averaging: AveragingChoice::PeakHold,
        start_on_launch: false,
        min_hz: 10.0,
        max_hz: 24_000.0,
        min_db: -100.0,
        max_db: 10.0,
        level_grid_step: 10.0,
        spl_offset_db: Some(94.3),
        mic_cal_path: Some("/Users/test/cal/umik-1.txt".into()),
    };
    put(dir, "settings_custom.cfg", custom.to_text());

    // Input: unknown key, comment, bad number, unsupported FFT size, inverted
    // axis, unknown window, a zero offset (kept: zero is not "none"), and an
    // empty path (dropped).
    let hand_edited = "\
ANLZCFG1
# edited by hand
future_key: 42
fft_size: 3000
window: kaiser
averaging: infinite
start_on_launch: maybe
min_hz: 5000
max_hz: 100
min_db: -80
max_db: -90
level_grid_step: 0
spl_offset_db: 0
mic_cal_path:
";
    put(dir, "settings_hand_edited.in.cfg", hand_edited);
    put(
        dir,
        "settings_hand_edited.out.cfg",
        Settings::from_text(hand_edited).to_text(),
    );
}

/// The synthetic response `--measure` is fed: the 1 s sweep the CLI writes
/// (`--generate sweep --seconds 1 --depth f32`, same parameters, same seed)
/// played through four discrete arrivals, accumulated in a fixed order in f32.
fn room_response(dir: &Path) {
    let rate = 48_000.0_f32;
    let stimulus = wav::render(
        Signal::Sweep {
            start_hz: 20.0,
            end_hz: 20_000.0,
            seconds: 1.0,
            amplitude: 0.5,
            repeat: false,
        },
        rate,
        1.0,
    )
    .expect("render sweep");

    // (delay in samples, gain): direct at 5 ms, then three reflections.
    let arrivals: [(usize, f32); 4] = [(240, 0.5), (336, 0.3), (768, -0.2), (1440, 0.1)];
    let mut response = vec![0.0_f32; stimulus.len() + 1440 + 1];
    for (delay, gain) in arrivals {
        for (n, s) in stimulus.iter().enumerate() {
            response[n + delay] += s * gain;
        }
    }
    wav::write(
        &dir.join("response_room_f32.wav"),
        &response,
        rate,
        SampleDepth::Float32,
    )
    .expect("write response");
}
