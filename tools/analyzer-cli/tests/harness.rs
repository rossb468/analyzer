//! End-to-end tests for the harness.
//!
//! These drive the built binary as a subprocess rather than calling into a
//! library, so they exercise the real thing: argument parsing, the audio backend,
//! the allocation trap around the callback, the capture ring, and the analyzer.
//! If the callback ever allocates, the child aborts and every test here fails —
//! which is the point.

use std::process::{Command, Output};

const BIN: &str = env!("CARGO_BIN_EXE_analyzer-cli");

/// Level of a full-scale-referenced sine at amplitude 0.5: 20·log₁₀(0.5).
const HALF_SCALE_DB: f32 = -6.0206;

/// A frequency that lands exactly on bin 85 at 48 kHz with a 4096-point FFT.
/// On-bin means no scalloping loss, so every window must agree.
const ON_BIN_HZ: &str = "996.09375";

fn run(args: &[&str]) -> Output {
    Command::new(BIN)
        .args(args)
        .output()
        .expect("failed to execute harness")
}

/// Parse the single `frequency\tlevel` row produced by `--peak`.
fn peak(args: &[&str]) -> (f32, f32) {
    let output = run(args);
    assert!(
        output.status.success(),
        "harness failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    let stdout = String::from_utf8_lossy(&output.stdout);
    let row = stdout
        .lines()
        .find(|line| !line.starts_with('#') && !line.trim().is_empty())
        .unwrap_or_else(|| panic!("no data row in output:\n{stdout}"));
    let mut columns = row.split('\t');
    let frequency = columns
        .next()
        .and_then(|c| c.parse().ok())
        .unwrap_or_else(|| panic!("bad frequency column in {row:?}"));
    let level = columns
        .next()
        .and_then(|c| c.parse().ok())
        .unwrap_or_else(|| panic!("bad level column in {row:?}"));
    (frequency, level)
}

/// The load-bearing test. An on-bin sine at amplitude 0.5 must read -6.02 dBFS
/// through every window, which only holds if the FFT scaling, the window
/// correction factors and the dBFS reference are all correct together.
#[test]
fn on_bin_sine_reads_correct_level_through_every_window() {
    for window in ["rect", "hann", "bh", "flattop", "tukey"] {
        let (frequency, level) = peak(&[
            "--sine",
            ON_BIN_HZ,
            "--amplitude",
            "0.5",
            "--seconds",
            "1",
            "--window",
            window,
            "--peak",
        ]);
        assert!(
            (frequency - 996.093_75).abs() < 0.001,
            "{window}: peak at {frequency} Hz"
        );
        assert!(
            (level - HALF_SCALE_DB).abs() < 0.02,
            "{window}: read {level} dBFS, expected {HALF_SCALE_DB}"
        );
    }
}

/// Off-bin tones lose level to scalloping, and how much is the whole reason
/// several windows exist. Flat-top is nearly immune, which is why it is the
/// calibration window; Hann is not.
#[test]
fn flat_top_resists_scalloping_loss_far_better_than_hann() {
    let off_bin = ["--sine", "1000", "--amplitude", "0.5", "--seconds", "1"];

    let mut hann = off_bin.to_vec();
    hann.extend(["--window", "hann", "--peak"]);
    let (_, hann_db) = peak(&hann);

    let mut flat = off_bin.to_vec();
    flat.extend(["--window", "flattop", "--peak"]);
    let (_, flat_db) = peak(&flat);

    let hann_loss = (HALF_SCALE_DB - hann_db).abs();
    let flat_loss = (HALF_SCALE_DB - flat_db).abs();

    // Flat-top's published scalloping loss is ~0.01 dB; Hann's worst case is
    // 1.42 dB. Assert the ordering and that flat-top is genuinely accurate.
    assert!(
        flat_loss < 0.05,
        "flat-top should be near-exact off-bin, lost {flat_loss} dB"
    );
    assert!(
        hann_loss > flat_loss * 4.0,
        "hann should lose clearly more than flat-top: {hann_loss} vs {flat_loss}"
    );
    assert!(
        hann_loss < 1.5,
        "hann loss should stay within its 1.42 dB worst case, got {hann_loss}"
    );
}

#[test]
fn amplitude_changes_level_by_the_expected_decibels() {
    let level_at = |amplitude: &str| {
        peak(&[
            "--sine",
            ON_BIN_HZ,
            "--amplitude",
            amplitude,
            "--seconds",
            "1",
            "--window",
            "flattop",
            "--peak",
        ])
        .1
    };

    let full = level_at("1.0");
    let half = level_at("0.5");
    let quarter = level_at("0.25");

    assert!(full.abs() < 0.02, "full scale should be 0 dBFS, got {full}");
    // Halving amplitude is -6.02 dB, every time.
    assert!((full - half - 6.0206).abs() < 0.03, "{full} -> {half}");
    assert!(
        (half - quarter - 6.0206).abs() < 0.03,
        "{half} -> {quarter}"
    );
}

/// The block size is the audio driver's business, not the analyzer's, so it must
/// not change the answer.
#[test]
fn callback_block_size_does_not_affect_the_result() {
    let level_at = |block: &str| {
        peak(&[
            "--sine",
            ON_BIN_HZ,
            "--amplitude",
            "0.5",
            "--seconds",
            "1",
            "--window",
            "hann",
            "--block",
            block,
            "--peak",
        ])
        .1
    };

    let reference = level_at("128");
    for block in ["1", "64", "512", "4096", "5000"] {
        let level = level_at(block);
        assert!(
            (level - reference).abs() < 1e-3,
            "block {block} gave {level}, reference {reference}"
        );
    }
}

#[test]
fn overlap_changes_frame_count_but_not_level() {
    let mut levels = Vec::new();
    for overlap in ["0", "50", "75", "87"] {
        let (_, level) = peak(&[
            "--sine",
            ON_BIN_HZ,
            "--amplitude",
            "0.5",
            "--seconds",
            "1",
            "--window",
            "hann",
            "--overlap",
            overlap,
            "--peak",
        ]);
        levels.push(level);
    }
    for level in &levels {
        assert!(
            (level - HALF_SCALE_DB).abs() < 0.02,
            "overlap changed the level: {levels:?}"
        );
    }
}

#[test]
fn full_output_covers_the_whole_spectrum_with_metadata() {
    let output = run(&["--sine", "1000", "--seconds", "1", "--fft", "1024"]);
    assert!(output.status.success());
    let stdout = String::from_utf8_lossy(&output.stdout);

    assert!(stdout.contains("# sample rate: 48000 Hz"));
    assert!(stdout.contains("# fft size: 1024"));
    assert!(stdout.contains("# level reference: 0 dBFS = full-scale sine"));

    let rows = stdout
        .lines()
        .filter(|line| !line.starts_with('#') && !line.trim().is_empty())
        .count();
    // One row per bin: size / 2 + 1.
    assert_eq!(rows, 513, "expected one row per bin");
}

#[test]
fn min_db_filters_quiet_bins() {
    let all = run(&["--sine", ON_BIN_HZ, "--seconds", "1", "--fft", "1024"]);
    let filtered = run(&[
        "--sine",
        ON_BIN_HZ,
        "--seconds",
        "1",
        "--fft",
        "1024",
        "--min-db",
        "-60",
    ]);

    let count = |output: &Output| {
        String::from_utf8_lossy(&output.stdout)
            .lines()
            .filter(|line| !line.starts_with('#') && !line.trim().is_empty())
            .count()
    };

    assert!(
        count(&filtered) < count(&all),
        "--min-db should drop rows: {} vs {}",
        count(&filtered),
        count(&all)
    );
    assert!(count(&filtered) > 0, "should not drop the peak");
}

#[test]
fn help_succeeds_and_describes_usage() {
    let output = run(&["--help"]);
    assert!(output.status.success());
    let stdout = String::from_utf8_lossy(&output.stdout);
    assert!(stdout.contains("USAGE:"));
    assert!(stdout.contains("--window"));
}

#[test]
fn no_input_is_an_error() {
    let output = run(&[]);
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("no input"));
}

