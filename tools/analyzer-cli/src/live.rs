//! Live capture from real hardware.
//!
//! This exists so the audio backend can be proven from a terminal, before any
//! user interface is written. When the GUI later fails to show a spectrum, that
//! failure is unambiguously the GUI's — the chain from converter to analysis is
//! already known good.
//!
//! # Microphone permission
//!
//! macOS gates capture behind TCC. A bare binary run from a terminal inherits
//! the terminal's permission and the first attempt prompts the user. If
//! permission is denied, capture still "succeeds" and simply delivers digital
//! silence forever, so this module watches for that and says so rather than
//! leaving the user staring at an empty meter.

use std::io::{self, Write};
use std::time::{Duration, Instant};

use analyzer_audio::{
    AudioBackend, AudioBuffers, AudioStream, CoreAudioBackend, DeviceId, StreamConfig,
};
use analyzer_dsp::SpectrumConfig;
use analyzer_engine::{Engine, EngineConfig, SpectrumFrame, rt_section};

/// A level this low over the whole run means nothing is arriving.
///
/// Real microphones always produce some self-noise, so a spectrum pinned at the
/// floor is a permission or routing problem, not a quiet room.
const SILENCE_DB: f32 = -160.0;

/// Print every device the backend can see.
pub(crate) fn list_devices() -> Result<String, String> {
    let backend = CoreAudioBackend::new();
    let devices = backend
        .devices()
        .map_err(|e| format!("enumerating devices: {e}"))?;

    let mut out = String::new();
    out.push_str("Audio devices\n");
    for device in &devices {
        let mut tags = Vec::new();
        if device.is_default_input {
            tags.push("default input");
        }
        if device.is_default_output {
            tags.push("default output");
        }
        let tag = if tags.is_empty() {
            String::new()
        } else {
            format!("  [{}]", tags.join(", "))
        };

        out.push_str(&format!("\n  {}{tag}\n", device.name));
        out.push_str(&format!("    uid:      {}\n", device.id));
        out.push_str(&format!(
            "    channels: {} in, {} out\n",
            device.input_channels, device.output_channels
        ));
        out.push_str(&format!(
            "    rate:     {} Hz\n",
            device.default_sample_rate
        ));
        if !device.supported_sample_rates.is_empty() {
            out.push_str(&format!(
                "    supports: {}\n",
                device
                    .supported_sample_rates
                    .iter()
                    .map(|r| format!("{r:.0}"))
                    .collect::<Vec<_>>()
                    .join(", ")
            ));
        }
    }
    Ok(out)
}

/// Settings for a live capture run.
#[derive(Debug, Clone)]
pub(crate) struct LiveOptions {
    /// Device UID, or `None` for the system default input.
    pub(crate) device: Option<String>,
    /// How long to capture.
    pub(crate) seconds: f64,
    /// Channel of the device to analyse.
    pub(crate) channel: usize,
    /// Requested callback size.
    pub(crate) block: u32,
    /// Spectrum settings; `sample_rate` is overwritten with what the device grants.
    pub(crate) spectrum: SpectrumConfig,
    /// Print a running meter to stderr while capturing.
    pub(crate) meter: bool,
}

/// Capture for a while and return the final spectrum.
///
/// Rendering belongs to the caller so the live and offline paths share one
/// formatter and cannot drift apart.
///
/// Runs the actual work on a thread behind a deadline. A refused microphone
/// permission does not fail promptly - CoreAudio's server retries
/// `StartAndWaitForState` on a 30 second timeout, so a denied stream stalls for
/// minutes and looks like a hang. The deadline turns that into an explanation.
pub(crate) fn capture(options: &LiveOptions) -> Result<SpectrumFrame, String> {
    let (tx, rx) = std::sync::mpsc::channel();
    let owned = options.clone();
    std::thread::Builder::new()
        .name("analyzer-capture".into())
        .spawn(move || {
            let _ = tx.send(capture_inner(&owned));
        })
        .map_err(|e| format!("spawning the capture thread: {e}"))?;

    // Enough slack for device negotiation, but far short of CoreAudio's retries.
    let budget = Duration::from_secs_f64(options.seconds + 15.0);
    match rx.recv_timeout(budget) {
        Ok(result) => result,
        Err(_) => Err(format!(
            "capture did not start within {:.0}s.\n\
             CoreAudio stalls like this when microphone access is refused, and macOS \
             will not raise a permission prompt for a process launched in a \
             non-interactive background session - it refuses silently.\n\
             Run this once from a foreground Terminal window and allow the prompt, or \
             grant access under System Settings > Privacy & Security > Microphone.",
            budget.as_secs_f64()
        )),
    }
}