#[test]
fn unknown_option_is_an_error() {
    let output = run(&["--nonsense"]);
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("unknown option"));
}

#[test]
fn odd_fft_size_is_rejected() {
    let output = run(&["--sine", "1000", "--fft", "1023"]);
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("must be even"));
}

#[test]
fn source_shorter_than_the_fft_is_rejected() {
    // 100 frames of material against a 4096-point FFT.
    let output = run(&["--sine", "1000", "--seconds", "0.002", "--fft", "4096"]);
    assert!(!output.status.success());
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.contains("need at least"), "stderr: {stderr}");
}

/// The input modes are mutually exclusive; picking two must be refused rather
/// than one silently winning.
#[test]
fn combining_input_modes_is_an_error() {
    for args in [
        vec!["--sine", "1000", "some.wav"],
        vec!["--live", "1", "--sine", "1000"],
        vec!["--list-devices", "--sine", "1000"],
    ] {
        let output = run(&args);
        assert!(!output.status.success(), "{args:?} should have failed");
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(stderr.contains("choose one of"), "{args:?} gave: {stderr}");
    }
}

/// Enumerating devices must not need capture permission. macOS prompts for the
/// microphone on the first *capture*, and a device list that tripped that
/// prompt would make the harness unusable for the one thing it is best at:
/// finding out what the machine can see before anything is recorded.
#[cfg(target_os = "macos")]
#[test]
fn list_devices_succeeds_without_capture_permission() {
    let output = run(&["--list-devices"]);
    assert!(output.status.success());
    assert!(String::from_utf8_lossy(&output.stdout).contains("Audio devices"));
}