fn capture_inner(options: &LiveOptions) -> Result<SpectrumFrame, String> {
    let mut backend = CoreAudioBackend::new();

    let device = match &options.device {
        Some(uid) => backend
            .devices()
            .map_err(|e| format!("enumerating devices: {e}"))?
            .into_iter()
            .find(|d| d.id.as_str() == uid)
            .ok_or_else(|| format!("no device with uid '{uid}' (try --list-devices)"))?,
        None => backend
            .default_input()
            .map_err(|e| format!("finding the default input: {e}"))?
            .ok_or("no default input device")?,
    };

    if device.input_channels == 0 {
        return Err(format!("{} has no input channels", device.name));
    }
    if options.channel >= device.input_channels as usize {
        return Err(format!(
            "channel {} requested but {} has {}",
            options.channel, device.name, device.input_channels
        ));
    }

    // Capture every channel and pick one for analysis, rather than asking the
    // device for a single channel. Interleaved capture of the whole device is
    // what a two-channel transfer function will need later.
    let channels = device.input_channels as usize;
    let rate = device.default_sample_rate;

    let mut spectrum = options.spectrum;
    spectrum.sample_rate = rate as f32;

    let (mut sink, mut engine) = Engine::start(EngineConfig {
        channels,
        analysis_channel: options.channel,
        spectrum,
        ring_capacity_frames: 16_384,
    });

    let config = StreamConfig {
        input: Some(DeviceId::new(device.id.as_str())),
        output: None,
        sample_rate: rate,
        buffer_frames: options.block,
        input_channels: (0..channels as u32).collect(),
        output_channels: Vec::new(),
    };

    let mut stream = backend
        .open_input(
            &config,
            // The real-time path, guarded. If this ever allocates the process
            // aborts rather than glitching.
            Box::new(move |buffers: &mut AudioBuffers<'_>| {
                rt_section(|| {
                    sink.write_interleaved(buffers.input());
                });
            }),
        )
        .map_err(|e| format!("opening {}: {e}", device.name))?;

    let granted = stream.config().clone();
    let latency = stream.latency();

    eprintln!(
        "capturing from {} ({} ch @ {} Hz, {} frame buffer, analysing channel {})",
        device.name, channels, granted.sample_rate, granted.buffer_frames, options.channel
    );
    eprintln!(
        "hardware latency: {} frames in, {} safety ({:.2} ms round trip)",
        latency.input_frames,
        latency.safety_offset_frames,
        latency.round_trip_seconds(granted.sample_rate) * 1000.0
    );

    stream.start().map_err(|e| format!("starting: {e}"))?;

    let deadline = Instant::now() + Duration::from_secs_f64(options.seconds);
    let mut last_meter = Instant::now();
    let mut loudest = f32::NEG_INFINITY;

    while Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(20));
        if !engine.has_new_frame() {
            continue;
        }
        let frame = engine.latest();
        let level = broadband_db(&frame.bins);
        loudest = loudest.max(level);

        if options.meter && last_meter.elapsed() >= Duration::from_millis(100) {
            last_meter = Instant::now();
            let (bin, peak) = peak_bin(&frame.bins);
            eprint!(
                "\r  {level:7.1} dBFS {}  peak {:>8.1} Hz @ {peak:6.1} dB   ",
                bar(level),
                frame.bin_frequency(bin),
            );
            let _ = io::stderr().flush();
        }
    }

    stream.stop().map_err(|e| format!("stopping: {e}"))?;
    if options.meter {
        eprintln!();
    }

    let frame = engine.latest().clone();

    if frame.overruns > 0 {
        return Err(format!(
            "{} block(s) dropped during capture - the measurement is invalid",
            frame.overruns
        ));
    }
    if frame.frames_averaged == 0 {
        return Err("no audio was captured at all".into());
    }
    if loudest <= SILENCE_DB {
        return Err(format!(
            "captured only digital silence from {}.\n\
             A real microphone always has some self-noise, so this is almost certainly\n\
             macOS microphone permission being denied rather than a quiet room.\n\
             Check System Settings > Privacy & Security > Microphone for your terminal.",
            device.name
        ));
    }

    Ok(frame)
}

/// Total level across the spectrum, undoing the per-bin dB conversion.
fn broadband_db(bins: &[f32]) -> f32 {
    // write_db_fs stores 10·log10(2·power), so power is 10^(db/10)/2.
    let total: f32 = bins.iter().map(|db| 10.0_f32.powf(db / 10.0) / 2.0).sum();
    if total > 0.0 {
        10.0 * (2.0 * total).log10()
    } else {
        SILENCE_DB
    }
}

fn peak_bin(bins: &[f32]) -> (usize, f32) {
    bins.iter()
        .enumerate()
        .max_by(|a, b| a.1.total_cmp(b.1))
        .map(|(bin, level)| (bin, *level))
        .unwrap_or((0, SILENCE_DB))
}

/// A 40-column meter spanning -90 to 0 dBFS.
fn bar(db: f32) -> String {
    const WIDTH: usize = 40;
    let filled = (((db + 90.0) / 90.0).clamp(0.0, 1.0) * WIDTH as f32).round() as usize;
    let mut out = String::with_capacity(WIDTH + 2);
    out.push('[');
    for i in 0..WIDTH {
        out.push(if i < filled { '#' } else { '.' });
    }
    out.push(']');
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bar_spans_the_full_range() {
        assert!(bar(-200.0).ends_with("..]"));
        assert!(bar(-90.0).contains(".."));
        assert!(bar(0.0).contains("##"));
        // Clamps rather than overflowing.
        assert_eq!(bar(50.0).len(), bar(-50.0).len());
    }

    #[test]
    fn broadband_of_silence_is_the_floor() {
        let bins = vec![-200.0_f32; 128];
        assert!(broadband_db(&bins) < -100.0);
    }

    /// A single full-scale bin means a full-scale broadband level.
    #[test]
    fn broadband_of_one_full_scale_bin_is_zero_db() {
        let mut bins = vec![-200.0_f32; 128];
        bins[10] = 0.0;
        assert!(broadband_db(&bins).abs() < 0.01, "{}", broadband_db(&bins));
    }

    /// Two equal tones carry twice the power: +3 dB, not +6.
    #[test]
    fn two_equal_bins_add_three_decibels() {
        let mut bins = vec![-200.0_f32; 128];
        bins[10] = -20.0;
        bins[50] = -20.0;
        let total = broadband_db(&bins);
        assert!((total - -17.0).abs() < 0.01, "got {total}");
    }

    #[test]
    fn peak_bin_finds_the_loudest() {
        let mut bins = vec![-80.0_f32; 16];
        bins[7] = -12.0;
        assert_eq!(peak_bin(&bins), (7, -12.0));
    }

    #[test]
    fn peak_of_empty_does_not_panic() {
        assert_eq!(peak_bin(&[]).0, 0);
    }

    /// Enumeration must work without any capture permission.
    #[test]
    fn list_devices_succeeds() {
        let text = list_devices().expect("enumeration should work without permission");
        assert!(text.contains("Audio devices"));
        assert!(text.contains("uid:"));
    }
}