/// Where there is no backend, the flag still parses and fails with an
/// explanation rather than vanishing from the interface or panicking.
#[cfg(not(target_os = "macos"))]
#[test]
fn list_devices_explains_that_there_is_no_backend() {
    let output = run(&["--list-devices"]);
    assert!(!output.status.success(), "should fail, not pretend");
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        stderr.contains("platform audio backend"),
        "should say what is missing, got: {stderr}"
    );
    assert!(!stderr.contains("panicked"), "should be a clean error");
}

#[test]
fn missing_wav_file_is_reported_not_panicked() {
    let output = run(&["definitely-not-here.wav"]);
    assert!(!output.status.success());
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.contains("opening"), "stderr: {stderr}");
    assert!(!stderr.contains("panicked"), "should be a clean error");
}

#[test]
fn channel_out_of_range_is_reported() {
    let output = run(&["--sine", "1000", "--seconds", "1", "--channel", "3"]);
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("channel 3"));
}

/// Reads a real WAV off disk, which is the path the REW parity comparison will
/// use. Also checks that integer PCM is scaled correctly rather than being off by
/// a factor of two.
#[test]
fn analyses_a_wav_file_from_disk() {
    let dir = std::env::temp_dir().join("analyzer-cli-tests");
    std::fs::create_dir_all(&dir).expect("create temp dir");
    let path = dir.join("on-bin-sine-16bit.wav");

    let spec = hound::WavSpec {
        channels: 2,
        sample_rate: 48_000,
        bits_per_sample: 16,
        sample_format: hound::SampleFormat::Int,
    };
    let mut writer = hound::WavWriter::create(&path, spec).expect("create wav");
    for n in 0..48_000_u32 {
        let phase = std::f64::consts::TAU * 996.093_75 * f64::from(n) / 48_000.0;
        // Left carries the tone at half scale; right is silent, so a channel
        // mix-up cannot pass unnoticed.
        let sample = (0.5 * phase.sin() * 32_767.0) as i16;
        writer.write_sample(sample).expect("write left");
        writer.write_sample(0_i16).expect("write right");
    }
    writer.finalize().expect("finalize wav");

    let file = path.to_string_lossy().to_string();
    let (frequency, level) = peak(&[&file, "--window", "flattop", "--channel", "0", "--peak"]);
    assert!(
        (frequency - 996.093_75).abs() < 0.001,
        "peak at {frequency}"
    );
    assert!(
        (level - HALF_SCALE_DB).abs() < 0.05,
        "16-bit PCM scaling wrong: {level} dBFS, expected {HALF_SCALE_DB}"
    );

    // The silent channel must read at the floor, not pick up the other one.
    let (_, silent) = peak(&[&file, "--window", "flattop", "--channel", "1", "--peak"]);
    assert!(silent < -80.0, "silent channel read {silent} dBFS");

    let _ = std::fs::remove_file(&path);
}

/// The swept-measurement path through the CLI, not just the module behind it.
#[test]
fn measure_demo_runs_end_to_end() {
    let output = run(&["--measure-demo"]);
    assert!(
        output.status.success(),
        "stderr: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    let stdout = String::from_utf8_lossy(&output.stdout);

    assert!(stdout.contains("# constructed:"));
    assert!(stdout.contains("# measured:"));
    assert!(stdout.contains("direct arrival"));
    assert!(stdout.contains("T30"));
    assert!(stdout.contains("gated response"));
}

/// The gate flag has to reach the measurement, not just parse.
#[test]
fn the_gate_flag_changes_the_reported_limit() {
    let limit = |ms: &str| -> f32 {
        let output = run(&["--measure-demo", "--gate", ms]);
        assert!(output.status.success());
        String::from_utf8_lossy(&output.stdout)
            .lines()
            .find(|line| line.contains("valid above"))
            .and_then(|line| line.split_whitespace().rev().nth(1)?.parse().ok())
            .unwrap_or(f32::NAN)
    };
    assert!(
        limit("40") < limit("5"),
        "a wider gate should reach lower in frequency"
    );
}

#[test]
fn measure_needs_two_paths() {
    let output = run(&["--measure", "only-one.wav"]);
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("needs a value"));
}

#[test]
fn measure_reports_a_missing_file_cleanly() {
    let output = run(&["--measure", "nope-a.wav", "nope-b.wav"]);
    assert!(!output.status.success());
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.contains("opening"), "stderr: {stderr}");
    assert!(!stderr.contains("panicked"));
}

#[test]
fn bench_runs_and_reports_every_stage() {
    let output = run(&["--bench", "0.2"]);
    assert!(output.status.success());
    let stdout = String::from_utf8_lossy(&output.stdout);
    assert!(stdout.contains("spectrum analysis"));
    assert!(stdout.contains("transfer function"));
    assert!(stdout.contains("ring soak"));
    assert!(stdout.contains("overruns:"));
}
